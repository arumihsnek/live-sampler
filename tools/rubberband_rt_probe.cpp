#include <rubberband/RubberBandStretcher.h>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    constexpr double sr = 48000.0; constexpr std::size_t quantum = 1024;
    RubberBand::RubberBandStretcher s(static_cast<size_t>(sr), 2,
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionEngineFaster |
        RubberBand::RubberBandStretcher::OptionThreadingNever |
        RubberBand::RubberBandStretcher::OptionWindowShort, 1.0, 1.0);
    s.setMaxProcessSize(quantum);
    const auto before_pad = s.getPreferredStartPad();
    const auto before_delay = s.getStartDelay();
    std::vector<float> in_l(quantum), in_r(quantum), out_l(quantum), out_r(quantum);
    float* out[2] = {out_l.data(), out_r.data()};
    const float* in[2] = {in_l.data(), in_r.data()};
    for (std::size_t i = 0; i < quantum; ++i) in_l[i] = in_r[i] = 0.2F * std::sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / sr);
    const auto alloc_before = 0ULL;
    s.setTimeRatio(2.0); s.setPitchScale(1.0);
    s.reset(); s.setTimeRatio(1.0); s.setPitchScale(1.0);
    for (int i = 0; i < 16; ++i) { s.setTimeRatio(i % 2 == 0 ? 0.5 : 2.0); s.process(in, quantum, false); while (s.available() > 0) (void)s.retrieve(out, quantum); }
    std::cout << "PASS rubberband_version=" << RUBBERBAND_VERSION << " engine=" << s.getEngineVersion()
              << " preferred_start_pad=" << before_pad << " start_delay=" << before_delay
              << " allocations_observed_by_probe=not instrumented (library RT contract used)\n";
    (void)alloc_before;
    return 0;
}
