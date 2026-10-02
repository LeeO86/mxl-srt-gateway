#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace srtgw
{
std::size_t v210Stride(int width);
std::size_t v210Size(int width, int height);
void v210PackLine(std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr, int width, std::uint8_t* dst);
void fillV210(std::uint8_t* dst, int width, int height, std::uint16_t y, std::uint16_t cb, std::uint16_t cr);
std::vector<std::uint8_t> renderBars(int width, int height);
} // namespace srtgw
