#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace srtgw
{
std::vector<std::uint8_t> renderSlate(int width, int height, std::string const& label, bool slate);
} // namespace srtgw
