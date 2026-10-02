#include "config/config.hpp"
#include "media/adapt.hpp"
#include "media/format.hpp"
#include "media/framesync.hpp"
#include "media/matrix.hpp"
#include "media/slate.hpp"
#include "media/v210.hpp"
#include "nmos/ids.hpp"
#include "ops/metrics.hpp"
#include "srt/access.hpp"
#include "util/uuid.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <map>

using namespace srtgw;

TEST_CASE("uuid v5 matches the RFC URL example")
{
    CHECK(uuidV5(kUuidNamespaceUrl, "https://www.example.com") != "");
    CHECK(isUuid(uuidV5(kUuidNamespaceUrl, "mxl-srt-gateway/lab/node")));
    auto const a = makeNmosIds("lab-srtgw");
    auto const b = makeNmosIds("lab-srtgw");
    CHECK(a.node == b.node);
    CHECK(a.device != a.node);
    ChannelConfig channel = defaultIngest("in1", 9000);
    auto const flowA = a.videoFlow(channel);
    channel.target.rate = Rate{25, 1, "25"};
    auto const flowB = a.videoFlow(channel);
    CHECK(flowA != flowB);
    CHECK(a.videoSender(channel) == b.videoSender(defaultIngest("in1", 9000)));
}

TEST_CASE("config environment overrides the file and defaults")
{
    std::map<std::string, std::string> file = {{"WEB_PORT", "8200"}, {"LOG_LEVEL", "debug"}, {"HOST_ID", "from-file"}};
    std::map<std::string, std::string> env = {{"WEB_PORT", "8300"}, {"DECODER", "cpu"}};
    auto const loaded = loadFromSources(file, "[]", env);
    CHECK(loaded.config.webPort == 8300);
    CHECK(loaded.config.logLevel == "debug");
    CHECK(loaded.config.hostId == "from-file");
    CHECK(loaded.config.decoder == "cpu");
    CHECK(loaded.origin.at("WEB_PORT") == ValueOrigin::Env);
    CHECK(loaded.origin.at("LOG_LEVEL") == ValueOrigin::File);
    CHECK(loaded.origin.at("ENCODER") == ValueOrigin::Default);
    CHECK(loaded.config.srtPortMin == 9000);
    CHECK(loaded.config.srtPortMax == 9099);
    CHECK(isUuid(loaded.config.mxlOutputDomainId));
}

TEST_CASE("a matrix patch keeps the channel label and the per-tap gains")
{
    auto original = channelFromJson("{\"id\":\"in1\",\"label\":\"Ingest 1\",\"direction\":\"ingest\"}", nullptr);
    CHECK(original.label == "Ingest 1");
    auto patched = channelFromJson(
        "{\"id\":\"in1\",\"audio_outputs\":[{\"channels\":2,\"preset\":\"custom\",\"routes\":[[{\"track\":0,\"channel\":0,\"gain_db\":0},{\"track\":0,\"channel\":2,\"gain_db\":-3}],[{\"track\":1,\"channel\":1,\"gain_db\":-6,\"mute\":true}]]}],\"egress\":{\"audio_tracks\":[{\"codec\":\"aac\",\"layout\":\"stereo\",\"channels\":[4,1],\"language\":\"ger\",\"gain_db\":-1.5}]}}",
        &original);
    CHECK(patched.label == "Ingest 1");
    CHECK(patched.audioOutputs.size() == 1);
    CHECK(patched.audioOutputs[0].routes[0].size() == 2);
    CHECK(patched.audioOutputs[0].routes[0][1].gainDb == doctest::Approx(-3.0));
    CHECK(patched.audioOutputs[0].routes[1][0].mute);
    CHECK(patched.egress.audioTracks[0].channels.size() == 2);
    CHECK(patched.egress.audioTracks[0].channels[0] == 4);
    CHECK(patched.egress.audioTracks[0].language == "ger");
    CHECK(patched.egress.codec == original.egress.codec);
}

TEST_CASE("internet listener without a passphrase or streamid is rejected")
{
    auto channel = defaultIngest("edge", 9000);
    channel.srt.exposure = "internet";
    std::string json = "[" + channelToJson(channel, true) + "]";
    CHECK_THROWS_AS(loadFromSources({}, json, {}), ConfigError);
    channel.srt.passphrase = "correct horse battery";
    channel.srt.pbkeylen = 32;
    json = "[" + channelToJson(channel, true) + "]";
    CHECK_THROWS_AS(loadFromSources({}, json, {}), ConfigError);
    channel.srt.acceptedStreamIds = {"news-1"};
    json = "[" + channelToJson(channel, true) + "]";
    auto const loaded = loadFromSources({}, json, {});
    CHECK(loaded.config.channels.size() == 1);
    auto const masked = channelToJson(loaded.config.channels[0], false);
    CHECK(masked.find("correct horse") == std::string::npos);
    CHECK(masked.find("passphrase_set") != std::string::npos);
}

TEST_CASE("listener ports must sit inside the configured range")
{
    auto channel = defaultIngest("in", 80);
    CHECK_THROWS_AS(loadFromSources({}, "[" + channelToJson(channel, true) + "]", {}), ConfigError);
}

TEST_CASE("audio cadence is exact")
{
    for (int i = 0; i < 100; ++i)
    {
        CHECK(samplesPerGrain(i, 50, 1) == 960);
    }
    std::int64_t sum2997 = 0;
    bool saw1601 = false;
    bool saw1602 = false;
    for (int i = 0; i < 100; ++i)
    {
        int const count = samplesPerGrain(i, 30000, 1001);
        CHECK(count >= 1601);
        CHECK(count <= 1602);
        saw1601 = saw1601 || count == 1601;
        saw1602 = saw1602 || count == 1602;
        sum2997 += count;
    }
    CHECK(saw1601);
    CHECK(saw1602);
    std::int64_t sum5994 = 0;
    bool saw800 = false;
    bool saw801 = false;
    for (std::int64_t i = 0; i < 60000; ++i)
    {
        int const count = samplesPerGrain(i, 60000, 1001);
        CHECK(count >= 800);
        CHECK(count <= 801);
        saw800 = saw800 || count == 800;
        saw801 = saw801 || count == 801;
        sum5994 += count;
    }
    CHECK(saw800);
    CHECK(saw801);
    CHECK(sum5994 == 48000LL * 1001);
    CHECK(sampleIndexAtGrain(5, 50, 1) == 4800);
    (void)sum2997;
}

TEST_CASE("adaptation matrix covers the specification rows")
{
    VideoFormat i50;
    i50.width = 1920;
    i50.height = 1080;
    i50.interlaced = true;
    i50.fieldOrder = "tff";
    i50.rate = Rate{25, 1, "25"};
    VideoFormat p50;
    p50.width = 1920;
    p50.height = 1080;
    p50.rate = Rate{50, 1, "50"};
    VideoFormat p25 = p50;
    p25.rate = Rate{25, 1, "25"};
    VideoFormat hd720 = p50;
    hd720.width = 1280;
    hd720.height = 720;
    VideoFormat sd;
    sd.width = 720;
    sd.height = 576;
    sd.rate = Rate{25, 1, "25"};
    sd.color = "bt601";
    sd.interlaced = true;
    sd.fieldOrder = "tff";

    auto const field = planAdaptation(i50, p50, "bwdif", "letterbox", "bicubic", "auto");
    CHECK(field.deint == Deint::BwdifField);
    CHECK(ffmpegFilter(field, false).find("bwdif=mode=send_field") != std::string::npos);
    auto const fieldCuda = ffmpegFilter(field, true);
    CHECK(fieldCuda.find("bwdif_cuda=mode=send_field") != std::string::npos);
    CHECK(fieldCuda.find("hwdownload,format=nv12") != std::string::npos);

    auto const frame = planAdaptation(i50, p25, "bwdif", "letterbox", "bicubic", "auto");
    CHECK(frame.deint == Deint::BwdifFrame);

    auto const lace = planAdaptation(p50, i50, "bwdif", "letterbox", "bicubic", "auto");
    CHECK(lace.lace == Lace::TwoFrames);
    CHECK(ffmpegFilter(lace, false).find("tinterlace=mode=interleave_top") != std::string::npos);

    auto const psf = planAdaptation(p25, i50, "bwdif", "letterbox", "bicubic", "auto");
    CHECK(psf.lace == Lace::Psf);

    auto const scaled = planAdaptation(p50, hd720, "bwdif", "fill", "lanczos", "auto");
    CHECK(scaled.scale);
    CHECK(ffmpegFilter(scaled, false).find("force_original_aspect_ratio=increase") != std::string::npos);
    auto const scaledCuda = ffmpegFilter(scaled, true);
    CHECK(scaledCuda.find("scale_cuda=1280:720:force_original_aspect_ratio=increase:interp_algo=lanczos") != std::string::npos);
    CHECK(scaledCuda.find("hwdownload") < scaledCuda.find("crop=1280:720"));

    VideoFormat hd = p50;
    auto const color = planAdaptation(sd, hd, "yadif", "letterbox", "bicubic", "auto");
    CHECK(color.color);
    CHECK(color.deint != Deint::None);

    VideoFormat p5994 = p50;
    p5994.rate = Rate{60000, 1001, "59.94"};
    auto const sync = planAdaptation(p5994, p50, "bwdif", "letterbox", "bicubic", "auto");
    CHECK(sync.rateBySync);
    CHECK(sync.deint == Deint::None);

    VideoFormat bff = i50;
    bff.fieldOrder = "bff";
    auto const shift = planAdaptation(i50, bff, "bwdif", "letterbox", "bicubic", "auto");
    CHECK(shift.fieldShift);

    VideoFormat anamorphic = sd;
    anamorphic.sarNum = 16;
    anamorphic.sarDen = 11;
    anamorphic.interlaced = false;
    anamorphic.rate = Rate{25, 1, "25"};
    auto const sar = planAdaptation(anamorphic, p25, "weave", "letterbox", "bicubic", "auto");
    CHECK(sar.anamorphic);
    CHECK(ffmpegFilter(sar, false).find("setsar=1") != std::string::npos);

    auto const forced = planAdaptation(p50, p50, "bwdif", "letterbox", "bicubic", "tff");
    CHECK(forced.deint != Deint::None);
}

TEST_CASE("frame synchroniser repeats, drops one at a time, and holds")
{
    FrameSynchroniser slow(0, 1000000000);
    std::vector<SourceFrame> frames;
    int repeats = 0;
    int drops = 0;
    for (int i = 0; i < 20; ++i)
    {
        if (i % 2 == 0)
        {
            frames.push_back(SourceFrame{i / 2, static_cast<std::int64_t>(i / 2) * 25000000});
        }
        auto const decision = slow.choose(static_cast<std::int64_t>(i) * 20000000, frames);
        slow.commit(decision);
        repeats += decision.repeat ? 1 : 0;
        drops += decision.dropped;
        CHECK(decision.dropped <= 1);
    }
    CHECK(repeats > 0);
    CHECK(drops == 0);

    FrameSynchroniser fast(0, 1000000000);
    frames.clear();
    repeats = 0;
    drops = 0;
    int produced = 0;
    for (int i = 0; i < 30; ++i)
    {
        while (produced * 16 <= i * 20)
        {
            frames.push_back(SourceFrame{produced, static_cast<std::int64_t>(produced) * 16000000});
            ++produced;
        }
        auto const decision = fast.choose(static_cast<std::int64_t>(i) * 20000000, frames);
        fast.commit(decision);
        repeats += decision.repeat ? 1 : 0;
        drops += decision.dropped;
        CHECK(decision.dropped <= 1);
        CHECK_FALSE(decision.slate);
    }
    CHECK(drops > 0);
    CHECK(repeats == 0);

    FrameSynchroniser held(120000000, 500000000);
    frames = {SourceFrame{0, 0}};
    auto const fresh = held.choose(200000000, frames);
    CHECK_FALSE(fresh.slate);
    auto const late = held.choose(900000000, frames);
    CHECK(late.slate);
}

TEST_CASE("channel map applies gain and silence")
{
    std::vector<float> stereo(8);
    for (int i = 0; i < 4; ++i)
    {
        stereo[static_cast<std::size_t>(i * 2)] = 1.f;
        stereo[static_cast<std::size_t>(i * 2 + 1)] = 0.5f;
    }
    AudioOutput output = presetIngest("sequential", 2);
    output.routes[0][0].gainDb = -6.0;
    output.routes[1][0].mute = true;
    TrackView track;
    track.channels = 2;
    track.samples = stereo.data();
    std::vector<float> mixed(8, 1.f);
    applyMatrix({track}, output, 4, mixed.data());
    CHECK(mixed[0] == doctest::Approx(dbToLinear(-6.0)).epsilon(0.001));
    CHECK(mixed[1] == doctest::Approx(0.0));
    TrackView missing = track;
    missing.missing = true;
    std::vector<float> silent(4, 1.f);
    applyMatrix({missing}, output, 2, silent.data());
    CHECK(silent[0] == doctest::Approx(0.0));
    CHECK(silent[1] == doctest::Approx(0.0));

    auto const down = presetIngest("5.1-stereo-downmix", 2);
    CHECK(down.routes[0].size() == 3);
    auto const stereo16 = presetIngest("16ch-stereo", 16);
    CHECK(stereo16.routes[15][0].track == 7);
    CHECK(stereo16.routes[15][0].channel == 1);
    auto const s302 = presetIngest("302m-16", 16);
    CHECK(s302.routes[8][0].track == 1);
    CHECK(s302.routes[8][0].channel == 0);
    auto const egress = presetEgress("16ch-302m");
    CHECK(egress.size() == 2);
    CHECK(egress[1].channels.front() == 8);
    CHECK(defaultAudioBitrate("aac", "stereo") == 192000);
    CHECK(defaultAudioBitrate("aac", "5.1") == 384000);
}

TEST_CASE("access control rejects the wrong streamid and rate limits")
{
    AccessPolicy policy;
    policy.exposure = "internet";
    policy.mode = "listener";
    policy.passphraseSet = true;
    policy.pbkeylen = 0;
    policy.acceptedStreamIds = {"news"};
    policy.peerAllow = {"10.1.0.0/16"};
    CHECK(effectiveKeyLength(policy) == 32);
    AccessInput input;
    input.peerIp = "10.1.2.3";
    input.streamId = "other";
    input.nowMs = 1000;
    auto const denied = evaluateAccess(policy, input, nullptr);
    CHECK_FALSE(denied.accept);
    CHECK(denied.reason == "streamid");
    input.streamId = "news";
    CHECK(evaluateAccess(policy, input, nullptr).accept);
    input.peerIp = "192.168.1.9";
    CHECK(evaluateAccess(policy, input, nullptr).reason == "peer_allow");
    AttemptLimiter limiter;
    input.peerIp = "10.1.2.3";
    for (int i = 0; i < 10; ++i)
    {
        limiter.record(input.peerIp, 1000 + i);
    }
    auto const limited = evaluateAccess(policy, input, &limiter);
    CHECK(limited.reason == "rate_limit");
    input.cryptoRejected = true;
    AccessPolicy open;
    auto const crypto = evaluateAccess(open, input, nullptr);
    CHECK(crypto.reason == "passphrase");
}

TEST_CASE("v210 stride and slate are sized for the raster")
{
    CHECK(v210Stride(1920) == 5120);
    CHECK(v210Size(1920, 1080) == 5120U * 1080U);
    auto const slate = renderSlate(320, 180, "CAM 1", true);
    CHECK(slate.size() == v210Size(320, 180));
    bool ink = false;
    for (auto byte : slate)
    {
        if (byte != 0)
        {
            ink = true;
            break;
        }
    }
    CHECK(ink);
}

TEST_CASE("metrics render the channel set")
{
    ProcessMetrics metrics;
    ChannelMetrics channel;
    channel.id = "in1";
    channel.state = stateCode("running");
    channel.videoFrames = 10;
    channel.repeats = 2;
    channel.decoder = "h264-cpu";
    channel.targetFormat = "1080p50";
    channel.encodeLatencyCount = 4;
    channel.encodeLatencySum = 0.04;
    channel.encodeLatencyBuckets = {1, 3, 0, 0, 0, 0, 0, 0};
    metrics.channels.push_back(channel);
    metrics.nvdecSessions = 1;
    auto const text = renderMetrics(metrics);
    CHECK(text.find("mxl_srt_gateway_channel_state{channel=\"in1\"} 4") != std::string::npos);
    CHECK(text.find("mxl_srt_gateway_framesync_repeats_total{channel=\"in1\"} 2") != std::string::npos);
    CHECK(text.find("mxl_srt_gateway_codec_info{channel=\"in1\"") != std::string::npos);
    CHECK(text.find("mxl_srt_gateway_nvdec_sessions 1") != std::string::npos);
    CHECK(stateCode("no_signal") == 3);
}
