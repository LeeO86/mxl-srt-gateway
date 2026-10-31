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
// sample is than its due time (> 0: the audio is late) and how late the audio that arrived since
// the last grain came for its own due time (INT64_MIN: none came); every `window` grains the mean
// error is acted on (averaging keeps the jitter of the PTS mapping out).
// - The audio is held (`holdNs`) as late as the window's latest arrival plus `marginNs`: a rise is
//   inserted at once, and the writer delays the video by as much (lip sync over latency); when
//   less would do, the hold shrinks by at most `releaseNs` per window. A source that sends its
//   audio in bursts later than the sync latency plays a little later rather than with gaps. Audio
//   that is missing from the source is no reason to hold: the audio after the gap comes on time.
// - Until a mean is within `toleranceNs` (start-up), it is stepped away at once.
// - Once aligned, the resampler plays the audio slightly faster or slower (`speed`, at most
//   `maxSpeed`) so the error is gone in about `horizonNs`, without a click; only a mean beyond
//   `stepNs` (a jump in the source timestamps, a long stall) is stepped again, and the alignment
//   starts over.
// - `stepNs` also bounds the hold: a rise beyond it is held only when the next window needs it
//   too (one late batch, such as the audio a sender's muxer kept back over a stall of its
//   source, is not), and a hold more than `stepNs` above what two windows in a row need drops
//   to it at once (the step then removes the audio it delayed) instead of being released slowly.
// A step once aligned, or a held rise beyond `stepNs`, is a resync: `resynced` is set for that
// call and `jumpNs` is how far it moved the audio (+ later, - earlier).
struct AudioAligner
{
    int window = 25;
    std::int64_t toleranceNs = 20000000;
    // av_resync_ms.
    std::int64_t stepNs = 40000000;
    std::int64_t horizonNs = 2000000000;
    std::int64_t marginNs = 10000000;
    std::int64_t releaseNs = 500000;
    double maxSpeed = 0.005;
    std::int64_t sumNs = 0;
    // The latest arrival of the window (how late, for its due time).
    std::int64_t needNs = INT64_MIN;
    std::int64_t holdNs = 0;
    int count = 0;
    bool aligned = false;
    // The last window asked for a rise (or a drop) of the hold beyond stepNs that was not made.
    bool rising = false;
    bool falling = false;
    // How much faster than nominal the resampler plays the audio (< 0: slower).
    double speed = 0;
    bool resynced = false;
    std::int64_t jumpNs = 0;

    // Samples (48 kHz) to drop from (> 0), or silence to insert at (< 0), the queue's head now.
    [[nodiscard]] std::int64_t add(std::int64_t errorNs, std::int64_t lateNs);
};

// The source PTS -> TAI offset of an ingest's video, from TAI - PTS when each frame is ready.
// Frames that come late (a sender's bursts, a busy decoder) must not move it, the sync latency
// absorbs them, so it follows the earliest frames: the minimum of each `window` frames.
// - The first frame sets it; until the first window is complete it follows the lowest one.
// - A window minimum within `stepNs` moves it by at most `slewNs` (a source clock that drifts).
// - Two window minimums in a row beyond `stepNs` (a jump in the source timestamps, a source that
//   arrives earlier or later for good) step it to the second one; add() returns the step.
struct PtsMapper
{
    int window = 50;
    std::int64_t stepNs = 60000000;
    std::int64_t slewNs = 1000000;
    std::int64_t offsetNs = 0;
    bool valid = false;
    bool started = false;
    std::int64_t windowMinNs = INT64_MAX;
    int count = 0;
    int beyond = 0;

    // Adds a frame's TAI - PTS; returns how far the offset stepped (0 when it did not).
    [[nodiscard]] std::int64_t add(std::int64_t sampleNs);
};

// Ingest: how long after its mapped time + sync_latency_ms the synchroniser shows a frame, on
// the output grain grid. A frame is shown at the first grain at or after that time, so the delay
// is the phase of the mapping against the grid, anywhere in [0, a grain) and constant for a
// steady source; the audio is lined up with it (it assumed half a grain: an AV error of up to
// half a grain, e.g. -5 ms at 50p on a loop). Smoothed over about 20 frames; a change beyond
// `snapNs` (the mapping stepped) is taken at once.
struct VideoPhase
{
    std::int64_t snapNs = 5000000;
    bool valid = false;
    double phaseNs = 0;

    // Each newly shown frame: output grain time - video hold - sync latency - its mapped time.
    void add(std::int64_t delayNs, std::int64_t grainNs);
    // The phase, or half a grain before the first frame.
    [[nodiscard]] std::int64_t value(std::int64_t grainNs) const;
};

// Egress: what an output grain carries when its source grain may be missing. A grain the source
// skipped, marked invalid or did not commit by the send deadline repeats the last good frame;
// only when no good grain came for longer than `holdNs` (hold_ms, a real loss of signal) is the
// slate sent. The first good grain afterwards is sent again at once.
enum class EgressPicture
{
    Source,
    Repeat,
    Slate,
};

struct EgressHold
{
    std::int64_t holdNs = 500000000;
    bool haveGood = false;
    std::int64_t lastGoodNs = 0;

    // `nowNs`: the output grain's time; `good`: its source grain was read and valid.
    [[nodiscard]] EgressPicture decide(bool good, std::int64_t nowNs);
    // The route changed: nothing to repeat any more.
    void reset();
};

// Silence (48 kHz samples) to queue before decoded audio that starts `gapNs` after the queued audio
// ends: a gap in the source timestamps (lost or skipped frames) then keeps the queued samples on
// their times. Below 2 ms it is rounding; beyond 1 s it is a jump, which the aligner steps.
[[nodiscard]] std::int64_t gapSamples(std::int64_t gapNs);
} // namespace srtgw
