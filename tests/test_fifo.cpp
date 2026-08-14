#define private public
#include "sampler_engine.hpp"
#undef private
#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>

static void expect(bool value, const char* what) {
    if (!value) { std::cerr << "FAIL: " << what << '\n'; std::exit(1); }
}

static constexpr int kNote = 36;
static constexpr int kOther = 37;
static constexpr std::size_t kQuantum = 128;

static void install_sample(SamplerEngine& engine, int note, std::size_t frames = 4096) {
    auto* sample = new SampleBuffer;
    sample->left = new float[frames];
    sample->right = new float[frames];
    for (std::size_t i = 0; i < frames; ++i) sample->left[i] = sample->right[i] = (i == 0 ? 0.25F : 0.0F);
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
    engine.process(kQuantum, in.data(), in.data(), out.data(), out.data(), batch.data(), count, true, true, 120.0);
}

static void pump(SamplerEngine& engine, int blocks) {
    std::array<float, kQuantum> in{};
    std::array<float, kQuantum> out{};
    for (int i = 0; i < blocks; ++i) engine.process(kQuantum, in.data(), in.data(), out.data(), out.data(), nullptr, 0, true, true, 120.0);
}

static void test_sequential() {
    SamplerEngine engine(48000.0, kQuantum, 1.0, 2);
    install_sample(engine, kNote);
    send(engine, {{0, 0x91, kNote, 100}});
    send(engine, {{0, 0x81, kNote, 0}});
    send(engine, {{0, 0x91, kNote, 100}});
    send(engine, {{0, 0x81, kNote, 0}});
    expect(engine.diagnostics().play_start.load() == 2, "sequential starts two voices");
    expect(engine.diagnostics().play_stop.load() == 2, "sequential stops two voices");
    expect(engine.pending_[kNote].size == 0 && engine.pending_[kNote].trailing_gaps == 0, "sequential FIFO empty");
    expect(!engine.voices_[0].active && !engine.voices_[1].active, "sequential voices released");
    expect(engine.diagnostics().runtime_sample_destructions.load() == 0, "sequential no runtime destruction");
}

static void test_overlap_fifo_and_shared_borrow() {
    SamplerEngine engine(48000.0, kQuantum, 1.0, 2);
    install_sample(engine, kNote);
    send(engine, {{0, 0x91, kNote, 100}, {0, 0x91, kNote, 110}});
    expect(engine.voices_[0].active && engine.voices_[1].active, "overlap keeps both voices active");
    expect(engine.voices_[0].sample == engine.slots_[kNote].current && engine.voices_[1].sample == engine.slots_[kNote].current, "overlap shares immutable sample borrow");
    expect(engine.pending_[kNote].size == 2 && engine.pending_[kNote].instances[0].voice == 0 && engine.pending_[kNote].instances[1].voice == 1, "overlap FIFO binds voice order");
    send(engine, {{0, 0x81, kNote, 0}});
    expect(!engine.voices_[0].active && engine.voices_[1].active, "first NoteOff releases oldest voice only");
    send(engine, {{0, 0x81, kNote, 0}});
    expect(!engine.voices_[1].active && engine.pending_[kNote].size == 0, "second NoteOff releases second voice");
}

static void test_voice_gap_gap() {
    SamplerEngine engine(48000.0, kQuantum, 1.0, 1);
    install_sample(engine, kNote);
    send(engine, {{0, 0x91, kNote, 100}, {0, 0x91, kNote, 100}, {0, 0x91, kNote, 100}});
    expect(engine.diagnostics().voice_exhausted.load() == 2, "two rejected NoteOns diagnosed");
    expect(engine.pending_[kNote].size == 1 && engine.pending_[kNote].trailing_gaps == 2, "voice gap gap compressed FIFO");
    send(engine, {{0, 0x81, kNote, 0}});
    expect(!engine.voices_[0].active, "voice gap gap first Off releases voice");
    send(engine, {{0, 0x81, kNote, 0}});
    send(engine, {{0, 0x81, kNote, 0}});
    expect(engine.pending_[kNote].size == 0 && engine.pending_[kNote].trailing_gaps == 0, "voice gap gap all consumed");
    expect(engine.diagnostics().play_stop.load() == 1, "gaps never release another voice");
}

static void test_voice_gap_later_voice() {
    SamplerEngine engine(48000.0, kQuantum, 1.0, 1);
    install_sample(engine, kNote);
    send(engine, {{0, 0x91, kNote, 100}, {0, 0x91, kNote, 100}});
    send(engine, {{0, 0x81, kNote, 0}});
    send(engine, {{0, 0x91, kNote, 100}});
    expect(engine.voices_[0].active, "later voice starts after first release");
    expect(engine.pending_[kNote].size == 1 && engine.pending_[kNote].instances[0].gaps_before == 1, "gap remains before later voice");
    send(engine, {{0, 0x81, kNote, 0}});
    expect(engine.voices_[0].active, "gap NoteOff does not release later voice");
    send(engine, {{0, 0x81, kNote, 0}});
    expect(!engine.voices_[0].active && engine.pending_[kNote].size == 0, "later voice released by its FIFO NoteOff");
}

static void test_natural_completion_and_isolation() {
    SamplerEngine engine(48000.0, kQuantum, 1.0, 2);
    install_sample(engine, kNote, 301);
    install_sample(engine, kOther, 4096);
    send(engine, {{0, 0x91, kNote, 100}});
    for (int i = 0; i < 1000 && engine.diagnostics().play_natural_complete.load() == 0; ++i) pump(engine, 1);
    expect(engine.diagnostics().play_natural_complete.load() == 1, "natural completion observed");
    send(engine, {{0, 0x91, kNote, 100}});
    expect(engine.voices_[0].active || engine.voices_[1].active, "later same-note voice starts");
    const auto same_note_voice = engine.voices_[0].active && engine.voices_[0].slot == kNote ? 0U : 1U;
    const auto stops_before_gap_off = engine.diagnostics().play_stop.load();
    send(engine, {{0, 0x91, kOther, 100}});
    send(engine, {{0, 0x81, kNote, 0}});
    expect(engine.voices_[same_note_voice].active && engine.voices_[same_note_voice].slot == kNote, "natural gap NoteOff preserves later same-note voice");
    expect(engine.diagnostics().play_stop.load() == stops_before_gap_off, "natural gap NoteOff does not count a play stop");
    send(engine, {{0, 0x81, kNote, 0}});
    send(engine, {{0, 0x81, kOther, 0}});
    expect(engine.diagnostics().play_stop.load() == 2, "later voice and other note release independently");
    expect(engine.diagnostics().runtime_sample_destructions.load() == 0, "natural isolation no runtime destruction");
}

static void test_multiple_rejected_and_bounded() {
    SamplerEngine engine(48000.0, kQuantum, 1.0, 1);
    install_sample(engine, kNote);
    send(engine, {{0, 0x91, kNote, 100}});
    for (int i = 0; i < 7; ++i) send(engine, {{0, 0x91, kNote, 100}});
    expect(engine.diagnostics().voice_exhausted.load() == 7, "multiple rejected NoteOns counted");
    expect(!engine.voices_[1].active, "voice beyond --voices remains inert");
    for (int i = 0; i < 8; ++i) send(engine, {{0, 0x81, kNote, 0}});
    expect(engine.pending_[kNote].size == 0 && engine.pending_[kNote].trailing_gaps == 0, "multiple rejected FIFO fully drains");
    expect(engine.diagnostics().play_stop.load() == 1, "multiple rejected NoteOffs release only bound voice");
}

int main() {
    test_sequential();
    test_overlap_fifo_and_shared_borrow();
    test_voice_gap_gap();
    test_voice_gap_later_voice();
    test_natural_completion_and_isolation();
    test_multiple_rejected_and_bounded();
    std::cout << "PASS fifo sequential overlap multi_gap gap_between_voices natural_completion multiple_rejected different_note_isolation shared_sample_borrow\n";
}
