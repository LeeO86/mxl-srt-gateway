#include "media/matrix.hpp"

#include <cmath>

namespace srtgw
{
double dbToLinear(double db)
{
    return std::pow(10.0, db / 20.0);
}

void applyMatrix(std::vector<TrackView> const& tracks, AudioOutput const& output, int frames, float* interleavedOut)
{
    int const channels = output.channels;
    if (channels <= 0 || frames <= 0 || interleavedOut == nullptr)
    {
        return;
    }
    for (int i = 0; i < frames * channels; ++i)
    {
        interleavedOut[i] = 0.f;
    }
    for (int ch = 0; ch < channels; ++ch)
    {
        if (ch >= static_cast<int>(output.routes.size()))
        {
            continue;
        }
        for (auto const& tap : output.routes[static_cast<std::size_t>(ch)])
        {
            if (tap.mute || tap.track < 0 || tap.track >= static_cast<int>(tracks.size()))
            {
                continue;
            }
            auto const& track = tracks[static_cast<std::size_t>(tap.track)];
            if (track.missing || track.samples == nullptr || tap.channel < 0 || tap.channel >= track.channels)
            {
                continue;
            }
            float const gain = static_cast<float>(dbToLinear(tap.gainDb));
            for (int frame = 0; frame < frames; ++frame)
            {
                interleavedOut[frame * channels + ch] += track.samples[frame * track.channels + tap.channel] * gain;
            }
        }
    }
}

namespace
{
RouteTap tap(int track, int channel, double gain = 0.0)
{
    RouteTap item;
    item.track = track;
    item.channel = channel;
    item.gainDb = gain;
    return item;
}

void ensureRoutes(AudioOutput& output)
{
    output.routes.resize(static_cast<std::size_t>(output.channels));
}
} // namespace

AudioOutput presetIngest(std::string const& name, int channels)
{
    AudioOutput output;
    output.channels = channels < 2 ? 2 : (channels > 64 ? 64 : channels);
    output.preset = name;
    ensureRoutes(output);
    if (name == "5.1+stereo")
    {
        for (int i = 0; i < 6 && i < output.channels; ++i)
        {
            output.routes[static_cast<std::size_t>(i)].push_back(tap(0, i));
        }
        if (output.channels > 6)
        {
            output.routes[6].push_back(tap(1, 0));
        }
        if (output.channels > 7)
        {
            output.routes[7].push_back(tap(1, 1));
        }
        return output;
    }
    if (name == "302m-16")
    {
        for (int i = 0; i < output.channels; ++i)
        {
            output.routes[static_cast<std::size_t>(i)].push_back(tap(i / 8, i % 8));
        }
        return output;
    }
    if (name == "5.1-stereo-downmix")
    {
        // ITU-style 5.1 (L R C LFE Ls Rs) into stereo. LFE is left out.
        if (output.channels >= 1)
        {
            output.routes[0] = {tap(0, 0), tap(0, 2, -3.0), tap(0, 4, -3.0)};
        }
        if (output.channels >= 2)
        {
            output.routes[1] = {tap(0, 1), tap(0, 2, -3.0), tap(0, 5, -3.0)};
        }
        return output;
    }
    if (name == "mono-dual")
    {
        if (output.channels >= 1)
        {
            output.routes[0].push_back(tap(0, 0));
        }
        if (output.channels >= 2)
        {
            output.routes[1].push_back(tap(0, 0));
        }
        return output;
    }
    // "16ch-stereo" and the default sequential map: stereo PID n -> channels 2n, 2n+1.
    if (name == "16ch-stereo")
    {
        for (int i = 0; i < output.channels; ++i)
        {
            output.routes[static_cast<std::size_t>(i)].push_back(tap(i / 2, i % 2));
        }
        return output;
    }
    for (int i = 0; i < output.channels; ++i)
    {
        output.routes[static_cast<std::size_t>(i)].push_back(tap(0, i));
    }
    output.preset = name.empty() ? "sequential" : name;
    return output;
}

int layoutChannels(std::string const& layout)
{
    if (layout == "mono")
    {
        return 1;
    }
    if (layout == "5.1" || layout == "5.1(side)")
    {
        return 6;
    }
    if (layout == "7.1")
    {
        return 8;
    }
    return 2;
}

int defaultAudioBitrate(std::string const& codec, std::string const& layout)
{
    int const channels = layoutChannels(layout);
    if (codec == "mp2")
    {
        return channels <= 1 ? 128000 : 256000;
    }
    if (codec == "s302m" || codec == "302m")
    {
        return 0;
    }
    if (codec == "opus")
    {
        return channels <= 2 ? 128000 : 256000;
    }
    if (channels <= 1)
    {
        return 96000;
    }
    if (channels <= 2)
    {
        return 192000;
    }
    if (channels <= 6)
    {
        return 384000;
    }
    return 512000;
}

std::vector<EgressAudioTrack> presetEgress(std::string const& name)
{
    std::vector<EgressAudioTrack> tracks;
    if (name == "16ch-302m")
    {
        for (int part = 0; part < 2; ++part)
        {
            EgressAudioTrack track;
            track.codec = "s302m";
            track.layout = "7.1";
            track.language = "und";
            for (int ch = 0; ch < 8; ++ch)
            {
                track.channels.push_back(part * 8 + ch);
            }
            tracks.push_back(track);
        }
        return tracks;
    }
    if (name == "5.1+stereo")
    {
        EgressAudioTrack surround;
        surround.codec = "aac";
        surround.layout = "5.1";
        surround.bitrate = 384000;
        surround.language = "und";
        for (int ch = 0; ch < 6; ++ch)
        {
            surround.channels.push_back(ch);
        }
        EgressAudioTrack stereo;
        stereo.codec = "aac";
        stereo.layout = "stereo";
        stereo.bitrate = 192000;
        stereo.channels = {6, 7};
        tracks.push_back(surround);
        tracks.push_back(stereo);
        return tracks;
    }
    int const pairs = name == "8x-stereo-aac" ? 8 : 1;
    for (int i = 0; i < pairs; ++i)
    {
        EgressAudioTrack track;
        track.codec = "aac";
        track.layout = "stereo";
        track.bitrate = 192000;
        track.language = "und";
        track.channels = {i * 2, i * 2 + 1};
        tracks.push_back(track);
    }
    return tracks;
}
} // namespace srtgw
