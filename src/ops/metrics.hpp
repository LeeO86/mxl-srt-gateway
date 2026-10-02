#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace srtgw
{
struct ChannelMetrics
{
    std::string id;
    int state = 0;
    double srtRttMs = 0;
    std::uint64_t pktLoss = 0;
    std::uint64_t pktRetrans = 0;
    std::uint64_t pktDrop = 0;
    double bitrateBps = 0;
    double bufferMs = 0;
    int connected = 0;
    std::uint64_t videoFrames = 0;
    std::uint64_t repeats = 0;
    std::uint64_t drops = 0;
    double audioDriftPpm = 0;
    double audioFifoMs = 0;
    std::uint64_t decodeErrors = 0;
    double decodeFps = 0;
    double encodeFps = 0;
    std::uint64_t encodeLatencyCount = 0;
    double encodeLatencySum = 0;
    std::vector<std::uint64_t> encodeLatencyBuckets;
    int failoverActive = 0;
    std::string decoder;
    std::string encoder;
    std::string sourceFormat;
    std::string targetFormat;
};

struct ProcessMetrics
{
    double residentBytes = 0;
    double cpuSeconds = 0;
    int nvencSessions = 0;
    int nvdecSessions = 0;
    double gpuMemoryBytes = 0;
    std::vector<ChannelMetrics> channels;
};

std::string renderMetrics(ProcessMetrics const& metrics);
int stateCode(std::string const& name);
} // namespace srtgw
