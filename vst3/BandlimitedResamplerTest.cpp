#include "BandlimitedResampler.h"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <cstdio>

using eps16::vst3::BandlimitedResampler;

static double renderTone(double frequency, std::uint32_t divider) {
    constexpr double dawRate = 48000.0;
    constexpr std::uint64_t durationCycles = 2000000;
    BandlimitedResampler resampler;
    if (!resampler.prepare(dawRate)) return -1.0;

    std::uint64_t sourceCycle = 0;
    std::uint64_t outputCycle = 0;
    std::uint64_t outputRemainder = 0;
    double squareSum = 0.0;
    std::size_t measured = 0;
    while (outputCycle < durationCycles) {
        outputRemainder += BandlimitedResampler::cpuClockHz;
        outputCycle += outputRemainder / 48000;
        outputRemainder %= 48000;
        while (sourceCycle <= outputCycle) {
            const auto phase = 2.0 * std::numbers::pi_v<double> * frequency *
                               sourceCycle /
                               BandlimitedResampler::cpuClockHz;
            const auto sample = static_cast<float>(std::sin(phase));
            resampler.push(sourceCycle, divider, sample, sample);
            sourceCycle += divider;
        }
        float left = 0.0f;
        float right = 0.0f;
        resampler.output(outputCycle, left, right);
        if (outputCycle > BandlimitedResampler::latencyCycles + 200000) {
            squareSum += static_cast<double>(left) * left;
            ++measured;
        }
    }
    return std::sqrt(squareSum / measured);
}

int main() {
    if (BandlimitedResampler::latencySamples(48000.0) != 59) return 1;
    const auto rate78kRms = renderTone(5000.0, 128);
    const auto rate44kRms = renderTone(5000.0, 224);
    const auto rate30kRms = renderTone(5000.0, 336);
    const auto rejectedRms = renderTone(30000.0, 128);
    std::printf("passband_rms=%f rejected_rms=%f ratio=%f\n",
                rate78kRms, rejectedRms, rejectedRms / rate78kRms);
    if (rate78kRms <= 0.65 || rate78kRms >= 0.75 ||
        rate44kRms <= 0.65 || rate44kRms >= 0.75 ||
        rate30kRms <= 0.65 || rate30kRms >= 0.75)
        return 1;
    if (rejectedRms >= rate78kRms * 0.01) return 1;
    return 0;
}
