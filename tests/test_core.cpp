#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#define private public
#include "sampler_engine.hpp"
#undef private

static void expect(bool value, const char* what) {
    if (!value) { std::cerr << "FAIL: " << what << '\n'; std::exit(1); }
}

static double zero_cross_pitch(const std::vector<float>& x, double sr) {
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < x.size(); ++i) if (x[i - 1] <= 0.0F && x[i] > 0.0F) ++crossings;
    return static_cast<double>(crossings) * sr / static_cast<double>(x.size());
}

static constexpr int kSlot = 36;
static constexpr std::size_t kCaptureBlocks = 64;
static constexpr std::size_t kQuantum = 128;
static constexpr double kSampleRate = 48000.0;

static void fill_tone(std::array<float, kQuantum>& left, std::array<float, kQuantum>& right,
                      double hz, std::size_t phase) {
    for (std::size_t i = 0; i < kQuantum; ++i) {
        const float value = static_cast<float>(0.25 * std::sin(
            2.0 * 3.14159265358979323846 * hz * static_cast<double>(phase + i) / kSampleRate));
        left[i] = value;
        right[i] = value;
    }
}

static void pump_until_commit(SamplerEngine& engine, uint64_t target) {
    std::array<float, kQuantum> in{};
    std::array<float, kQuantum> out{};
    for (int i = 0; i < 250; ++i) {
        if (engine.diagnostics().slot_commit.load() >= target) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        engine.process(kQuantum, in.data(), in.data(), out.data(), out.data(), nullptr, 0, true, true, 120.0);
    }
    expect(engine.diagnostics().slot_commit.load() >= target, "slot commit published");
}

static void capture_tone(SamplerEngine& engine, double hz, uint64_t expected_commit) {
    std::array<float, kQuantum> in_l{};
    std::array<float, kQuantum> in_r{};
    std::array<float, kQuantum> out_l{};
    std::array<float, kQuantum> out_r{};
    std::size_t phase = 0;
    MidiEvent rec_on{0, 0x90, kSlot, 100};
    fill_tone(in_l, in_r, hz, phase);
    engine.process(kQuantum, in_l.data(), in_r.data(), out_l.data(), out_r.data(), &rec_on, 1, true, true, 120.0);
    phase += kQuantum;
    for (std::size_t block = 1; block < kCaptureBlocks; ++block) {
        fill_tone(in_l, in_r, hz, phase);
        engine.process(kQuantum, in_l.data(), in_r.data(), out_l.data(), out_r.data(), nullptr, 0, true, true, 120.0);
        phase += kQuantum;
    }
    MidiEvent rec_off{0, 0x80, kSlot, 0};
    engine.process(kQuantum, in_l.data(), in_r.data(), out_l.data(), out_r.data(), &rec_off, 1, true, true, 120.0);
    pump_until_commit(engine, expected_commit);
}

static double sample_pitch(const SampleBuffer& sample) {
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < sample.frames; ++i) {
        if (sample.left[i - 1] <= 0.0F && sample.left[i] > 0.0F) ++crossings;
    }
    return static_cast<double>(crossings) * sample.sample_rate / static_cast<double>(sample.frames);
}

static void test_ratio() {
    expect(std::abs(elastic_time_ratio(4.0, 60.0, 96000, 48000.0) - 2.0) < 1e-12, "ratio 60");
    expect(std::abs(elastic_time_ratio(4.0, 120.0, 96000, 48000.0) - 1.0) < 1e-12, "ratio 120");
    expect(std::abs(elastic_time_ratio(4.0, 240.0, 96000, 48000.0) - 0.5) < 1e-12, "ratio 240");
}

static void test_midi_and_capture_boundary() {
    SamplerEngine engine(48000.0, 128, 1.0, 8);
    engine.start_worker();
    float in_l[128]{}; float in_r[128]{}; float out_l[128]{}; float out_r[128]{};
    MidiEvent events[2]{{17, 0x90, 36, 100}, {83, 0x80, 36, 0}};
    engine.process(128, in_l, in_r, out_l, out_r, events, 2, true, true, 120.0);
    for (int i = 0; i < 100 && !engine.has_sample(36); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        engine.process(128, in_l, in_r, out_l, out_r, nullptr, 0, true, true, 120.0);
    }
    expect(engine.has_sample(36), "capture finalized");
    expect(engine.slot_frames(36) == 66, "event.time frame boundary");
    engine.stop_worker();
}

static void test_real_slot_replace() {
    SamplerEngine engine(kSampleRate, kQuantum, 1.0, 8);
    engine.start_worker();
    capture_tone(engine, 440.0, 1);
    SampleBuffer* a = engine.slots_[kSlot].current;
    expect(a != nullptr && std::abs(sample_pitch(*a) - 440.0) < 30.0, "slot36 contains distinguishable A tone");
    capture_tone(engine, 660.0, 2);
    SampleBuffer* b = engine.slots_[kSlot].current;
    expect(b != nullptr && b != a, "slot36 contains a new B buffer");
    expect(std::abs(sample_pitch(*b) - 660.0) < 30.0, "slot36 contains distinguishable B tone");
    expect(engine.retired_count_ == 1 && engine.retired_samples_[0] == a, "A moved to retired storage");
    expect(engine.diagnostics().runtime_sample_destructions.load() == 0, "replace has no runtime destruction");
    expect(engine.diagnostics().slot_commit.load() == 2, "real replace commits twice");
    std::cout << "REAL_SLOT_REPLACE_TEST PASS A_HZ=" << sample_pitch(*a) << " B_HZ=" << sample_pitch(*b)
              << " slot_commit=" << engine.diagnostics().slot_commit.load() << " retired=1 runtime_sample_destructions="
              << engine.diagnostics().runtime_sample_destructions.load() << "\n";
    engine.stop_worker();
}

static void test_publish_play_stop_destroy_lifetime() {
    SamplerEngine engine(kSampleRate, kQuantum, 1.0, 8);
    engine.start_worker();
    capture_tone(engine, 440.0, 1);
    std::array<float, kQuantum> io{};
    MidiEvent play_on{0, 0x91, kSlot, 127};
    engine.process(kQuantum, io.data(), io.data(), io.data(), io.data(), &play_on, 1, true, true, 120.0);
    expect(engine.diagnostics().play_start.load() == 1, "PLAY NoteOn starts a voice");
    expect(engine.voices_[0].active && engine.voices_[0].sample == engine.slots_[kSlot].current,
           "PLAY voice borrows slot.current");
    MidiEvent play_off{0, 0x81, kSlot, 0};
    engine.process(kQuantum, io.data(), io.data(), io.data(), io.data(), &play_off, 1, true, true, 120.0);
    expect(engine.diagnostics().play_stop.load() == 1, "PLAY NoteOff stops the voice");
    expect(engine.diagnostics().runtime_sample_destructions.load() == 0, "play lifetime has no runtime destruction");
    std::cout << "PLAY_LIFETIME_TEST PASS play_start=" << engine.diagnostics().play_start.load()
              << " play_stop=" << engine.diagnostics().play_stop.load() << " runtime_sample_destructions="
              << engine.diagnostics().runtime_sample_destructions.load() << "\n";
    engine.stop_worker();
}

static void test_replace_while_playing_rejected() {
    SamplerEngine engine(kSampleRate, kQuantum, 1.0, 8);
    engine.start_worker();
    capture_tone(engine, 440.0, 1);
    SampleBuffer* a = engine.slots_[kSlot].current;
    std::array<float, kQuantum> io{};
    MidiEvent play_on{0, 0x91, kSlot, 127};
    engine.process(kQuantum, io.data(), io.data(), io.data(), io.data(), &play_on, 1, true, true, 120.0);
    const auto captures_before = engine.diagnostics().capture_start.load();
    MidiEvent rec_on{0, 0x90, kSlot, 100};
    engine.process(kQuantum, io.data(), io.data(), io.data(), io.data(), &rec_on, 1, true, true, 120.0);
    expect(engine.diagnostics().slot_active_capture.load() == 1, "active-slot REC is rejected");
    expect(engine.diagnostics().capture_start.load() == captures_before, "rejected REC does not start capture");
    expect(engine.slots_[kSlot].current == a, "active-slot REC retains A");
    expect(engine.diagnostics().runtime_sample_destructions.load() == 0, "rejected REC has no runtime destruction");
    MidiEvent play_off{0, 0x81, kSlot, 0};
    engine.process(kQuantum, io.data(), io.data(), io.data(), io.data(), &play_off, 1, true, true, 120.0);
    capture_tone(engine, 660.0, 2);
    expect(engine.slots_[kSlot].current != a, "REC after PLAY OFF starts replacement");
    expect(engine.diagnostics().capture_start.load() == captures_before + 1, "inactive-slot REC starts capture");
    expect(engine.diagnostics().runtime_sample_destructions.load() == 0, "safe replacement has no runtime destruction");
    std::cout << "REPLACE_WHILE_PLAYING_REJECT PASS slot_active_capture="
              << engine.diagnostics().slot_active_capture.load() << " capture_start="
              << engine.diagnostics().capture_start.load() << " runtime_sample_destructions="
              << engine.diagnostics().runtime_sample_destructions.load() << "\n";
    engine.stop_worker();
}

static void test_dynamic_bpm_diagnostic() {
    SamplerEngine engine(kSampleRate, kQuantum, 1.0, 8);
    std::array<float, kQuantum> io{};
    engine.process(kQuantum, io.data(), io.data(), io.data(), io.data(), nullptr, 0, true, true, 120.0);
    engine.process(kQuantum, io.data(), io.data(), io.data(), io.data(), nullptr, 0, true, true, 90.0);
    engine.process(kQuantum, io.data(), io.data(), io.data(), io.data(), nullptr, 0, true, true, 150.0);
    expect(engine.diagnostics().bbt_bpm_changes.load() >= 2, "dynamic BPM counts two valid changes");
    expect(engine.diagnostics().last_bpm_milli.load() == 150000, "dynamic BPM ends at 150000 milli-BPM");
    std::cout << "BBT_BPM_TEST PASS bbt_bpm_changes=" << engine.diagnostics().bbt_bpm_changes.load()
              << " last_bpm_milli=" << engine.diagnostics().last_bpm_milli.load() << "\n";
}

static void test_stretch() {
    constexpr double sr = 48000.0; constexpr std::size_t input_frames = 96000; constexpr double hz = 440.0;
    std::vector<float> left(input_frames), right(input_frames);
    for (std::size_t i = 0; i < input_frames; ++i) left[i] = right[i] = 0.2F * std::sin(2.0 * 3.14159265358979323846 * hz * static_cast<double>(i) / sr);
    for (double ratio : {2.0, 1.0, 0.5}) {
        RubberBand::RubberBandStretcher s(static_cast<size_t>(sr), 2,
            RubberBand::RubberBandStretcher::OptionProcessOffline |
            RubberBand::RubberBandStretcher::OptionEngineFaster |
            RubberBand::RubberBandStretcher::OptionThreadingNever, ratio, 1.0);
        s.setExpectedInputDuration(input_frames); s.setMaxProcessSize(1024); s.setPitchScale(1.0);
        const float* in[2] = {left.data(), right.data()}; s.study(in, input_frames, true);
        std::vector<float> out_l(static_cast<std::size_t>(input_frames * ratio + 20000));
        std::vector<float> out_r(out_l.size()); std::size_t produced = 0;
        for (std::size_t p = 0; p < input_frames; p += 1024) {
            const std::size_t n = std::min<std::size_t>(1024, input_frames - p);
            const float* block[2] = {left.data() + p, right.data() + p}; s.process(block, n, p + n == input_frames);
            const int available = s.available(); if (available <= 0) continue;
            const std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(available), out_l.size() - produced);
            float* dest[2] = {out_l.data() + produced, out_r.data() + produced}; produced += s.retrieve(dest, want);
        }
        while (s.available() > 0 && produced < out_l.size()) {
            const std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(s.available()), out_l.size() - produced);
            float* dest[2] = {out_l.data() + produced, out_r.data() + produced}; produced += s.retrieve(dest, want);
        }
        const double expected = input_frames * ratio;
        expect(std::abs(static_cast<double>(produced) - expected) < 2500.0, "stretch duration");
        const std::vector<float> pitch_sample(out_l.begin() + std::min<std::size_t>(2000, produced / 4), out_l.begin() + std::min<std::size_t>(produced, 20000));
        expect(std::abs(zero_cross_pitch(pitch_sample, sr) - hz) < 8.0, "stretch pitch");
    }
}

int main() {
    test_ratio(); test_midi_and_capture_boundary(); test_real_slot_replace();
    test_publish_play_stop_destroy_lifetime(); test_replace_while_playing_rejected();
    test_dynamic_bpm_diagnostic(); test_stretch();
    std::cout << "PASS core ratio midi capture real_slot_replace play_lifetime replace_while_playing dynamic_bpm stretch\n";
}
