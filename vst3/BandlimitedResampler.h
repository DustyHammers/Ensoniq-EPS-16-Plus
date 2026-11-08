#ifndef EPS16_VST3_BANDLIMITED_RESAMPLER_H
#define EPS16_VST3_BANDLIMITED_RESAMPLER_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace eps16::vst3 {

namespace detail {
inline double bandlimitedResamplerSinc(double value) {
    if (std::abs(value) < 1.0e-12) return 1.0;
    const auto angle = std::numbers::pi_v<double> * value;
    return std::sin(angle) / angle;
}
} // namespace detail

/* OversampleFactor 1 is the plain converter-rate path: BandlimitedResampler
   below is an alias for it, byte-for-byte the same design this class had
   before the output stage existed. OversampleFactor 16 is the same design
   reconfigured for the 16x-oversampled output-stage path (see
   output_stage.h for why 16x, not 8x) - 16x the taps, 1/16th the
   clock-divider unit, and 16x the history depth, so a source at 16x the
   rate keeps the same time-domain tap window.

   This scaling keeps latencyCycles exactly invariant under
   OversampleFactor: tapCount scales by it, maximumClockDivider scales by
   its reciprocal, and latencyCycles = (tapCount/2)*maximumClockDivider
   cancels the factor out algebraically. That is what makes the 16x path's
   reported latency identical to the 1x path's at any given converter
   rate - see BandlimitedResamplerTest.cpp's latency-equality check and
   PluginProcessor.cpp's setLatencySamples() call, which relies on it by
   always reading the 1x (OversampleFactor == 1) value regardless of
   whether the output stage is enabled. */
template <std::size_t OversampleFactor>
class BandlimitedResamplerT {
public:
    static_assert(OversampleFactor >= 1 && (16 % OversampleFactor) == 0,
                  "OversampleFactor must evenly divide the base clock "
                  "divider unit (16)");

    static constexpr std::uint64_t cpuClockHz = 10000000;
    static constexpr std::size_t tapCount = 48 * OversampleFactor;
    static constexpr std::size_t phaseCount = 512;
    static constexpr std::size_t historyCapacity = 256 * OversampleFactor;
    static constexpr std::uint32_t divisorUnit =
        static_cast<std::uint32_t>(16 / OversampleFactor);
    static constexpr std::uint32_t maximumClockDivider = divisorUnit * 32U;
    static constexpr std::uint64_t latencyCycles =
        (tapCount / 2) * maximumClockDivider;

    bool prepare(double dawSampleRate) {
        if (!std::isfinite(dawSampleRate) || dawSampleRate < 8000.0 ||
            dawSampleRate > 768000.0)
            return false;

        outputRate = dawSampleRate;
        coefficientTable.resize(32 * phaseCount * tapCount);
        for (std::size_t voiceIndex = 0; voiceIndex < 32; ++voiceIndex) {
            const auto divider =
                static_cast<std::uint32_t>(divisorUnit * (voiceIndex + 1));
            const double sourceRate = static_cast<double>(cpuClockHz) / divider;
            const double cutoff = 0.5 * passbandScale *
                                  std::min(1.0, outputRate / sourceRate);
            for (std::size_t phase = 0; phase < phaseCount; ++phase) {
                const double fraction = static_cast<double>(phase) / phaseCount;
                auto *table = coefficientTable.data() +
                              (voiceIndex * phaseCount + phase) * tapCount;
                double sum = 0.0;
                for (std::size_t tap = 0; tap < tapCount; ++tap) {
                    const int offset = static_cast<int>(tap) -
                                       (static_cast<int>(tapCount) / 2 - 1);
                    const double distance = fraction - offset;
                    const double windowPosition = distance / (tapCount / 2.0);
                    double weight = 0.0;
                    if (std::abs(windowPosition) <= 1.0) {
                        const double window =
                            0.42 + 0.5 * std::cos(std::numbers::pi_v<double> *
                                                   windowPosition) +
                            0.08 * std::cos(2.0 * std::numbers::pi_v<double> *
                                            windowPosition);
                        weight = 2.0 * cutoff *
                                 detail::bandlimitedResamplerSinc(
                                     2.0 * cutoff * distance) *
                                 window;
                    }
                    table[tap] = static_cast<float>(weight);
                    sum += weight;
                }
                if (std::abs(sum) > 1.0e-12)
                    for (std::size_t tap = 0; tap < tapCount; ++tap)
                        table[tap] = static_cast<float>(table[tap] / sum);
            }
        }
        reset();
        return true;
    }

    void reset() {
        historyWrite = 0;
        historyCount = 0;
    }

    void push(std::uint64_t cpuCycle, std::uint32_t clockDivider, float left,
              float right) {
        if (clockDivider < divisorUnit || clockDivider > maximumClockDivider ||
            (clockDivider % divisorUnit) != 0)
            return;
        history[historyWrite] = {cpuCycle, clockDivider, left, right};
        historyWrite = (historyWrite + 1) % historyCapacity;
        historyCount = std::min(historyCount + 1, historyCapacity);
    }

    void output(std::uint64_t cpuCycle, float &left, float &right) const {
        left = 0.0f;
        right = 0.0f;
        if (coefficientTable.empty() || historyCount < 2 ||
            cpuCycle <= latencyCycles)
            return;

        const auto targetCycle = cpuCycle - latencyCycles;
        std::size_t base = historyCount;
        for (std::size_t index = historyCount; index-- > 0;) {
            if (chronological(index).cycle <= targetCycle) {
                base = index;
                break;
            }
        }
        if (base == historyCount) return;

        const auto &baseFrame = chronological(base);
        const auto elapsed = targetCycle - baseFrame.cycle;
        const double fraction = std::clamp(
            static_cast<double>(elapsed) / baseFrame.divider, 0.0,
            std::nextafter(1.0, 0.0));
        const auto phase = std::min(
            static_cast<std::size_t>(fraction * phaseCount), phaseCount - 1);
        const auto *table = coefficients(baseFrame.divider, phase);

        for (std::size_t tap = 0; tap < tapCount; ++tap) {
            const int offset = static_cast<int>(tap) -
                               (static_cast<int>(tapCount) / 2 - 1);
            const auto source = static_cast<std::int64_t>(base) + offset;
            if (source < 0 || source >= static_cast<std::int64_t>(historyCount))
                continue;
            const auto &frame = chronological(static_cast<std::size_t>(source));
            left += frame.left * table[tap];
            right += frame.right * table[tap];
        }
    }

    static int latencySamples(double dawSampleRate) {
        if (!std::isfinite(dawSampleRate) || dawSampleRate <= 0.0) return 0;
        return static_cast<int>(std::ceil(
            static_cast<double>(latencyCycles) * dawSampleRate / cpuClockHz));
    }

private:
    static constexpr double passbandScale = 0.95;

    struct Frame {
        std::uint64_t cycle{};
        std::uint32_t divider{};
        float left{};
        float right{};
    };

    const Frame &chronological(std::size_t index) const {
        const auto oldest = (historyWrite + historyCapacity - historyCount) %
                            historyCapacity;
        return history[(oldest + index) % historyCapacity];
    }

    const float *coefficients(std::uint32_t divider, std::size_t phase) const {
        const auto voiceIndex = divider / divisorUnit - 1;
        return coefficientTable.data() +
               (voiceIndex * phaseCount + phase) * tapCount;
    }

    std::array<Frame, historyCapacity> history{};
    std::size_t historyWrite{};
    std::size_t historyCount{};
    std::vector<float> coefficientTable;
    double outputRate{};
};

using BandlimitedResampler = BandlimitedResamplerT<1>;
using BandlimitedResampler16x = BandlimitedResamplerT<16>;

} // namespace eps16::vst3

#endif
