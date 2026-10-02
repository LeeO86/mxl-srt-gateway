#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace srtgw
{
struct AccessPolicy
{
    std::string exposure = "internal";
    std::string mode = "listener";
    bool passphraseSet = false;
    int pbkeylen = 0;
    std::vector<std::string> acceptedStreamIds;
    std::vector<std::string> peerAllow;
};

struct AccessInput
{
    std::string peerIp;
    std::string streamId;
    bool cryptoRejected = false;
    std::int64_t nowMs = 0;
};

struct AccessDecision
{
    bool accept = true;
    std::string reason;
};

class AttemptLimiter
{
public:
    bool tooMany(std::string const& ip, std::int64_t nowMs) const;
    void record(std::string const& ip, std::int64_t nowMs);

private:
    std::map<std::string, std::vector<std::int64_t>> attempts_;
};

AccessDecision evaluateAccess(AccessPolicy const& policy, AccessInput const& input, AttemptLimiter const* limiter);
int effectiveKeyLength(AccessPolicy const& policy);
} // namespace srtgw
