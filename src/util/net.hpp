#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace srtgw
{
std::string hostname();
std::string primaryIpv4();
// Dotted IPv4 that can be announced: not 0.0.0.0 and not 127.0.0.0/8.
bool isAnnounceIpv4(std::string const& text);
// Bind and release a UDP socket. Empty address means 0.0.0.0.
bool udpBindAvailable(std::string const& address, int port, std::string* error);
// HTTP/1.0 GET. Returns the status code, or -1 when the request does not complete.
int httpGetStatus(std::string const& host, int port, std::string const& path, int timeoutMs);
int httpDelete(std::string const& host, int port, std::string const& path, int timeoutMs);
bool ipv4InCidr(std::string_view ip, std::string_view cidr);
std::uint64_t taiNowNs();
std::int64_t monoNowMs();
} // namespace srtgw
