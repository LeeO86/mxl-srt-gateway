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

namespace
{
std::atomic<bool> stopping{false};
std::atomic<bool> paused{false};
}

// Writes a short v210 colour-bar flow plus a 1 kHz tone on channel 0.
// Usage: mxl-srt-flow-writer <domain-dir> <domain-id> <flow-id> <seconds> [audio-flow-id]
// The audio flow id defaults to <flow-id>-a. SIGTERM ends it early; the flows are deleted when it
// exits. SIGUSR1 pauses or resumes the writing (the flows stay, their heads stand still).
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
            video.write(index, bars.data(), bars.size(), false);
            int const count = samplesPerGrain(static_cast<std::int64_t>(index), 25, 1);
            std::vector<float> samples(static_cast<std::size_t>(count * 2), 0.f);
            for (int frame = 0; frame < count; ++frame)
            {
                samples[static_cast<std::size_t>(frame * 2)] = 0.1f;
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
