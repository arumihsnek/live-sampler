#include "sampler_engine.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
constexpr auto kOptions = RubberBand::RubberBandStretcher::OptionProcessRealTime |
                           RubberBand::RubberBandStretcher::OptionEngineFaster |
                           RubberBand::RubberBandStretcher::OptionThreadingNever |
                           RubberBand::RubberBandStretcher::OptionWindowShort |
                           RubberBand::RubberBandStretcher::OptionSmoothingOn |
                           RubberBand::RubberBandStretcher::OptionTransientsSmooth;
}

SamplerEngine::SamplerEngine(double sample_rate, uint32_t quantum, double max_capture_seconds, uint32_t max_voices)
    : sample_rate_(sample_rate), quantum_(quantum),
      capture_capacity_(static_cast<std::size_t>(sample_rate * max_capture_seconds)),
      max_voices_(std::min<uint32_t>(max_voices, 8U)),
      capture_left_(std::make_unique<float[]>(capture_capacity_)),
      capture_right_(std::make_unique<float[]>(capture_capacity_)) {
    if (!(sample_rate_ > 0.0) || quantum_ == 0 || capture_capacity_ == 0 || max_voices_ == 0) throw std::invalid_argument("invalid sampler configuration");
    for (int i = 0; i < 128; ++i) slots_[i].note = i;
    const std::size_t scratch = static_cast<std::size_t>(quantum_) * 4U + 8192U;
    for (uint32_t i = 0; i < max_voices_; ++i) {
        auto& v = voices_[i];
        v.stretcher = std::make_unique<RubberBand::RubberBandStretcher>(
            static_cast<size_t>(sample_rate_), 2, kOptions, 1.0, 1.0);
        v.stretcher->setMaxProcessSize(quantum_);
        v.stretcher->setPitchScale(1.0);
        v.input_left = std::make_unique<float[]>(scratch);
        v.input_right = std::make_unique<float[]>(scratch);
        v.output_left = std::make_unique<float[]>(scratch);
        v.output_right = std::make_unique<float[]>(scratch);
        if (v.stretcher->getEngineVersion() != 2) throw std::runtime_error("unexpected Rubber Band engine");
    }
}

SamplerEngine::~SamplerEngine() {
    stop_worker();
    assert(diagnostics_.runtime_sample_destructions.load(std::memory_order_acquire) == 0);
    for (auto& voice : voices_) {
        voice.active = false;
        voice.sample = nullptr;
        voice.slot = -1;
    }
    for (auto& slot : slots_) {
        destroy_sample(slot.current);
        slot.current = nullptr;
    }
    SampleBuffer* p = nullptr;
    while (publish_queue_.pop(p)) destroy_sample(p);
    for (std::size_t i = 0; i < retired_count_; ++i) destroy_sample(retired_samples_[i]);
    destroy_sample(blocked_publish_);
    destroy_sample(shutdown_pending_);
    blocked_publish_ = nullptr;
    shutdown_pending_ = nullptr;
    retired_count_ = 0;
}

void SamplerEngine::start_worker() {
    worker_stop_.store(false, std::memory_order_release);
    runtime_active_.store(true, std::memory_order_release);
    worker_ = std::thread(&SamplerEngine::worker_loop, this);
}

void SamplerEngine::stop_worker() {
    worker_stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    runtime_active_.store(false, std::memory_order_release);
}

void SamplerEngine::worker_loop() {
    SampleBuffer* pending = nullptr;
    while (!worker_stop_.load(std::memory_order_acquire)) {
        if (pending != nullptr) {
            if (publish_queue_.push(pending)) pending = nullptr;
        }
        FinalizeRequest req{};
        if (pending == nullptr && finalize_queue_.pop(req)) {
            auto* sample = new SampleBuffer;
            sample->left = new float[req.frames];
            sample->right = new float[req.frames];
            std::memcpy(sample->left, capture_left_.get(), req.frames * sizeof(float));
            std::memcpy(sample->right, capture_right_.get(), req.frames * sizeof(float));
            sample->frames = req.frames;
            sample->sample_rate = sample_rate_;
            sample->captured_beats = req.beats;
            sample->slot = req.slot;
            if (!publish_queue_.push(sample)) { pending = sample; diagnostics_.publish_overflow.fetch_add(1, std::memory_order_relaxed); }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (pending != nullptr) {
        // The owner is transferred to the post-join destructor. Never reclaim
        // a SampleBuffer from the worker while the runtime may still be live.
        shutdown_pending_ = pending;
    }
}

void SamplerEngine::destroy_sample(SampleBuffer* sample) noexcept {
    if (sample == nullptr) return;
    const bool runtime = runtime_active_.load(std::memory_order_acquire);
    if (runtime) {
        diagnostics_.runtime_sample_destructions.fetch_add(1, std::memory_order_relaxed);
        std::abort();
    }
    delete[] sample->left;
    delete[] sample->right;
    delete sample;
}

void SamplerEngine::drain_publish_queue() noexcept {
    if (blocked_publish_ != nullptr) {
        if (!publish_sample(blocked_publish_)) return;
        blocked_publish_ = nullptr;
    }
    SampleBuffer* sample = nullptr;
    while (publish_queue_.pop(sample)) {
        if (!publish_sample(sample)) {
            blocked_publish_ = sample;
            return;
        }
    }
}

bool SamplerEngine::publish_sample(SampleBuffer* sample) noexcept {
    const int note = sample->slot;
    if (note < 0 || note >= 128) return retire_if_unused(sample);
    auto* old = slots_[note].current;
    if (old != nullptr) {
        // Do not remove the old slot reference until retirement has an
        // explicit destination. This transaction prevents losing old when
        // both bounded retirement stores are full.
        if (sample_voice_referenced(old)) return false;
        // Remove the slot's retaining reference before making old visible to
        // the worker. If enqueue fails, no worker saw old and rollback is safe.
        slots_[note].current = nullptr;
        if (!enqueue_retirement(old)) {
            diagnostics_.retirement_store_full.fetch_add(1, std::memory_order_relaxed);
            slots_[note].current = old;
            return false;
        }
    }
    slots_[note].current = sample;
    diagnostics_.slot_commit.fetch_add(1, std::memory_order_relaxed);
    capture_pending_ = false;
    return true;
}

bool SamplerEngine::sample_referenced(SampleBuffer* sample) const noexcept {
    if (sample == nullptr) return false;
    // A slot retains ownership of its current sample independently of voices.
    // A voice may release its borrow while the slot still publishes the same
    // immutable buffer; that buffer must not enter the retire queue yet.
    for (const auto& slot : slots_) if (slot.current == sample) return true;
    for (const auto& v : voices_) if (v.active && v.sample == sample) return true;
    return false;
}

bool SamplerEngine::sample_voice_referenced(SampleBuffer* sample) const noexcept {
    if (sample == nullptr) return false;
    for (const auto& v : voices_) if (v.active && v.sample == sample) return true;
    return false;
}

bool SamplerEngine::enqueue_retirement(SampleBuffer* sample) noexcept {
    if (sample == nullptr || sample->retire_queued) return true;
    if (retired_count_ >= retired_samples_.size()) return false;
    retired_samples_[retired_count_++] = sample;
    sample->retire_queued = true;
    return true;
}

bool SamplerEngine::retire_if_unused(SampleBuffer* sample) noexcept {
    if (sample == nullptr || sample->retire_queued || sample_referenced(sample)) return true;
    return enqueue_retirement(sample);
}

void SamplerEngine::release_voice(Voice& voice) noexcept {
    SampleBuffer* old = voice.sample;
    voice.active = false;
    voice.slot = -1;
    voice.sample = nullptr;
    voice.source_pos = 0;
    voice.final_sent = false;
    retire_if_unused(old);
}

void SamplerEngine::start_capture(int note, bool valid_bbt, bool rolling) noexcept {
    if (!valid_bbt || !rolling) { diagnostics_.invalid_bbt.fetch_add(1, std::memory_order_relaxed); return; }
    if (capture_active_ || capture_pending_) { diagnostics_.capture_while_busy.fetch_add(1, std::memory_order_relaxed); return; }
    if (note < 0 || note >= 128) return;
    if (sample_voice_referenced(slots_[note].current)) { diagnostics_.slot_active_capture.fetch_add(1, std::memory_order_relaxed); return; }
    capture_active_ = true;
    capture_slot_ = note;
    capture_frames_ = 0;
    capture_start_beat_ = beat_counter_;
    diagnostics_.capture_start.fetch_add(1, std::memory_order_relaxed);
}

void SamplerEngine::stop_capture(int note) noexcept {
    if (!capture_active_ || note != capture_slot_) return;
    const double beats = std::max(0.0, beat_counter_ - capture_start_beat_);
    FinalizeRequest req{capture_slot_, capture_frames_, beats};
    if (!finalize_queue_.push(req)) {
        diagnostics_.finalize_overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    capture_active_ = false;
    capture_pending_ = true;
    capture_slot_ = -1;
    diagnostics_.capture_stop.fetch_add(1, std::memory_order_relaxed);
}

void SamplerEngine::start_play(int note, uint8_t velocity, bool valid_bbt, bool rolling, double bpm) noexcept {
    if (!valid_bbt || !rolling || !(bpm > 0.0)) { diagnostics_.invalid_bbt.fetch_add(1, std::memory_order_relaxed); return; }
    if (note < 0 || note >= 128 || slots_[note].current == nullptr) { diagnostics_.empty_play.fetch_add(1, std::memory_order_relaxed); return; }
    for (auto& v : voices_) if (v.active && v.slot == note) release_voice(v);
    Voice* chosen = nullptr;
    for (uint32_t i = 0; i < max_voices_; ++i) if (!voices_[i].active) { chosen = &voices_[i]; break; }
    if (chosen == nullptr) { diagnostics_.voice_exhausted.fetch_add(1, std::memory_order_relaxed); return; }
    chosen->active = true;
    chosen->slot = note;
    chosen->velocity = velocity;
    chosen->sample = slots_[note].current;
    diagnostics_.play_start.fetch_add(1, std::memory_order_relaxed);
    chosen->source_pos = 0;
    chosen->final_sent = false;
    chosen->stretcher->reset();
    const double ratio = elastic_time_ratio(chosen->sample->captured_beats, bpm, chosen->sample->frames, sample_rate_);
    chosen->stretcher->setTimeRatio(ratio > 0.0 ? ratio : 1.0);
    chosen->stretcher->setPitchScale(1.0);
    chosen->pad_remaining = chosen->stretcher->getPreferredStartPad();
    chosen->delay_remaining = chosen->stretcher->getStartDelay();
    diagnostics_.play_onsets.fetch_add(1, std::memory_order_relaxed);
    diagnostics_.last_onset_error_frames.store(static_cast<int64_t>(chosen->delay_remaining), std::memory_order_relaxed);
}

void SamplerEngine::stop_play(int note) noexcept {
    for (auto& v : voices_) if (v.active && v.slot == note) {
        diagnostics_.play_stop.fetch_add(1, std::memory_order_relaxed);
        release_voice(v);
    }
}

void SamplerEngine::handle_event(const MidiEvent& event, bool valid_bbt, bool rolling, double bpm) noexcept {
    const uint8_t type = event.status & 0xF0U;
    const int channel = static_cast<int>(event.status & 0x0FU);
    const int note = static_cast<int>(event.data1 & 0x7FU);
    if (type != 0x80U && type != 0x90U) return;
    const bool on = type == 0x90U && event.data2 != 0;
    if (channel == 0) {
        if (on) diagnostics_.midi_note_on_ch0.fetch_add(1, std::memory_order_relaxed);
        else diagnostics_.midi_note_off_ch0.fetch_add(1, std::memory_order_relaxed);
        if (on) start_capture(note, valid_bbt, rolling); else stop_capture(note);
    } else if (channel == 1) {
        if (on) diagnostics_.midi_note_on_ch1.fetch_add(1, std::memory_order_relaxed);
        else diagnostics_.midi_note_off_ch1.fetch_add(1, std::memory_order_relaxed);
        if (on) start_play(note, event.data2, valid_bbt, rolling, bpm); else stop_play(note);
    }
}

void SamplerEngine::capture_segment(std::size_t offset, std::size_t frames, const float* in_left, const float* in_right) noexcept {
    if (!capture_active_ || frames == 0) return;
    const std::size_t copy = std::min(frames, capture_capacity_ - capture_frames_);
    if (copy == 0) return;
    std::memcpy(capture_left_.get() + capture_frames_, in_left + offset, copy * sizeof(float));
    std::memcpy(capture_right_.get() + capture_frames_, in_right + offset, copy * sizeof(float));
    capture_frames_ += copy;
}

void SamplerEngine::render_voice(Voice& v, std::size_t frames, float* out_left, float* out_right, double bpm) noexcept {
    if (!v.active || v.sample == nullptr || frames == 0) return;
    const double ratio = elastic_time_ratio(v.sample->captured_beats, bpm, v.sample->frames, sample_rate_);
    if (ratio > 0.0) v.stretcher->setTimeRatio(ratio);
    const float gain = static_cast<float>(v.velocity) / 127.0F;
    std::size_t produced = 0;
    for (int guard = 0; guard < 8 && produced < frames && v.active; ++guard) {
        const int available = v.stretcher->available();
        if (available > 0) {
            const std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(available), frames - produced);
            float* output[2] = {v.output_left.get(), v.output_right.get()};
            const std::size_t got = v.stretcher->retrieve(output, want);
            for (std::size_t i = 0; i < got; ++i) {
                if (v.delay_remaining > 0) { --v.delay_remaining; continue; }
                out_left[produced + i] += v.output_left[i] * gain;
                out_right[produced + i] += v.output_right[i] * gain;
            }
            produced += got;
            continue;
        }
        if (v.final_sent) { if (available < 0) release_voice(v); break; }
        std::size_t in_count = 0;
        bool final_block = false;
        while (in_count < quantum_) {
            if (v.pad_remaining > 0) {
                v.input_left[in_count] = 0.0F; v.input_right[in_count] = 0.0F;
                --v.pad_remaining; ++in_count; continue;
            }
            if (v.source_pos < v.sample->frames) {
                v.input_left[in_count] = v.sample->left[v.source_pos];
                v.input_right[in_count] = v.sample->right[v.source_pos];
                ++v.source_pos; ++in_count; continue;
            }
            final_block = true;
            break;
        }
        if (in_count > 0) {
            const float* const input[2] = {v.input_left.get(), v.input_right.get()};
            v.stretcher->process(input, in_count, final_block);
            if (final_block) v.final_sent = true;
        } else if (final_block) {
            v.stretcher->process(nullptr, 0, true);
            v.final_sent = true;
        } else break;
    }
}

void SamplerEngine::render_segment(std::size_t offset, std::size_t frames, const float* in_left, const float* in_right,
                                    float* out_left, float* out_right, bool valid_bbt, bool rolling, double bpm) noexcept {
    if (frames == 0) return;
    if (valid_bbt && rolling && bpm > 0.0) {
        if (!beat_initialized_) { beat_counter_ = 0.0; beat_initialized_ = true; }
        const double increment = static_cast<double>(frames) * bpm / (sample_rate_ * 60.0);
        capture_segment(offset, frames, in_left, in_right);
        beat_counter_ += increment;
        for (uint32_t i = 0; i < max_voices_; ++i) render_voice(voices_[i], frames, out_left, out_right, bpm);
    } else {
        if (capture_active_) diagnostics_.invalid_bbt.fetch_add(1, std::memory_order_relaxed);
        for (uint32_t i = 0; i < max_voices_; ++i) if (voices_[i].active) release_voice(voices_[i]);
    }
}

void SamplerEngine::process(uint32_t nframes, const float* in_left, const float* in_right,
                            float* out_left, float* out_right, const MidiEvent* events, std::size_t event_count,
                            bool valid_bbt, bool rolling, double bpm) noexcept {
    assert(diagnostics_.runtime_sample_destructions.load(std::memory_order_acquire) == 0);
    if (valid_bbt && rolling && bpm > 0.0) {
        diagnostics_.bbt_valid_callbacks.fetch_add(1, std::memory_order_relaxed);
        const int64_t bpm_milli = static_cast<int64_t>(bpm * 1000.0);
        if (previous_valid_bpm_milli_ != 0 && previous_valid_bpm_milli_ != bpm_milli) {
            diagnostics_.bbt_bpm_changes.fetch_add(1, std::memory_order_relaxed);
        }
        previous_valid_bpm_milli_ = bpm_milli;
        diagnostics_.last_bpm_milli.store(bpm_milli, std::memory_order_relaxed);
    }
    drain_publish_queue();
    std::memset(out_left, 0, nframes * sizeof(float));
    std::memset(out_right, 0, nframes * sizeof(float));
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < event_count; ++i) {
        const std::size_t t = std::min<std::size_t>(events[i].time, nframes);
        if (t < cursor) continue;
        render_segment(cursor, t - cursor, in_left, in_right, out_left, out_right, valid_bbt, rolling, bpm);
        handle_event(events[i], valid_bbt, rolling, bpm);
        cursor = t;
    }
    render_segment(cursor, nframes - cursor, in_left, in_right, out_left, out_right, valid_bbt, rolling, bpm);
}

std::size_t SamplerEngine::slot_frames(int note) const noexcept { return (note >= 0 && note < 128 && slots_[note].current) ? slots_[note].current->frames : 0; }
double SamplerEngine::slot_beats(int note) const noexcept { return (note >= 0 && note < 128 && slots_[note].current) ? slots_[note].current->captured_beats : 0.0; }
bool SamplerEngine::has_sample(int note) const noexcept { return slot_frames(note) != 0; }
