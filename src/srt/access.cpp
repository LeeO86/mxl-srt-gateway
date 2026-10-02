#include "srt/access.hpp"

#include "util/net.hpp"

namespace srtgw
{
bool AttemptLimiter::tooMany(std::string const& ip, std::int64_t nowMs) const
{
    auto const it = attempts_.find(ip);
    if (it == attempts_.end())
    {
        return false;
    }
    int count = 0;
    for (auto const stamp : it->second)
    {
        if (nowMs - stamp < 30000)
        {
            ++count;
        }
    }
    return count >= 10;
}

void AttemptLimiter::record(std::string const& ip, std::int64_t nowMs)
{
    auto& stamps = attempts_[ip];
    stamps.push_back(nowMs);
    while (!stamps.empty() && nowMs - stamps.front() > 30000)
    {
        stamps.erase(stamps.begin());
    }
}

int effectiveKeyLength(AccessPolicy const& policy)
{
    if (policy.pbkeylen == 16 || policy.pbkeylen == 24 || policy.pbkeylen == 32)
    {
        return policy.pbkeylen;
    }
    if (policy.exposure == "internet")
    {
        return 32;
    }
    return policy.passphraseSet ? 16 : 0;
}

AccessDecision evaluateAccess(AccessPolicy const& policy, AccessInput const& input, AttemptLimiter const* limiter)
{
    AccessDecision decision;
    if (limiter != nullptr && limiter->tooMany(input.peerIp, input.nowMs))
    {
        decision.accept = false;
        decision.reason = "rate_limit";
        return decision;
    }
    if (input.cryptoRejected)
    {
        decision.accept = false;
        decision.reason = "passphrase";
        return decision;
    }
    if (!policy.peerAllow.empty())
    {
        bool matched = false;
        for (auto const& cidr : policy.peerAllow)
        {
            if (ipv4InCidr(input.peerIp, cidr))
            {
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            decision.accept = false;
            decision.reason = "peer_allow";
            return decision;
        }
    }
    if (policy.mode == "listener" && !policy.acceptedStreamIds.empty())
    {
        bool matched = false;
        for (auto const& allowed : policy.acceptedStreamIds)
        {
            if (allowed == input.streamId)
            {
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            decision.accept = false;
            decision.reason = "streamid";
            return decision;
        }
    }
    if (policy.exposure == "internet" && policy.mode == "listener" && policy.acceptedStreamIds.empty())
    {
        decision.accept = false;
        decision.reason = "streamid_required";
        return decision;
    }
    return decision;
}
} // namespace srtgw
