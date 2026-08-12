#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <string>
#include <thread>
#include <jack/jack.h>

struct Recorder final {
    jack_client_t* client = nullptr;
    jack_port_t* in_left = nullptr;
    jack_port_t* in_right = nullptr;
    std::string sampler_name = "live-sampler";
    std::atomic<uint64_t> first_nonzero{UINT64_MAX};
    std::atomic<uint64_t> first_callback_frame{UINT64_MAX};
    std::atomic<uint64_t> callbacks{0};
};

static int process(jack_nframes_t n, void* arg) noexcept {
    auto* r = static_cast<Recorder*>(arg);
    auto* l = static_cast<const float*>(jack_port_get_buffer(r->in_left, n));
    auto* rr = static_cast<const float*>(jack_port_get_buffer(r->in_right, n));
    jack_position_t pos{};
    jack_transport_query(r->client, &pos);
    uint64_t expected = UINT64_MAX;
    r->first_callback_frame.compare_exchange_strong(expected, pos.frame, std::memory_order_relaxed);
    r->callbacks.fetch_add(1, std::memory_order_relaxed);
    for (jack_nframes_t i = 0; i < n; ++i) {
        if (l[i] != 0.0F || rr[i] != 0.0F) {
            expected = UINT64_MAX;
            r->first_nonzero.compare_exchange_strong(expected, pos.frame + i, std::memory_order_relaxed);
            break;
        }
    }
    return 0;
}

int main() {
    Recorder r;
    if (const char* n = std::getenv("PILOT_SAMPLER_NAME")) r.sampler_name = n;
    jack_status_t status = JackServerFailed;
    r.client = jack_client_open("onset_acyclic_recorder", JackNoStartServer, &status);
    if (!r.client) return 1;
    r.in_left = jack_port_register(r.client, "audio_in_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    r.in_right = jack_port_register(r.client, "audio_in_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (!r.in_left || !r.in_right || jack_set_process_callback(r.client, process, &r) != 0 || jack_activate(r.client) != 0) return 2;
    for (int i = 0; i < 50; ++i) {
        const int l = jack_connect(r.client, (r.sampler_name + ":audio_out_1").c_str(), jack_port_name(r.in_left));
        const int rr = jack_connect(r.client, (r.sampler_name + ":audio_out_2").c_str(), jack_port_name(r.in_right));
        if ((l == 0 || l == 17) && (rr == 0 || rr == 17)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::seconds(30));
    jack_deactivate(r.client);
    std::cout << "RECORDER_FIRST_CALLBACK_FRAME=" << r.first_callback_frame.load(std::memory_order_relaxed)
              << " RECORDER_FIRST_NONZERO_FRAME=" << r.first_nonzero.load(std::memory_order_relaxed)
              << " CALLBACKS=" << r.callbacks.load(std::memory_order_relaxed) << "\n";
    jack_client_close(r.client);
    return 0;
}
