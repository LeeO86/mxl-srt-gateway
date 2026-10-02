#pragma once

#include <cstdint>
#include <vector>

namespace srtgw
{
struct SourceFrame
{
    std::int64_t index = 0;
    std::int64_t mappedPtsNs = 0;
};

struct SyncDecision
{
    bool slate = false;
    bool repeat = false;
    int dropped = 0;
    std::int64_t sourceIndex = -1;
};

class FrameSynchroniser
{
public:
    FrameSynchroniser(std::int64_t latencyNs, std::int64_t holdNs);

    [[nodiscard]] SyncDecision choose(std::int64_t outputTimeNs, std::vector<SourceFrame> const& available) const;
    void commit(SyncDecision const& decision);
    void reset();

private:
    std::int64_t latencyNs_;
    std::int64_t holdNs_;
    bool hasLast_ = false;
    std::int64_t lastIndex_ = -1;
};
} // namespace srtgw
