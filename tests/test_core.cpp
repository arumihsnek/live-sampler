#include "sampler_engine.hpp"
#include "slot.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

static void expect(bool value, const char* what) { if (!value) { std::cerr << "FAIL: " << what << '\n'; std::exit(1); } }
static double zero_cross_pitch(const std::vector<float>& x, double sr) {
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < x.size(); ++i) if (x[i - 1] <= 0.0F && x[i] > 0.0F) ++crossings;
    return static_cast<double>(crossings) * sr / static_cast<double>(x.size());
}
static void test_ratio() {
    expect(std::abs(elastic_time_ratio(4.0, 60.0, 96000, 48000.0) - 2.0) < 1e-12, "ratio 60");
    expect(std::abs(elastic_time_ratio(4.0, 120.0, 96000, 48000.0) - 1.0) < 1e-12, "ratio 120");
    expect(std::abs(elastic_time_ratio(4.0, 240.0, 96000, 48000.0) - 0.5) < 1e-12, "ratio 240");
}
static void test_slot_replace() {
    Slot slot{36, nullptr};
    auto* a = new SampleBuffer; a->slot = 36; a->frames = 10;
    auto* b = new SampleBuffer; b->slot = 36; b->frames = 20;
    slot.current = a; slot.current = b;
    expect(slot.note == 36 && slot.current == b && slot.current->slot == 36, "slot identity across replace");
    delete a; delete b;
}
static void test_midi_and_capture_boundary() {
    SamplerEngine engine(48000.0, 128, 1.0, 8);
    engine.start_worker();
    float in_l[128]{}; float in_r[128]{}; float out_l[128]{}; float out_r[128]{};
    MidiEvent events[2]{{17, 0x90, 36, 100}, {83, 0x80, 36, 0}};
    engine.process(128, in_l, in_r, out_l, out_r, events, 2, true, true, 120.0);
    for (int i = 0; i < 100 && !engine.has_sample(36); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); engine.process(128, in_l, in_r, out_l, out_r, nullptr, 0, true, true, 120.0); }
    expect(engine.has_sample(36), "capture finalized");
    expect(engine.slot_frames(36) == 66, "event.time frame boundary");
    engine.stop_worker();
}
static void test_publish_play_stop_destroy_lifetime() {
    SamplerEngine engine(48000.0, 128, 1.0, 8);
    engine.start_worker();
    float in_l[128]{}; float in_r[128]{}; float out_l[128]{}; float out_r[128]{};
    MidiEvent capture[2]{{0, 0x90, 36, 100}, {64, 0x80, 36, 0}};
    engine.process(128, in_l, in_r, out_l, out_r, capture, 2, true, true, 120.0);
    for (int i = 0; i < 100 && !engine.has_sample(36); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        engine.process(128, in_l, in_r, out_l, out_r, nullptr, 0, true, true, 120.0);
    }
    expect(engine.has_sample(36), "lifetime sample published");
    MidiEvent play_on[1]{{0, 0x90, 36, 127}};
    engine.process(128, in_l, in_r, out_l, out_r, play_on, 1, true, true, 120.0);
    MidiEvent play_off[1]{{0, 0x80, 36, 0}};
    engine.process(128, in_l, in_r, out_l, out_r, play_off, 1, true, true, 120.0);
    engine.stop_worker();
    // Destructor must not free the slot-current sample through retire_queue_
    // and then free the same pointer again through slots_.
}

static void test_stretch() {
    constexpr double sr = 48000.0; constexpr std::size_t input_frames = 96000; constexpr double hz = 440.0;
    std::vector<float> left(input_frames), right(input_frames);
    for (std::size_t i = 0; i < input_frames; ++i) left[i] = right[i] = 0.2F * std::sin(2.0 * M_PI * hz * static_cast<double>(i) / sr);
    for (double ratio : {2.0, 1.0, 0.5}) {
        RubberBand::RubberBandStretcher s(static_cast<size_t>(sr), 2,
            RubberBand::RubberBandStretcher::OptionProcessOffline |
            RubberBand::RubberBandStretcher::OptionEngineFaster |
            RubberBand::RubberBandStretcher::OptionThreadingNever, ratio, 1.0);
        s.setExpectedInputDuration(input_frames); s.setMaxProcessSize(1024); s.setPitchScale(1.0);
        const float* in[2] = {left.data(), right.data()}; s.study(in, input_frames, true);
        std::vector<float> out_l(static_cast<std::size_t>(input_frames * ratio + 20000));
        std::vector<float> out_r(out_l.size());
        std::size_t produced = 0;
        for (std::size_t p = 0; p < input_frames; p += 1024) {
            const std::size_t n = std::min<std::size_t>(1024, input_frames - p);
            const float* block[2] = {left.data() + p, right.data() + p};
            s.process(block, n, p + n == input_frames);
            const int available = s.available();
            if (available <= 0) continue;
            const std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(available), out_l.size() - produced);
            float* dest[2] = {out_l.data() + produced, out_r.data() + produced};
            produced += s.retrieve(dest, want);
        }
        while (s.available() > 0 && produced < out_l.size()) {
            const std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(s.available()), out_l.size() - produced);
            float* dest[2] = {out_l.data() + produced, out_r.data() + produced};
            produced += s.retrieve(dest, want);
        }
        const double expected = input_frames * ratio;
        expect(std::abs(static_cast<double>(produced) - expected) < 2500.0, "stretch duration");
        const std::vector<float> pitch_sample(out_l.begin() + std::min<std::size_t>(2000, produced / 4), out_l.begin() + std::min<std::size_t>(produced, 20000));
        expect(std::abs(zero_cross_pitch(pitch_sample, sr) - hz) < 8.0, "stretch pitch");
    }
}
int main() { test_ratio(); test_slot_replace(); test_midi_and_capture_boundary(); test_publish_play_stop_destroy_lifetime(); test_stretch(); std::cout << "PASS core ratio slot midi capture stretch lifetime\n"; }
