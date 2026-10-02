#include "media/slate.hpp"

#include "media/v210.hpp"

#include <algorithm>
#include <cctype>

namespace srtgw
{
namespace
{
// 5x7 glyphs, row-major, bit 4 is the leftmost pixel.
std::uint8_t const* glyph(char ch)
{
    static std::uint8_t const space[7] = {0, 0, 0, 0, 0, 0, 0};
    static std::uint8_t const n[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static std::uint8_t const o[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static std::uint8_t const s[7] = {0x0e, 0x11, 0x10, 0x0e, 0x01, 0x11, 0x0e};
    static std::uint8_t const i[7] = {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e};
    static std::uint8_t const g[7] = {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e};
    static std::uint8_t const a[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static std::uint8_t const l[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
    static std::uint8_t const digits[10][7] = {
        {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
        {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
        {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
        {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},
        {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
        {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
        {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e},
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
        {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
    };
    char const upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    switch (upper)
    {
    case 'N':
        return n;
    case 'O':
        return o;
    case 'S':
        return s;
    case 'I':
        return i;
    case 'G':
        return g;
    case 'A':
        return a;
    case 'L':
        return l;
    case ' ':
        return space;
    default:
        break;
    }
    if (upper >= '0' && upper <= '9')
    {
        return digits[upper - '0'];
    }
    return space;
}

void stamp(std::vector<std::uint16_t>& y, int width, int height, std::string const& text, int originX, int originY, int scale)
{
    int x = originX;
    for (char ch : text)
    {
        auto const* rows = glyph(ch);
        for (int row = 0; row < 7; ++row)
        {
            for (int col = 0; col < 5; ++col)
            {
                if ((rows[row] & (1U << (4 - col))) == 0)
                {
                    continue;
                }
                for (int sy = 0; sy < scale; ++sy)
                {
                    for (int sx = 0; sx < scale; ++sx)
                    {
                        int const px = x + col * scale + sx;
                        int const py = originY + row * scale + sy;
                        if (px >= 0 && py >= 0 && px < width && py < height)
                        {
                            y[static_cast<std::size_t>(py * width + px)] = 940;
                        }
                    }
                }
            }
        }
        x += 6 * scale;
    }
}
} // namespace

std::vector<std::uint8_t> renderSlate(int width, int height, std::string const& label, bool slate)
{
    std::vector<std::uint8_t> frame(v210Size(width, height));
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width * height), slate ? 128 : 64);
    std::vector<std::uint16_t> c(static_cast<std::size_t>(width), 512);
    if (slate)
    {
        int const scale = std::max(2, height / 90);
        std::string const headline = "NO SIGNAL";
        int const textWidth = static_cast<int>(headline.size()) * 6 * scale;
        stamp(y, width, height, headline, std::max(0, (width - textWidth) / 2), height / 2 - 8 * scale, scale);
        std::string clean;
        for (char ch : label)
        {
            if (std::isalnum(static_cast<unsigned char>(ch)) || ch == ' ')
            {
                clean.push_back(ch);
            }
        }
        if (clean.size() > 24)
        {
            clean.resize(24);
        }
        int const subScale = std::max(1, scale / 2);
        int const subWidth = static_cast<int>(clean.size()) * 6 * subScale;
        stamp(y, width, height, clean, std::max(0, (width - subWidth) / 2), height / 2 + 6 * scale, subScale);
    }
    auto const stride = v210Stride(width);
    for (int row = 0; row < height; ++row)
    {
        v210PackLine(y.data() + static_cast<std::size_t>(row * width), c.data(), c.data(), width, frame.data() + stride * static_cast<std::size_t>(row));
    }
    return frame;
}
} // namespace srtgw
