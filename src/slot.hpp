#pragma once
#include <cstddef>

struct SampleBuffer final {
    float* left = nullptr;
    float* right = nullptr;
    std::size_t frames = 0;
    double sample_rate = 0.0;
    double captured_beats = 0.0;
    int slot = -1;
    bool retire_queued = false;
    bool retire_deferred = false;
};

struct Slot final {
    int note = 0;
    SampleBuffer* current = nullptr;
};

inline double elastic_time_ratio(double captured_beats, double current_bpm,
                                 std::size_t sample_frames, double sample_rate) noexcept {
    if (!(captured_beats > 0.0) || !(current_bpm > 0.0) || sample_frames == 0 || !(sample_rate > 0.0)) return 0.0;
    const double target_seconds = captured_beats * 60.0 / current_bpm;
    const double physical_seconds = static_cast<double>(sample_frames) / sample_rate;
    return target_seconds / physical_seconds;
}
