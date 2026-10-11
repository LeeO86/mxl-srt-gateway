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

std::int64_t AudioAligner::add(std::int64_t errorNs, std::int64_t lateNs)
{
    sumNs += errorNs;
    needNs = std::max(needNs, lateNs);
    if (++count < window)
    {
        return 0;
    }
    std::int64_t const before = holdNs;
    holdNs = std::max({std::int64_t{0}, needNs + marginNs, holdNs - releaseNs});
    // A hold that rises is inserted at once, the video waits as long (holdNs); the rest is slewed.
    std::int64_t const attack = std::max<std::int64_t>(0, holdNs - before);
    std::int64_t const mean = sumNs / count - holdNs;
    sumNs = 0;
    needNs = INT64_MIN;
    count = 0;
    if (aligned && mean + attack >= -stepNs && mean + attack <= stepNs)
    {
        speed = std::clamp(static_cast<double>(mean + attack) / static_cast<double>(horizonNs), -maxSpeed, maxSpeed);
        return -attack * 48 / 1000000;
    }
    speed = 0;
    aligned = mean >= -toleranceNs && mean <= toleranceNs;
    return aligned ? 0 : mean * 48 / 1000000;
}

std::int64_t PtsMapper::add(std::int64_t sampleNs)
{
    if (!valid || (!started && sampleNs < offsetNs))
    {
        offsetNs = sampleNs;
        valid = true;
    }
    windowMinNs = std::min(windowMinNs, sampleNs);
    if (++count < window)
    {
        return 0;
    }
    std::int64_t const minNs = windowMinNs;
    windowMinNs = INT64_MAX;
    count = 0;
    if (!started)
    {
        started = true;
        offsetNs = minNs;
        return 0;
    }
    std::int64_t const error = minNs - offsetNs;
    if (error > stepNs || error < -stepNs)
    {
        if (++beyond < 2)
        {
            return 0;
        }
        beyond = 0;
        offsetNs = minNs;
        return error;
    }
    beyond = 0;
    offsetNs += std::clamp(error, -slewNs, slewNs);
    return 0;
}

std::int64_t gapSamples(std::int64_t gapNs)
{
    return gapNs > 2000000 && gapNs < 1000000000 ? gapNs * 48 / 1000000 : 0;
}
} // namespace srtgw
