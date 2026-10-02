#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace srtgw
{
struct Rate
{
    int num = 50;
    int den = 1;
    std::string name = "50";

    [[nodiscard]] double hz() const;
    [[nodiscard]] bool sameAs(Rate const& other) const;
};

struct VideoFormat
{
    int width = 1920;
    int height = 1080;
    bool interlaced = false;
    std::string fieldOrder = "tff";
    Rate rate{};
    std::string color = "bt709";
    int sarNum = 1;
    int sarDen = 1;

    [[nodiscard]] std::string label() const;
    [[nodiscard]] std::string signature() const;
    [[nodiscard]] double fieldHz() const;
};

std::vector<Rate> const& knownRates();
bool parseRate(std::string const& text, Rate* out);
bool allowedRaster(int width, int height);
bool allowedInterlace(VideoFormat const& format);
std::string formatLabel(int width, int height, bool interlaced, Rate const& rate);

int samplesPerGrain(std::int64_t grainIndex, int rateNum, int rateDen);
std::int64_t sampleIndexAtGrain(std::int64_t grainIndex, int rateNum, int rateDen);
} // namespace srtgw
