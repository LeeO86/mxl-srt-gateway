#include "mxl/domain.hpp"
#include "media/slate.hpp"
#include "media/v210.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <vector>

namespace
{
std::atomic<bool> stopping{false};
std::atomic<bool> paused{false};
}

// Writes a short v210 colour-bar flow plus a DC level on channel 0.
// Usage: mxl-srt-flow-writer <domain-dir> <domain-id> <flow-id> <seconds> [audio-flow-id] [marks]
// The audio flow id defaults to <flow-id>-a. SIGTERM ends it early; the flows are deleted when it
// exits. SIGUSR1 pauses or resumes the writing (the flows stay, their heads stand still).
// "marks": a black picture with one white frame, and silence with a 100 ms 1 kHz tone, both
// starting on each TAI multiple of 5 s (for mxl-srt-avsync-check).
int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::cerr << "usage: mxl-srt-flow-writer <domain-dir> <domain-id> <flow-id> <seconds> [audio-flow-id]\n";
        return 2;
    }
    using namespace srtgw;
    std::string const domainDir = argv[1];
    std::string const domainId = argv[2];
    std::string const flowId = argv[3];
    int const seconds = std::atoi(argv[4]);
    std::signal(SIGTERM, [](int) { stopping.store(true); });
    std::signal(SIGUSR1, [](int) { paused.store(!paused.load()); });
    std::string const audioFlowId = argc > 5 ? argv[5] : flowId + "-a";
    bool const marks = argc > 6 && std::string(argv[6]) == "marks";
    VideoFormat format;
    format.width = 1280;
    format.height = 720;
    format.rate = Rate{25, 1, "25"};
    try
    {
        MxlDomain domain(domainDir, domainId, 1000000000, true);
        MxlVideoWriter video(domain, flowId, "test video", "test:Video", format);
        MxlAudioWriter audio(domain, audioFlowId, "test audio", "test:Audio 1", 2);
        auto const bars = renderBars(format.width, format.height);
        std::vector<std::uint8_t> dark(v210Size(format.width, format.height));
        std::vector<std::uint8_t> white(dark.size());
        fillV210(dark.data(), format.width, format.height, 64, 512, 512);
        fillV210(white.data(), format.width, format.height, 940, 512, 512);
        std::int64_t const periodSamples = 5 * 48000;
        std::uint64_t index = grainIndexNow(format.rate);
        int const total = std::max(1, seconds) * 25;
        for (int i = 0; i < total && !stopping.load(); ++i)
        {
            if (paused.load())
            {
                while (paused.load() && !stopping.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                // Resumes at the current grain.
                index = grainIndexNow(format.rate);
            }
            // On the grain clock: a writer that runs slow shows as repeats on the egress.
            while (grainIndexNow(format.rate) < index)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            auto const& picture = !marks ? bars : (index % (5 * 25) == 0 ? white : dark);
            video.write(index, picture.data(), picture.size(), false);
            int const count = samplesPerGrain(static_cast<std::int64_t>(index), 25, 1);
            std::int64_t const first = sampleIndexAtGrain(static_cast<std::int64_t>(index), 25, 1);
            std::vector<float> samples(static_cast<std::size_t>(count * 2), 0.f);
            for (int frame = 0; frame < count; ++frame)
            {
                std::int64_t const inPeriod = (first + frame) % periodSamples;
                samples[static_cast<std::size_t>(frame * 2)] =
                    !marks ? 0.1f : (inPeriod < 4800 ? static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979 * 1000.0 * static_cast<double>(inPeriod) / 48000.0)) : 0.f);
            }
            std::uint64_t const end = static_cast<std::uint64_t>(sampleIndexAtGrain(static_cast<std::int64_t>(index + 1), 25, 1));
            audio.write(end, samples.data(), count, 2);
            ++index;
        }
    }
    catch (std::exception const& ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}
