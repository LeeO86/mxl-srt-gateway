#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace srtgw
{
std::string hostname();
std::string primaryIpv4();
bool ipv4InCidr(std::string_view ip, std::string_view cidr);
std::uint64_t taiNowNs();
std::int64_t monoNowMs();
} // namespace srtgw
