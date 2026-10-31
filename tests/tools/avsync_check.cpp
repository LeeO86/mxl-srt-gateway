#include "mxl/domain.hpp"
#include "media/v210.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Follows a video and an audio flow and prints one JSON line per AV mark: a white frame on a
// dark picture and the onset of a tone after silence (mxl-srt-flow-writer "marks", or an ffmpeg
// source with the same pattern). Each line has the time of the white frame's grain and of the
// tone's first sample (TAI, ms within the period: how far the chain re-stamped a mark that was
// made on a TAI multiple of the period), av_ms (audio minus video) and unix_s, the wall clock
// when the mark was found.
// With [run-frames] (mxl-srt-flow-writer "marks": 3), a mark is the end of a run of white frames,
// and its line also has `run` (white frames) and `valid`: the run is run-frames +-1 long and no
// slate frame (the egress's NO SIGNAL picture, mid-grey) lies within 2 frames before it or right
// after it, as an analyzer needs the frames of its flash window. At the end one summary line:
// marks found, valid marks, slate frames seen.
// Usage: mxl-srt-avsync-check <domain-dir> <video-flow> <audio-flow> <seconds> [period-s] [run-frames]
int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::cerr << "usage: mxl-srt-avsync-check <domain-dir> <video-flow> <audio-flow> <seconds> [period-s] [run-frames]\n";
        return 2;
    }
    using namespace srtgw;
    std::string const videoFlow = argv[2];
    std::string const audioFlow = argv[3];
    int const seconds = std::atoi(argv[4]);
    std::int64_t const periodNs = (argc > 5 ? std::atoll(argv[5]) : 5) * 1000000000LL;
    int const runFrames = argc > 6 ? std::atoi(argv[6]) : 0;
    try
    {
        MxlDomain domain(argv[1], "avsync-check", 1000000000, false);
        MxlVideoReader video;
        MxlAudioReader audio;
        auto const start = std::chrono::steady_clock::now();
        auto const elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
        std::uint64_t nextGrain = 0;
        std::uint64_t nextSample = 0;
        bool bright = true;
        int quiet = 0;
        std::int64_t videoMark = -1;
        std::int64_t audioMark = -1;
        std::vector<float> block;
        // Run mode: the white run being read, the last slate frame, the run of the pending mark.
        std::int64_t runStartNs = 0;
        int run = 0;
        bool runSlate = false;
        std::uint64_t lastSlate = 0;
        bool haveSlate = false;
        int markRun = 0;
        bool markValid = false;
        int marks = 0;
        int validMarks = 0;
        int slateFrames = 0;
        while (elapsed() < seconds)
        {
            if (video.isOpen() && video.replaced())
            {
                video.close();
            }
            if (audio.isOpen() && audio.replaced())
            {
                audio.close();
            }
            if (!video.isOpen() && video.open(domain, videoFlow))
            {
                nextGrain = video.head();
            }
            if (!audio.isOpen() && audio.open(domain, audioFlow))
            {
                nextSample = audio.head();
            }
            if (!video.isOpen() || !audio.isOpen())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            auto const format = video.format();
            for (; nextGrain <= video.head(); ++nextGrain)
            {
                std::uint8_t const* grain = nullptr;
                std::size_t size = 0;
                bool invalid = false;
                if (!video.view(nextGrain, 0, &grain, &size, &invalid) || size < v210Size(format.width, format.height))
                {
                    continue;
                }
                // The mean luma of the middle row (the first Y of each 6-pixel group).
                std::uint8_t const* row = grain + v210Stride(format.width) * static_cast<std::size_t>(format.height / 2);
                std::uint64_t sum = 0;
                int const groups = format.width / 6;
                for (int g = 0; g < groups; ++g)
                {
                    std::uint32_t word = 0;
                    std::memcpy(&word, row + static_cast<std::size_t>(g) * 16, 4);
                    sum += (word >> 10) & 0x3ff;
                }
                std::uint64_t const luma = groups > 0 ? sum / static_cast<std::uint64_t>(groups) : 0;
                bool const now = luma > 500;
                if (runFrames <= 0)
                {
                    if (now && !bright)
                    {
                        videoMark = static_cast<std::int64_t>(grainTimeNs(format.rate, nextGrain));
                    }
                    bright = now;
                    continue;
                }
                // Dark is 64, the slate 128, white 940.
                bool const slate = luma > 100 && luma < 400;
                if (now)
                {
                    if (run == 0)
                    {
                        runStartNs = static_cast<std::int64_t>(grainTimeNs(format.rate, nextGrain));
                        runSlate = haveSlate && nextGrain - lastSlate <= 2;
                    }
                    ++run;
                    continue;
                }
                if (run > 0)
                {
                    videoMark = runStartNs;
                    markRun = run;
                    markValid = !runSlate && !slate && std::abs(run - runFrames) <= 1;
                    ++marks;
                    validMarks += markValid ? 1 : 0;
                    run = 0;
                }
                if (slate)
                {
                    lastSlate = nextGrain;
                    haveSlate = true;
                    ++slateFrames;
                }
            }
            int const channels = audio.channels();
            while (nextSample + 480 <= audio.head())
            {
                if (audio.read(nextSample + 480, 480, 0, block))
                {
                    for (int i = 0; i < 480; ++i)
                    {
                        if (std::fabs(block[static_cast<std::size_t>(i * channels)]) > 0.05f)
                        {
                            if (quiet >= 4800)
                            {
                                std::uint64_t const sample = nextSample + static_cast<std::uint64_t>(i);
                                audioMark = static_cast<std::int64_t>(sample / 48000 * 1000000000ULL + sample % 48000 * 1000000000ULL / 48000);
                            }
                            quiet = 0;
                        }
                        else
                        {
                            ++quiet;
                        }
                    }
                }
                nextSample += 480;
            }
            if (videoMark >= 0 && audioMark >= 0)
            {
                if (std::llabs(audioMark - videoMark) < periodNs / 2)
                {
                    double const unixS = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
                    std::printf("{\"unix_s\":%.2f,\"video_ms\":%.1f,\"audio_ms\":%.1f,\"av_ms\":%.1f", unixS, static_cast<double>(videoMark % periodNs) / 1e6,
                        static_cast<double>(audioMark % periodNs) / 1e6, static_cast<double>(audioMark - videoMark) / 1e6);
                    if (runFrames > 0)
                    {
                        std::printf(",\"run\":%d,\"valid\":%s", markRun, markValid ? "true" : "false");
                    }
                    std::printf("}\n");
                    std::fflush(stdout);
                    videoMark = -1;
                    audioMark = -1;
                }
                else if (audioMark < videoMark)
                {
                    audioMark = -1;
                }
                else
                {
                    videoMark = -1;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (runFrames > 0)
        {
            std::printf("{\"summary\":true,\"marks\":%d,\"valid\":%d,\"slate_frames\":%d}\n", marks, validMarks, slateFrames);
        }
    }
    catch (std::exception const& ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}
