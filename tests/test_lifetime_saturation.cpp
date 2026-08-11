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

    for (std::size_t i = 0; i < engine.retired_samples_.size(); ++i) {
        auto* p = sample(static_cast<int>(i % 128));
        p->retire_queued = true;
        engine.retired_samples_[engine.retired_count_++] = p;
    }

    auto* replacement = sample(36);
    expect(engine.publish_queue_.push(replacement), "publish replacement");
    float in[1]{}; float out[1]{};
    engine.start_worker();
    engine.process(1, in, in, out, out, nullptr, 0, true, true, 120.0);
    expect(engine.slots_[36].current == old, "old slot retained when retirement store is full");
    expect(engine.blocked_publish_ == replacement, "replacement retained when retirement store is full");
    expect(engine.diagnostics_.runtime_sample_destructions.load() == 0,
           "no SampleBuffer destruction during runtime");
    engine.stop_worker();
    expect(engine.diagnostics_.runtime_sample_destructions.load() == 0,
           "no SampleBuffer destruction before shutdown cleanup");
    std::cout << "PASS lifetime_saturation runtime_sample_destructions=0\n";
}
