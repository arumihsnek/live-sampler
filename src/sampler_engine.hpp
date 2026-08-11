#pragma once
#include "rt_queue.hpp"
#include "slot.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <rubberband/RubberBandStretcher.h>

struct MidiEvent final { uint32_t time = 0; uint8_t status = 0; uint8_t data1 = 0; uint8_t data2 = 0; };

struct Diagnostics final {
    std::atomic<uint64_t> invalid_bbt{0};
    std::atomic<uint64_t> capture_while_busy{0};
    std::atomic<uint64_t> empty_play{0};
    std::atomic<uint64_t> voice_exhausted{0};
    std::atomic<uint64_t> slot_active_capture{0};
    std::atomic<uint64_t> midi_overflow{0};
    std::atomic<uint64_t> finalize_overflow{0};
    std::atomic<uint64_t> publish_overflow{0};
    std::atomic<uint64_t> play_onsets{0};
    std::atomic<int64_t> last_onset_error_frames{0};
};

class SamplerEngine final {
public:
    SamplerEngine(double sample_rate, uint32_t quantum, double max_capture_seconds, uint32_t max_voices);
    ~SamplerEngine();
    SamplerEngine(const SamplerEngine&) = delete;
    SamplerEngine& operator=(const SamplerEngine&) = delete;

    void start_worker();
    void stop_worker();
    void process(uint32_t nframes, const float* in_left, const float* in_right,
                 float* out_left, float* out_right, const MidiEvent* events, std::size_t event_count,
                 bool valid_bbt, bool rolling, double bpm) noexcept;
    const Diagnostics& diagnostics() const noexcept { return diagnostics_; }
    Diagnostics& diagnostics() noexcept { return diagnostics_; }
    double beat_counter() const noexcept { return beat_counter_; }
    std::size_t slot_frames(int note) const noexcept;
    double slot_beats(int note) const noexcept;
    bool has_sample(int note) const noexcept;

private:
    struct FinalizeRequest { int slot; std::size_t frames; double beats; };
    struct Voice final {
        bool active = false;
        int slot = -1;
        uint8_t velocity = 0;
        SampleBuffer* sample = nullptr;
        std::size_t source_pos = 0;
        std::size_t pad_remaining = 0;
        std::size_t delay_remaining = 0;
        bool final_sent = false;
        std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
        std::unique_ptr<float[]> input_left;
        std::unique_ptr<float[]> input_right;
        std::unique_ptr<float[]> output_left;
        std::unique_ptr<float[]> output_right;
    };

    void drain_publish_queue() noexcept;
    void drain_deferred_retire_queue() noexcept;
    void drain_retire_queue();
    static void destroy_sample(SampleBuffer* sample) noexcept;
    bool publish_sample(SampleBuffer* sample) noexcept;
    void worker_loop();
    void handle_event(const MidiEvent& event, bool valid_bbt, bool rolling, double bpm) noexcept;
    void render_segment(std::size_t offset, std::size_t frames, const float* in_left, const float* in_right,
                        float* out_left, float* out_right, bool valid_bbt, bool rolling, double bpm) noexcept;
    void capture_segment(std::size_t offset, std::size_t frames, const float* in_left, const float* in_right) noexcept;
    void start_capture(int note, bool valid_bbt, bool rolling) noexcept;
    void stop_capture(int note) noexcept;
    void start_play(int note, uint8_t velocity, bool valid_bbt, bool rolling, double bpm) noexcept;
    void stop_play(int note) noexcept;
    bool retire_if_unused(SampleBuffer* sample) noexcept;
    bool enqueue_retirement(SampleBuffer* sample) noexcept;
    bool sample_referenced(SampleBuffer* sample) const noexcept;
    bool sample_voice_referenced(SampleBuffer* sample) const noexcept;
    void release_voice(Voice& voice) noexcept;
    void render_voice(Voice& voice, std::size_t frames, float* out_left, float* out_right, double bpm) noexcept;

    double sample_rate_;
    uint32_t quantum_;
    std::size_t capture_capacity_;
    uint32_t max_voices_;
    std::unique_ptr<float[]> capture_left_;
    std::unique_ptr<float[]> capture_right_;
    std::array<Slot, 128> slots_{};
    std::array<Voice, 8> voices_{};
    SpscQueue<FinalizeRequest, 8> finalize_queue_;
    SpscQueue<SampleBuffer*, 8> publish_queue_;
    SpscQueue<SampleBuffer*, 32> retire_queue_;
    std::array<SampleBuffer*, 256> deferred_retire_{};
    std::size_t deferred_retire_count_ = 0;
    SampleBuffer* blocked_publish_ = nullptr;
    std::atomic<bool> worker_stop_{false};
    std::thread worker_;
    bool capture_active_ = false;
    bool capture_pending_ = false;
    int capture_slot_ = -1;
    std::size_t capture_frames_ = 0;
    double capture_start_beat_ = 0.0;
    double beat_counter_ = 0.0;
    bool beat_initialized_ = false;
    Diagnostics diagnostics_;
};
