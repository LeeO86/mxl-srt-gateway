#include "util/net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cerrno>

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

bool isAnnounceIpv4(std::string const& text)
{
    std::uint32_t value = 0;
    if (!parseIpv4(text, &value))
    {
        return false;
    }
    unsigned const first = (value >> 24) & 0xff;
    return value != 0 && first != 127;
}

bool udpBindAvailable(std::string const& address, int port, std::string* error)
{
    int const fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        if (error != nullptr)
        {
            *error = "socket failed";
        }
        return false;
    }
    int const yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    auto const host = address.empty() ? "0.0.0.0" : address;
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        ::close(fd);
        if (error != nullptr)
        {
            *error = "address is not an IPv4 literal";
        }
        return false;
    }
    bool const ok = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (!ok && error != nullptr)
    {
        *error = std::strerror(errno);
    }
    ::close(fd);
    return ok;
}

int httpExchange(std::string const& method, std::string const& host, int port, std::string const& path, int timeoutMs)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    auto const portText = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0)
    {
        return -1;
    }
    int fd = -1;
    for (auto* item = result; item != nullptr; item = item->ai_next)
    {
        fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        int const flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int const connected = ::connect(fd, item->ai_addr, item->ai_addrlen);
        if (connected == 0)
        {
            break;
        }
        if (errno != EINPROGRESS)
        {
            ::close(fd);
            fd = -1;
            continue;
        }
        pollfd poller{};
        poller.fd = fd;
        poller.events = POLLOUT;
        if (::poll(&poller, 1, timeoutMs) <= 0)
        {
            ::close(fd);
            fd = -1;
            continue;
        }
        int soError = 0;
        socklen_t len = sizeof(soError);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
        if (soError != 0)
        {
            ::close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    ::freeaddrinfo(result);
    if (fd < 0)
    {
        return -1;
    }
    ::fcntl(fd, F_SETFL, 0);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    std::string const request = method + " " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) < 0)
    {
        ::close(fd);
        return -1;
    }
    std::string response;
    char buf[512];
    while (response.find("\r\n") == std::string::npos && response.size() < 2048)
    {
        auto const n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        response.append(buf, buf + n);
    }
    ::close(fd);
    if (response.compare(0, 5, "HTTP/") != 0)
    {
        return -1;
    }
    auto const space = response.find(' ');
    if (space == std::string::npos)
    {
        return -1;
    }
    try
    {
        return std::stoi(response.substr(space + 1));
    }
    catch (...)
    {
        return -1;
    }
}

int httpGetStatus(std::string const& host, int port, std::string const& path, int timeoutMs)
{
    return httpExchange("GET", host, port, path, timeoutMs);
}

int httpDelete(std::string const& host, int port, std::string const& path, int timeoutMs)
{
    return httpExchange("DELETE", host, port, path, timeoutMs);
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

bool tcpBindAvailable(int port, std::string* error)
{
    int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        if (error != nullptr)
        {
            *error = "socket failed";
        }
        return false;
    }
    int const yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bool const ok = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (!ok && error != nullptr)
    {
        *error = std::strerror(errno);
    }
    ::close(fd);
    return ok;
}
} // namespace srtgw
