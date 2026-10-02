#include "util/net.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace srtgw
{
std::string hostname()
{
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0)
    {
        return "srtgw";
    }
    buf[sizeof(buf) - 1] = '\0';
    return buf[0] == '\0' ? "srtgw" : std::string(buf);
}

std::string primaryIpv4()
{
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
    {
        return "127.0.0.1";
    }
    std::string found = "127.0.0.1";
    for (auto* it = list; it != nullptr; it = it->ifa_next)
    {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        auto const* addr = reinterpret_cast<sockaddr_in const*>(it->ifa_addr);
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) == nullptr)
        {
            continue;
        }
        if (std::strcmp(text, "127.0.0.1") == 0)
        {
            continue;
        }
        found = text;
        break;
    }
    ::freeifaddrs(list);
    return found;
}

namespace
{
bool parseIpv4(std::string_view text, std::uint32_t* out)
{
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;
    char tail = 0;
    if (std::sscanf(std::string(text).c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
    {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255)
    {
        return false;
    }
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}
} // namespace

bool ipv4InCidr(std::string_view ip, std::string_view cidr)
{
    auto const slash = cidr.find('/');
    std::string_view addr = cidr;
    int bits = 32;
    if (slash != std::string_view::npos)
    {
        addr = cidr.substr(0, slash);
        try
        {
            bits = std::stoi(std::string(cidr.substr(slash + 1)));
        }
        catch (...)
        {
            return false;
        }
    }
    if (bits < 0 || bits > 32)
    {
        return false;
    }
    std::uint32_t ipValue = 0;
    std::uint32_t net = 0;
    if (!parseIpv4(ip, &ipValue) || !parseIpv4(addr, &net))
    {
        return false;
    }
    if (bits == 0)
    {
        return true;
    }
    std::uint32_t const mask = bits == 32 ? 0xffffffffU : (0xffffffffU << (32 - bits));
    return (ipValue & mask) == (net & mask);
}

std::uint64_t taiNowNs()
{
    timespec ts{};
#if defined(CLOCK_TAI)
    if (::clock_gettime(CLOCK_TAI, &ts) == 0)
    {
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
    }
#endif
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0)
    {
        return 0;
    }
    // CLOCK_TAI is the facility clock MXL uses. If it is unavailable, shift
    // UTC by the current fixed leap-second offset so indices still advance.
    constexpr std::uint64_t kLeapSeconds = 37;
    return (static_cast<std::uint64_t>(ts.tv_sec) + kLeapSeconds) * 1000000000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

std::int64_t monoNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace srtgw
