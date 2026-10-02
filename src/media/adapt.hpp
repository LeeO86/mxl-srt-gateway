#pragma once

#include "media/format.hpp"

#include <string>

namespace srtgw
{
enum class Deint
{
    None,
    BwdifField,
    BwdifFrame,
    YadifField,
    YadifFrame,
    WeaveField,
    WeaveFrame,
};

enum class Lace
{
    None,
    TwoFrames,
    Psf,
};

struct AdaptationPlan
{
    Deint deint = Deint::None;
    Lace lace = Lace::None;
    bool fieldShift = false;
    bool scale = false;
    bool color = false;
    bool anamorphic = false;
    bool rateBySync = false;
    std::string aspect = "letterbox";
    std::string kernel = "bicubic";
    std::string targetFieldOrder = "tff";
    VideoFormat output{};
    std::string summary;
};

AdaptationPlan planAdaptation(VideoFormat source, VideoFormat const& target, std::string const& deinterlacer, std::string const& aspect,
    std::string const& kernel, std::string const& sourceScan);

std::string ffmpegFilter(AdaptationPlan const& plan, bool cuda);
std::string deintName(Deint deint);
} // namespace srtgw
