#include "media/v210.hpp"

#include <algorithm>
#include <cstring>

namespace srtgw
{
std::size_t v210Stride(int width)
{
    int const groups = (width + 5) / 6;
    return static_cast<std::size_t>(groups) * 16U;
}

std::size_t v210Size(int width, int height)
{
    return v210Stride(width) * static_cast<std::size_t>(height < 0 ? 0 : height);
}

namespace
{
void pack6(std::uint32_t* dst, std::uint16_t u0, std::uint16_t y0, std::uint16_t v0, std::uint16_t y1, std::uint16_t u2, std::uint16_t y2,
    std::uint16_t v2, std::uint16_t y3, std::uint16_t u4, std::uint16_t y4, std::uint16_t v4, std::uint16_t y5)
{
    auto const m = [](std::uint16_t v) { return static_cast<std::uint32_t>(v & 0x3ff); };
    dst[0] = m(u0) | (m(y0) << 10) | (m(v0) << 20);
    dst[1] = m(y1) | (m(u2) << 10) | (m(y2) << 20);
    dst[2] = m(v2) | (m(y3) << 10) | (m(u4) << 20);
    dst[3] = m(y4) | (m(v4) << 10) | (m(y5) << 20);
}
} // namespace

void v210PackLine(std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr, int width, std::uint8_t* dst)
{
    int const groups = (width + 5) / 6;
    for (int g = 0; g < groups; ++g)
    {
        auto at = [&](std::uint16_t const* plane, int index, std::uint16_t neutral) {
            if (index < 0 || index >= width)
            {
                return neutral;
            }
            return plane[index];
        };
        int const x = g * 6;
        std::uint32_t words[4];
        pack6(words, at(cb, x, 512), at(y, x, 64), at(cr, x, 512), at(y, x + 1, 64), at(cb, x + 2, 512), at(y, x + 2, 64), at(cr, x + 2, 512),
            at(y, x + 3, 64), at(cb, x + 4, 512), at(y, x + 4, 64), at(cr, x + 4, 512), at(y, x + 5, 64));
        std::memcpy(dst + static_cast<std::size_t>(g) * 16U, words, sizeof(words));
    }
}

void fillV210(std::uint8_t* dst, int width, int height, std::uint16_t y, std::uint16_t cb, std::uint16_t cr)
{
    std::vector<std::uint16_t> yLine(static_cast<std::size_t>(width), y);
    std::vector<std::uint16_t> cLine(static_cast<std::size_t>(width), cb);
    std::vector<std::uint16_t> rLine(static_cast<std::size_t>(width), cr);
    auto const stride = v210Stride(width);
    for (int row = 0; row < height; ++row)
    {
        v210PackLine(yLine.data(), cLine.data(), rLine.data(), width, dst + stride * static_cast<std::size_t>(row));
    }
}

std::vector<std::uint8_t> renderBars(int width, int height)
{
    std::vector<std::uint8_t> frame(v210Size(width, height));
    struct Bar
    {
        std::uint16_t y;
        std::uint16_t cb;
        std::uint16_t cr;
    };
    Bar const bars[8] = {
        {940, 512, 512},
        {877, 128, 555},
        {754, 384, 64},
        {690, 64, 167},
        {313, 960, 856},
        {250, 640, 960},
        {127, 896, 469},
        {64, 512, 512},
    };
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width));
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(width));
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(width));
    int const barWidth = std::max(1, width / 8);
    for (int x = 0; x < width; ++x)
    {
        auto const& bar = bars[std::min(7, x / barWidth)];
        y[static_cast<std::size_t>(x)] = bar.y;
        cb[static_cast<std::size_t>(x)] = bar.cb;
        cr[static_cast<std::size_t>(x)] = bar.cr;
    }
    auto const stride = v210Stride(width);
    for (int row = 0; row < height; ++row)
    {
        v210PackLine(y.data(), cb.data(), cr.data(), width, frame.data() + stride * static_cast<std::size_t>(row));
    }
    return frame;
}
} // namespace srtgw
