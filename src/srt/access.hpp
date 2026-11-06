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

// A caller link's failed connect attempts, for its log lines and the channel status. The first
// failure after a start or a connect, and each failure whose reason differs from the last one, is
// logged at warn; the same reason again at debug.
struct ConnectFailure
{
    // Failed attempts since the link started.
    std::uint64_t total = 0;
    // The last one: "<event>: <detail>" (e.g. "srt_connect_failed: Connection timeout") and when (UTC, ISO 8601).
    std::string reason;
    std::string at;
};

class ConnectFailures
{
public:
    // Records a failed attempt. True when it should be logged at warn.
    bool fail(std::string const& reason, std::string const& at);
    // A connect: the failed attempts since the last connect (0 when none). The next failure warns again.
    std::uint64_t connected();
    [[nodiscard]] ConnectFailure last() const { return last_; }
    // Failed attempts since the last connect (or the start).
    [[nodiscard]] std::uint64_t run() const { return run_; }

private:
    ConnectFailure last_;
    std::uint64_t run_ = 0;
};

AccessDecision evaluateAccess(AccessPolicy const& policy, AccessInput const& input, AttemptLimiter const* limiter);
int effectiveKeyLength(AccessPolicy const& policy);
} // namespace srtgw
