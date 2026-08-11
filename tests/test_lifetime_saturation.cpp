#include <array>
#include <memory>
#include <thread>
#define private public
#include "sampler_engine.hpp"
#undef private
#include <cstdlib>
#include <iostream>

static void expect(bool value, const char* what) {
    if (!value) { std::cerr << "FAIL: " << what << '\n'; std::exit(1); }
}

static SampleBuffer* sample(int slot) {
    auto* p = new SampleBuffer;
    p->left = new float[1];
    p->right = new float[1];
    p->frames = 1;
    p->slot = slot;
    return p;
}

int main() {
    SamplerEngine engine(48000.0, 128, 1.0, 8);
    auto* old = sample(36);
    engine.slots_[36].current = old;

    for (std::size_t i = 0; i < 31; ++i) {
        auto* p = sample(static_cast<int>(i));
        p->retire_queued = true;
        expect(engine.retire_queue_.push(p), "fill retire queue");
    }
    for (std::size_t i = 0; i < engine.deferred_retire_.size(); ++i) {
        auto* p = sample(static_cast<int>(i));
        p->retire_deferred = true;
        engine.deferred_retire_[engine.deferred_retire_count_++] = p;
    }

    auto* replacement = sample(36);
    expect(engine.publish_queue_.push(replacement), "publish replacement");
    float in[1]{}; float out[1]{};
    engine.process(1, in, in, out, out, nullptr, 0, true, true, 120.0);
    expect(engine.slots_[36].current == old, "old slot retained when retire stores full");
    expect(engine.blocked_publish_ == replacement, "replacement retained when retire stores full");
}
