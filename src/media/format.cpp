#include "media/format.hpp"

#include <cmath>
#include <sstream>

namespace srtgw
{
double Rate::hz() const
{
    return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
}

bool Rate::sameAs(Rate const& other) const
{
    if (num == other.num && den == other.den)
    {
        return true;
    }
    double const a = hz();
    double const b = other.hz();
    if (a <= 0.0 || b <= 0.0)
    {
        return false;
    }
    return std::abs(a - b) / a < 0.002;
}

std::string VideoFormat::label() const
{
    return formatLabel(width, height, interlaced, rate);
}

std::string VideoFormat::signature() const
{
    std::ostringstream out;
    out << width << 'x' << height << (interlaced ? 'i' : 'p') << rate.name << fieldOrder << color;
    return out.str();
}

double VideoFormat::fieldHz() const
{
    return interlaced ? rate.hz() * 2.0 : rate.hz();
}

std::vector<Rate> const& knownRates()
{
    static std::vector<Rate> const rates = {
        {24000, 1001, "23.98"},
        {24, 1, "24"},
        {25, 1, "25"},
        {30000, 1001, "29.97"},
        {30, 1, "30"},
        {50, 1, "50"},
        {60000, 1001, "59.94"},
        {60, 1, "60"},
    };
    return rates;
}

bool parseRate(std::string const& text, Rate* out)
{
    for (auto const& rate : knownRates())
    {
        if (text == rate.name || text == std::to_string(rate.num) + "/" + std::to_string(rate.den))
        {
            *out = rate;
            return true;
        }
    }
    if (text == "23.976")
    {
        *out = Rate{24000, 1001, "23.98"};
        return true;
    }
    return false;
}

bool allowedRaster(int width, int height)
{
    return (width == 1920 && height == 1080) || (width == 1280 && height == 720) || (width == 3840 && height == 2160);
}

bool allowedInterlace(VideoFormat const& format)
{
    if (!format.interlaced)
    {
        return true;
    }
    if (format.width != 1920 || format.height != 1080)
    {
        return false;
    }
    return format.rate.name == "25" || format.rate.name == "29.97";
}

std::string formatLabel(int width, int height, bool interlaced, Rate const& rate)
{
    int nominalHeight = height;
    std::string scan;
    std::string rateText = rate.name;
    if (interlaced)
    {
        scan = "i";
        if (rate.name == "25")
        {
            rateText = "50";
        }
        else if (rate.name == "29.97")
        {
            rateText = "59.94";
        }
    }
    else
    {
        scan = "p";
    }
    if (width == 1280 && height == 720)
    {
        nominalHeight = 720;
    }
    else if (width == 1920 && height == 1080)
    {
        nominalHeight = 1080;
    }
    else if (width == 3840 && height == 2160)
    {
        nominalHeight = 2160;
    }
    return std::to_string(nominalHeight) + scan + rateText;
}

std::int64_t sampleIndexAtGrain(std::int64_t grainIndex, int rateNum, int rateDen)
{
    if (rateNum <= 0)
    {
        return 0;
    }
    __int128 const num = static_cast<__int128>(grainIndex) * 48000 * rateDen + rateNum / 2;
    return static_cast<std::int64_t>(num / rateNum);
}

int samplesPerGrain(std::int64_t grainIndex, int rateNum, int rateDen)
{
    auto const count = sampleIndexAtGrain(grainIndex + 1, rateNum, rateDen) - sampleIndexAtGrain(grainIndex, rateNum, rateDen);
    return static_cast<int>(count < 0 ? 0 : count);
}
} // namespace srtgw
