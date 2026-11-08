#ifndef EPS16_VST3_BANDLIMITED_RESAMPLER_H
#define EPS16_VST3_BANDLIMITED_RESAMPLER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace eps16::vst3 {

class BandlimitedResampler {
public:
    static constexpr std::uint64_t cpuClockHz = 10000000;
    static constexpr std::size_t tapCount = 48;
    static constexpr std::size_t phaseCount = 512;
    static constexpr std::size_t historyCapacity = 256;
    static constexpr std::uint32_t maximumClockDivider = 16U * 32U;
    static constexpr std::uint64_t latencyCycles =
        (tapCount / 2) * maximumClockDivider;

    bool prepare(double dawSampleRate);
    void reset();
    void push(std::uint64_t cpuCycle, std::uint32_t clockDivider,
              float left, float right);
    void output(std::uint64_t cpuCycle, float &left, float &right) const;

    static int latencySamples(double dawSampleRate);

private:
    struct Frame {
        std::uint64_t cycle{};
        std::uint32_t divider{};
        float left{};
        float right{};
    };

    const Frame &chronological(std::size_t index) const;
    const float *coefficients(std::uint32_t divider,
                              std::size_t phase) const;

    std::array<Frame, historyCapacity> history{};
    std::size_t historyWrite{};
    std::size_t historyCount{};
    std::vector<float> coefficientTable;
    double outputRate{};
};

} // namespace eps16::vst3

#endif
