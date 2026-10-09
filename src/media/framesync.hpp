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

// Lines an audio queue up with the video. Each grain adds how much older the queue's first
// sample is than its due time (> 0: the audio is late). Every `window` grains the mean error,
// when it is beyond the tolerance, is corrected at once: averaging keeps the jitter of the
// PTS mapping (decoder bursts) from clicking.
struct AudioAligner
{
    int window = 25;
    std::int64_t toleranceNs = 20000000;
    std::int64_t sumNs = 0;
    int count = 0;

    // Samples (48 kHz) to drop from (> 0), or silence to insert at (< 0), the queue's head now.
    [[nodiscard]] std::int64_t add(std::int64_t errorNs);
};
} // namespace srtgw
