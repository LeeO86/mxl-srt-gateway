#include "srt/url.hpp"

#include "srt/access.hpp"
#include "util/net.hpp"

#include <cstdio>

namespace srtgw
{
namespace
{
// Percent-encodes what would end or split a query value (&, #, %, +, space, ?, non-ASCII). The
// characters of an SRT access-control stream id (#!::r=…,m=…) other than # stay as they are.
std::string queryValue(std::string const& text)
{
    std::string out;
    for (unsigned char const ch : text)
    {
        bool const plain = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || std::string("-._~!$'()*,;:@/=").find(static_cast<char>(ch)) != std::string::npos;
        if (plain)
        {
            out += static_cast<char>(ch);
        }
        else
        {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", ch);
            out += hex;
        }
    }
    return out;
}

std::string hostPart(std::string const& host)
{
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}
} // namespace

std::string peerUrl(SrtEndpointConfig const& srt, std::string const& host, bool withSecret)
{
    int const latency = srt.latencyMs > 0 ? srt.latencyMs : 200;
    // No port, no URL (the UI hides the row).
    if ((srt.mode == "caller" ? srt.remotePort : srt.localPort) <= 0)
    {
        return {};
    }
    std::string url;
    std::string streamId;
    if (srt.mode == "caller")
    {
        url = "srt://0.0.0.0:" + std::to_string(srt.remotePort) + "?mode=listener";
    }
    else
    {
        bool const anyAddress = srt.localAddress.empty() || srt.localAddress == "0.0.0.0" || srt.localAddress == "::";
        url = "srt://" + hostPart(!anyAddress ? srt.localAddress : !host.empty() ? host : primaryIpv4()) + ":" + std::to_string(srt.localPort);
        url += srt.mode == "rendezvous" ? "?mode=rendezvous" : "?mode=caller";
        if (srt.mode == "listener" && !srt.acceptedStreamIds.empty())
        {
            streamId = srt.acceptedStreamIds.front();
        }
    }
    url += "&latency=" + std::to_string(latency);
    if (!streamId.empty())
    {
        url += "&streamid=" + queryValue(streamId);
    }
    if (withSecret && !srt.passphrase.empty())
    {
        url += "&passphrase=" + queryValue(srt.passphrase);
        int const key = effectiveKeyLength(policyFor(srt));
        if (key != 0 && key != 16)
        {
            url += "&pbkeylen=" + std::to_string(key);
        }
    }
    return url;
}
} // namespace srtgw
