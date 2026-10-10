#include "media/framesync.hpp"

#include <algorithm>

namespace srtgw
{
FrameSynchroniser::FrameSynchroniser(std::int64_t latencyNs, std::int64_t holdNs)
    : latencyNs_(latencyNs)
    , holdNs_(holdNs)
{
}

SyncDecision FrameSynchroniser::choose(std::int64_t outputTimeNs, std::vector<SourceFrame> const& available) const
{
    SyncDecision decision;
    if (available.empty())
    {
        decision.slate = true;
        return decision;
    }
    std::int64_t const deadline = outputTimeNs - latencyNs_;
    SourceFrame const* best = nullptr;
    for (auto const& frame : available)
    {
        if (frame.mappedPtsNs <= deadline && (best == nullptr || frame.index > best->index))
        {
            best = &frame;
        }
    }
    if (best == nullptr)
    {
        decision.slate = true;
        return decision;
    }
    if (outputTimeNs - best->mappedPtsNs > holdNs_)
    {
        decision.slate = true;
        return decision;
    }
    if (!hasLast_)
    {
        decision.sourceIndex = best->index;
        return decision;
    }
    if (best->index < lastIndex_)
    {
        decision.sourceIndex = best->index;
        return decision;
    }
    if (best->index <= lastIndex_)
    {
        decision.repeat = true;
        decision.sourceIndex = lastIndex_;
        return decision;
    }
    if (best->index == lastIndex_ + 1)
    {
        decision.sourceIndex = best->index;
        return decision;
    }
    std::int64_t const spaced = lastIndex_ + 2;
    bool const haveSpaced = std::any_of(available.begin(), available.end(), [&](SourceFrame const& frame) {
        return frame.index == spaced && frame.mappedPtsNs <= deadline;
    });
    if (haveSpaced)
    {
        decision.dropped = 1;
        decision.sourceIndex = spaced;
        return decision;
    }
    // The frames after lastIndex_ have already left the buffer. Stepping one frame
    // per output grain would never catch up and would write the slate forever.
    decision.dropped = static_cast<int>(best->index - lastIndex_ - 1);
    decision.sourceIndex = best->index;
    return decision;
}

void FrameSynchroniser::commit(SyncDecision const& decision)
{
    if (decision.slate || decision.sourceIndex < 0)
    {
        return;
    }
    hasLast_ = true;
    lastIndex_ = decision.sourceIndex;
}

void FrameSynchroniser::reset()
{
    hasLast_ = false;
    lastIndex_ = -1;
}

std::int64_t AudioAligner::add(std::int64_t errorNs, std::int64_t headroomNs)
{
    sumNs += errorNs;
    needNs = std::max(needNs, errorNs - headroomNs);
    if (++count < window)
    {
        return 0;
    }
    holdNs = std::max({std::int64_t{0}, needNs + marginNs, holdNs - releaseNs});
    std::int64_t const mean = sumNs / count - holdNs;
    sumNs = 0;
    needNs = INT64_MIN;
    count = 0;
    if (aligned && mean >= -stepNs && mean <= stepNs)
    {
        speed = std::clamp(static_cast<double>(mean) / static_cast<double>(horizonNs), -maxSpeed, maxSpeed);
        return 0;
    }
    speed = 0;
    aligned = mean >= -toleranceNs && mean <= toleranceNs;
    return aligned ? 0 : mean * 48 / 1000000;
}
} // namespace srtgw
