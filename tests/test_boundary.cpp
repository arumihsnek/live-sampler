#define private public
#include "sampler_engine.hpp"
#undef private
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

// Issue #8 regression: the zero-input finalization branch
// (v.stretcher->process(nullptr, 0, true) in render_voice) is a caller/API
// contract violation against Rubber Band 4.0.0 (process() dereferences
// inputs[c] unconditionally and documents no null-input exemption, unlike
// study()). The fix sends exactly one silent frame. These tests prove:
//   - the formerly-crashing branch is exercised exactly once per affected
//     voice and zero times for control lengths (diagnostic counter);
//   - natural completion semantics are unchanged (play_natural_complete,
//     voice release, bounded tail, finite output);
//   - no runtime sample destruction.
// The branch oracle is derived from the engine's ACTUAL configured quantum
// and the stretcher's ACTUAL start pad (read dynamically), never from a
// hard-coded constant.

static void expect(bool value, const char* what) {
    if (!value) { std::cerr << "FAIL: " << what << '\n'; std::exit(1); }
}

static constexpr int kNote = 36;
static constexpr int kOther = 37;
static constexpr std::size_t kQuantum = 128;
static constexpr std::size_t kMaxPump = 4000;
static constexpr std::size_t kCompletionBound = 1000;

static void install_tone(SamplerEngine& engine, int note, std::size_t frames) {
    auto* sample = new SampleBuffer;
    sample->left = new float[frames];
    sample->right = new float[frames];
    const double phase_step = 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const float v = static_cast<float>(0.3 * std::sin(phase_step * static_cast<double>(i)));
        sample->left[i] = v;
        sample->right[i] = v;
    }
    sample->frames = frames;
    sample->sample_rate = 48000.0;
    sample->captured_beats = 1.0;
    sample->slot = note;
    engine.slots_[note].current = sample;
}

static void send(SamplerEngine& engine, std::initializer_list<MidiEvent> events) {
    std::array<float, kQuantum> in{};
    std::array<float, kQuantum> out{};
    std::array<MidiEvent, 8> batch{};
    std::size_t count = 0;
    for (const auto& event : events) batch[count++] = event;
    engine.process(kQuantum, in.data(), in.data(), out.data(), out.data(), batch.data(), count,
                   true, true, 120.0);
}

// Pump one quantum block; returns false if any non-finite sample reaches output.
static bool pump_one(SamplerEngine& engine, std::array<float, kQuantum>& out) {
    std::array<float, kQuantum> in{};
    std::array<float, kQuantum> out_r{};
    engine.process(kQuantum, in.data(), in.data(), out.data(), out_r.data(), nullptr, 0,
                   true, true, 120.0);
    for (std::size_t i = 0; i < kQuantum; ++i) {
        if (!std::isfinite(out[i]) || !std::isfinite(out_r[i])) return false;
    }
    return true;
}

// Pump until the given number of natural completions is observed (bounded).
static std::size_t pump_until(SamplerEngine& engine, uint64_t target) {
    std::array<float, kQuantum> out{};
    std::size_t pumped = 0;
    while (engine.diagnostics().play_natural_complete.load() < target && pumped < kMaxPump) {
        expect(pump_one(engine, out), "finite output during playback");
        ++pumped;
    }
    return pumped;
}

// Single voice, one length. expect_finalize oracle derived from actual
// quantum + actual start pad: branch fires iff (pad + L) % quantum == 0.
static void test_single_length(std::size_t frames) {
    SamplerEngine engine(48000.0, kQuantum, 1.0, 2);
    install_tone(engine, kNote, frames);
    send(engine, {{0, 0x91, kNote, 100}});
    expect(engine.voices_[0].active, "voice active after start_play");
    expect(engine.voices_[0].input_left && engine.voices_[0].input_right,
           "input scratch buffers preallocated with capacity >= 1");
    const std::size_t pad = engine.voices_[0].stretcher->getPreferredStartPad();
    const std::size_t quantum = engine.quantum_;
    const bool expect_finalize = ((pad + frames) % quantum) == 0;

    const std::size_t pumped = pump_until(engine, 1);

    expect(engine.diagnostics().play_natural_complete.load() == 1, "natural completion observed");
    expect(pumped < kCompletionBound, "bounded tail / completion latency");
    expect(engine.diagnostics().runtime_sample_destructions.load() == 0,
           "no runtime sample destruction");
    expect(engine.diagnostics().stretcher_first_output_frame.load() != UINT64_MAX,
           "stretcher produced non-silent output");
    expect(engine.diagnostics().stretcher_finalize_calls.load() == (expect_finalize ? 1U : 0U),
           expect_finalize ? "zero-input finalization branch exercised exactly once"
                           : "zero-input finalization branch not exercised for control length");
    expect(!engine.voices_[0].active, "voice released after natural completion");
}

// Two voices, same note (FIFO overlap), same boundary length: both voices
// must each hit the branch exactly once -> global counter == 2.
static void test_overlap_both_boundary() {
    SamplerEngine engine(48000.0, kQuantum, 1.0, 2);
    constexpr std::size_t frames = 8192; // exact 128-multiple, pad multiple of 128
    install_tone(engine, kNote, frames);
    send(engine, {{0, 0x91, kNote, 100}, {0, 0x91, kNote, 110}});
    expect(engine.voices_[0].active && engine.voices_[1].active, "overlap keeps both voices active");

    const std::size_t pad = engine.voices_[0].stretcher->getPreferredStartPad();
    const std::size_t quantum = engine.quantum_;
    expect(((pad + frames) % quantum) == 0, "test precondition: boundary length");

    const std::size_t pumped = pump_until(engine, 2);
    expect(engine.diagnostics().play_natural_complete.load() == 2, "both voices complete naturally");
    expect(pumped < kCompletionBound, "bounded tail for overlap");
    expect(engine.diagnostics().stretcher_finalize_calls.load() == 2,
           "each of two overlapping boundary voices hits finalize branch once");
    expect(engine.diagnostics().runtime_sample_destructions.load() == 0,
           "overlap no runtime sample destruction");
}

// Two voices, same note, non-boundary control length: neither may hit branch.
static void test_overlap_control_no_finalize() {
    SamplerEngine engine(48000.0, kQuantum, 1.0, 2);
    constexpr std::size_t frames = 8193; // not an exact 128-multiple
    install_tone(engine, kNote, frames);
    send(engine, {{0, 0x91, kNote, 100}, {0, 0x91, kNote, 110}});
    expect(engine.voices_[0].active && engine.voices_[1].active, "overlap keeps both voices active");

    const std::size_t pad = engine.voices_[0].stretcher->getPreferredStartPad();
    const std::size_t quantum = engine.quantum_;
    expect(((pad + frames) % quantum) != 0, "test precondition: control length");

    const std::size_t pumped = pump_until(engine, 2);
    expect(engine.diagnostics().play_natural_complete.load() == 2, "both voices complete naturally");
    expect(pumped < kCompletionBound, "bounded tail for overlap control");
    expect(engine.diagnostics().stretcher_finalize_calls.load() == 0,
           "overlap control never hits finalize branch");
    expect(engine.diagnostics().runtime_sample_destructions.load() == 0,
           "overlap control no runtime sample destruction");
}

int main() {
    constexpr std::size_t triplets[3][3] = {
        {8063, 8064, 8065},
        {8191, 8192, 8193},
        {8319, 8320, 8321},
    };
    for (const auto& t : triplets) {
        for (std::size_t frames : t) test_single_length(frames);
    }
    test_overlap_both_boundary();
    test_overlap_control_no_finalize();
    std::cout << "PASS boundary 8063-8065 8191-8193 8319-8321 single overlap finite_output "
                 "natural_completion no_destruction finalize_branch_control\n";
    return 0;
}
