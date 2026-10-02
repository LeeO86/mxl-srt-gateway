#pragma once

#include <string>
#include <vector>

namespace srtgw
{
struct RouteTap
{
    int track = 0;
    int channel = 0;
    double gainDb = 0.0;
    bool mute = false;
};

struct AudioOutput
{
    int channels = 16;
    std::string preset;
    std::vector<std::vector<RouteTap>> routes;
};

struct TrackView
{
    int channels = 0;
    float const* samples = nullptr;
    bool missing = false;
};

double dbToLinear(double db);
void applyMatrix(std::vector<TrackView> const& tracks, AudioOutput const& output, int frames, float* interleavedOut);
AudioOutput presetIngest(std::string const& name, int channels);
int layoutChannels(std::string const& layout);

struct EgressAudioTrack
{
    std::string codec = "aac";
    std::string layout = "stereo";
    std::vector<int> channels;
    int bitrate = 0;
    std::string language = "und";
    double gainDb = 0.0;
    bool mute = false;
    int pid = 0;
};

std::vector<EgressAudioTrack> presetEgress(std::string const& name);
int defaultAudioBitrate(std::string const& codec, std::string const& layout);
} // namespace srtgw
