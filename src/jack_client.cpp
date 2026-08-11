#include "jack_client.hpp"
#include <jack/midiport.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

JackClient::JackClient(const std::string& name, double max_capture_seconds, uint32_t voices,
                       int rec_channel, int play_channel)
    : rec_channel_(rec_channel - 1), play_channel_(play_channel - 1) {
    jack_status_t status = JackServerFailed;
    client_ = jack_client_open(name.c_str(), JackNoStartServer, &status);
    if (client_ == nullptr) throw std::runtime_error("jack_client_open failed (is the JACK/PipeWire server running?)");
    in_left_ = jack_port_register(client_, "audio_in_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    in_right_ = jack_port_register(client_, "audio_in_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    out_left_ = jack_port_register(client_, "audio_out_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    out_right_ = jack_port_register(client_, "audio_out_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    midi_in_ = jack_port_register(client_, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    if (!in_left_ || !in_right_ || !out_left_ || !out_right_ || !midi_in_) throw std::runtime_error("JACK port registration failed");
    engine_ = std::make_unique<SamplerEngine>(jack_get_sample_rate(client_), jack_get_buffer_size(client_), max_capture_seconds, voices);
    if (jack_set_process_callback(client_, &JackClient::process_cb, this) != 0) throw std::runtime_error("jack_set_process_callback failed");
    jack_on_shutdown(client_, &JackClient::shutdown_cb, this);
}

JackClient::~JackClient() {
    stop_.store(true, std::memory_order_release);
    if (client_ != nullptr) {
        jack_deactivate(client_);
        jack_client_close(client_);
        client_ = nullptr;
    }
}

bool JackClient::activate() {
    engine_->start_worker();
    return jack_activate(client_) == 0;
}

void JackClient::request_stop() noexcept { stop_.store(true, std::memory_order_release); }

void JackClient::run_until_stopped() {
    while (!stop_.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

void JackClient::print_diagnostics() const {
    const auto& d = engine_->diagnostics();
    std::cout << "diagnostics invalid_bbt=" << d.invalid_bbt.load() << " capture_while_busy=" << d.capture_while_busy.load()
              << " empty_play=" << d.empty_play.load() << " voice_exhausted=" << d.voice_exhausted.load()
              << " slot_active_capture=" << d.slot_active_capture.load() << " finalize_overflow=" << d.finalize_overflow.load()
              << " publish_overflow=" << d.publish_overflow.load() << " onset_delay_frames=" << d.last_onset_error_frames.load() << '\n';
}

int JackClient::process_cb(jack_nframes_t nframes, void* arg) noexcept { return static_cast<JackClient*>(arg)->process(nframes); }
void JackClient::shutdown_cb(void* arg) noexcept { static_cast<JackClient*>(arg)->request_stop(); }

int JackClient::process(jack_nframes_t nframes) noexcept {
    auto* in_l = static_cast<const float*>(jack_port_get_buffer(in_left_, nframes));
    auto* in_r = static_cast<const float*>(jack_port_get_buffer(in_right_, nframes));
    auto* out_l = static_cast<float*>(jack_port_get_buffer(out_left_, nframes));
    auto* out_r = static_cast<float*>(jack_port_get_buffer(out_right_, nframes));
    void* midi = jack_port_get_buffer(midi_in_, nframes);
    MidiEvent events[256]{};
    const uint32_t count = jack_midi_get_event_count(midi);
    std::size_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        jack_midi_event_t ev{};
        if (jack_midi_event_get(&ev, midi, i) != 0 || ev.size < 3 || used >= 256) {
            engine_->diagnostics().midi_overflow.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        events[used].time = ev.time;
        events[used].status = ev.buffer[0];
        events[used].data1 = ev.buffer[1];
        events[used].data2 = ev.buffer[2];
        const int ch = events[used].status & 0x0F;
        if (ch == rec_channel_) events[used].status = static_cast<uint8_t>((events[used].status & 0xF0U) | 0U);
        else if (ch == play_channel_) events[used].status = static_cast<uint8_t>((events[used].status & 0xF0U) | 1U);
        ++used;
    }
    jack_position_t pos{};
    const jack_transport_state_t state = jack_transport_query(client_, &pos);
    const bool valid_bbt = (pos.valid & JackPositionBBT) != 0 && pos.beats_per_minute > 0.0;
    const bool rolling = state == JackTransportRolling || state == JackTransportStarting || state == JackTransportLooping;
    engine_->process(nframes, in_l, in_r, out_l, out_r, events, used, valid_bbt, rolling, pos.beats_per_minute);
    return 0;
}
