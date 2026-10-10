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
// sample is than its due time (> 0: the audio is late) and how much audio the queue holds beyond
// this grain (its headroom); every `window` grains the mean error is acted on (averaging keeps
// the jitter of the PTS mapping out).
// - The audio is held (`holdNs`) as late as the window's worst grain needed to keep `marginNs`
//   of headroom, at once; when less would do, the hold shrinks by at most `releaseNs` per window.
//   A source that sends its audio in bursts later than the sync latency plays a little late
//   rather than with gaps.
// - Until a mean is within `toleranceNs` (start-up), it is stepped away at once.
// - Once aligned, the resampler plays the audio slightly faster or slower (`speed`, at most
//   `maxSpeed`) so the error is gone in about `horizonNs`, without a click; only a mean beyond
//   `stepNs` (a jump in the source timestamps, a long stall) is stepped again, and the alignment
//   starts over.
struct AudioAligner
{
    int window = 25;
    std::int64_t toleranceNs = 20000000;
    std::int64_t stepNs = 100000000;
    std::int64_t horizonNs = 2000000000;
    std::int64_t marginNs = 10000000;
    std::int64_t releaseNs = 500000;
    double maxSpeed = 0.005;
    std::int64_t sumNs = 0;
    // The most any grain of the window needed the audio later (error - headroom).
    std::int64_t needNs = INT64_MIN;
    std::int64_t holdNs = 0;
    int count = 0;
    bool aligned = false;
    // How much faster than nominal the resampler plays the audio (< 0: slower).
    double speed = 0;

    // Samples (48 kHz) to drop from (> 0), or silence to insert at (< 0), the queue's head now.
    [[nodiscard]] std::int64_t add(std::int64_t errorNs, std::int64_t headroomNs);
};
} // namespace srtgw
