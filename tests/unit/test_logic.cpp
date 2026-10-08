#include "config/config.hpp"
#include "config/store.hpp"
#include "mxl/domain.hpp"
#include "media/adapt.hpp"
#include "media/format.hpp"
#include "media/framesync.hpp"
#include "media/matrix.hpp"
#include "media/slate.hpp"
#include "media/v210.hpp"
#include "nmos/ids.hpp"
#include "ops/metrics.hpp"
#include "srt/access.hpp"
#include "util/jsonutil.hpp"
#include "util/uuid.hpp"
#include "version.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
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

TEST_CASE("platform settings keep aliases and reject bad announce addresses")
{
    auto const host = loadFromSources({}, "", {{"NMOS_HOST_ADDRESS", "10.1.2.3"}, {"SRTGW_PUBLIC_IP", "10.9.9.9"}});
    CHECK(host.config.hostAddress == "10.1.2.3");
    auto const alias = loadFromSources({}, "", {{"SRTGW_PUBLIC_IP", "10.4.5.6"}});
    CHECK(alias.config.hostAddress == "10.4.5.6");
    CHECK_THROWS_AS(loadFromSources({}, "", {{"NMOS_HOST_ADDRESS", "srtgw.example"}}), ConfigError);
    CHECK_THROWS_AS(loadFromSources({}, "", {{"SRTGW_PUBLIC_IP", "127.0.0.1"}}), ConfigError);
    CHECK_THROWS_AS(loadFromSources({}, "", {{"NMOS_HOST_ADDRESS", "0.0.0.0"}}), ConfigError);

    auto const ms = loadFromSources({}, "", {{"MXL_HISTORY_DURATION_MS", "2500"}, {"SRTGW_HISTORY_DURATION_NS", "5"}});
    CHECK(ms.config.historyDurationNs == 2500LL * 1000000LL);
    auto const ns = loadFromSources({}, "", {{"SRTGW_HISTORY_DURATION_NS", "2000000000"}});
    CHECK(ns.config.historyDurationNs == 2000000000LL);

    auto const query = loadFromSources({}, "", {{"NMOS_REGISTRY_PORT", "4000"}});
    CHECK(query.config.nmosQueryPort == 4001);
    CHECK(query.config.nmosQueryAddress.empty());
    auto const querySet = loadFromSources({}, "", {{"NMOS_REGISTRY_ADDRESS", "10.0.0.8"}, {"NMOS_QUERY_ADDRESS", "10.0.0.9"}, {"NMOS_QUERY_PORT", "4002"}});
    CHECK(querySet.config.nmosQueryAddress == "10.0.0.9");
    CHECK(querySet.config.nmosQueryPort == 4002);

    CHECK_THROWS_AS(loadFromSources({}, "", {{"NMOS_TAGS", "[1]"}}), ConfigError);
    auto const tags = loadFromSources({}, "", {{"NMOS_TAGS", "{\"urn:x-srf:production\":[\"sport-sa\"],\"urn:x-srf:function\":[\"srt1\"]}"}});
    CHECK(tags.config.nmosTagsJson.find("sport-sa") != std::string::npos);
    CHECK(tags.config.cleanupOnExit == false);
    CHECK(loadFromSources({}, "", {{"MXL_CLEANUP_ON_EXIT", "true"}}).config.cleanupOnExit);
    CHECK(loadFromSources({}, "", {}).config.stateDir == "/config");
    CHECK(loadFromSources({}, "", {}).config.shutdownTimeoutS == 10);
    CHECK(loadFromSources({}, "", {}).config.nmosDnsSd == false);

    auto const a = makeNmosIds("sport-sa-srtgw");
    auto const b = makeNmosIds("sport-sa-srtgw");
    CHECK(a.node == b.node);
    CHECK(a.domain == b.domain);
    CHECK(a.node != makeNmosIds("other").node);
}

TEST_CASE("output domain files are not overwritten and cleanup stays inside the domain")
{
    auto const root = std::filesystem::temp_directory_path() / "srtgw-domain-test";
    std::filesystem::remove_all(root);
    auto const path = root / "own";
    std::string error;
    CHECK(ensureOutputDomain(path.string(), "11111111-1111-4111-8111-111111111111", 1000000000, &error));
    auto const before = std::filesystem::file_size(path / "domain_def.json");
    CHECK(ensureOutputDomain(path.string(), "11111111-1111-4111-8111-111111111111", 1000000000, &error));
    CHECK(std::filesystem::file_size(path / "domain_def.json") == before);
    CHECK_FALSE(ensureOutputDomain(path.string(), "22222222-2222-4222-8222-222222222222", 1000000000, &error));
    CHECK(error.find("refusing") != std::string::npos);
    std::ifstream in(path / "domain_def.json");
    std::string text;
    std::getline(in, text);
    CHECK(text.find("11111111-1111-4111-8111-111111111111") != std::string::npos);
    // BCP-007-03 schema: id, label, description and tags are required.
    CHECK(text.find("\"label\":\"") != std::string::npos);
    CHECK(text.find("\"description\":\"") != std::string::npos);
    CHECK(text.find("\"tags\":{}") != std::string::npos);
    CHECK_FALSE(removeOwnDomain(path.string(), "22222222-2222-4222-8222-222222222222"));
    CHECK(std::filesystem::exists(path / "domain_def.json"));
    CHECK(removeOwnDomain(path.string(), "11111111-1111-4111-8111-111111111111"));
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK_FALSE(removeOwnDomain("/", "11111111-1111-4111-8111-111111111111"));
    std::filesystem::remove_all(root);
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

TEST_CASE("v210 fields: one parity of the rows, and back")
{
    for (auto const& raster : {std::pair{1920, 1080}, std::pair{718, 9}})
    {
        int const width = raster.first;
        int const height = raster.second;
        CAPTURE(height);
        int const cw = (width + 1) / 2;
        int const ch = (height + 1) / 2;
        std::vector<std::uint8_t> y(static_cast<std::size_t>(width * height));
        std::vector<std::uint8_t> cb(static_cast<std::size_t>(cw * ch));
        std::vector<std::uint8_t> cr(cb.size());
        for (std::size_t i = 0; i < y.size(); ++i)
        {
            y[i] = static_cast<std::uint8_t>(i * 7 + i / 1000);
        }
        for (std::size_t i = 0; i < cb.size(); ++i)
        {
            cb[i] = static_cast<std::uint8_t>(i * 13);
            cr[i] = static_cast<std::uint8_t>(i * 5 + 3);
        }
        auto const stride = v210Stride(width);
        std::vector<std::uint8_t> frame(v210Size(width, height));
        yuv420ToV210(y.data(), width, cb.data(), cr.data(), cw, width, height, true, frame.data(), stride);
        std::vector<std::vector<std::uint8_t>> fields;
        for (int parity : {0, 1})
        {
            std::vector<std::uint8_t> direct(stride * static_cast<std::size_t>((height + 1 - parity) / 2));
            yuv420ToV210(y.data(), width, cb.data(), cr.data(), cw, width, height, true, direct.data(), stride, parity);
            std::vector<std::uint8_t> cut(direct.size());
            copyV210Field(frame.data(), stride, height, parity, cut.data());
            CHECK(direct == cut);
            fields.push_back(direct);
        }
        std::vector<std::uint8_t> woven(frame.size());
        interleaveV210Fields(fields[0].data(), fields[1].data(), stride, height, woven.data());
        CHECK(woven == frame);
    }
}

TEST_CASE("the x264 thread count is kept and clamped")
{
    auto const channel = channelFromJson("{\"id\":\"out1\",\"direction\":\"egress\",\"egress\":{\"threads\":4}}", nullptr);
    CHECK(channel.egress.threads == 4);
    CHECK(channelFromJson(channelToJson(channel, true), nullptr).egress.threads == 4);
    CHECK(channelFromJson("{\"id\":\"out1\",\"direction\":\"egress\",\"egress\":{\"threads\":500}}", nullptr).egress.threads == 64);
    CHECK(channelFromJson("{\"id\":\"out1\",\"direction\":\"egress\"}", nullptr).egress.threads == 0);
}

TEST_CASE("the x264 preset does not replace the egress audio tracks")
{
    auto const channel = channelFromJson(
        "{\"id\":\"out1\",\"direction\":\"egress\",\"egress\":{\"preset\":\"veryfast\",\"audio_tracks\":[{\"codec\":\"aac\",\"layout\":\"stereo\",\"channels\":[0,1],\"language\":\"eng\"},{\"codec\":\"mp2\",\"layout\":\"stereo\",\"channels\":[2,3]}]}}",
        nullptr);
    CHECK(channel.egress.preset == "veryfast");
    REQUIRE(channel.egress.audioTracks.size() == 2);
    // A saved channel (the config file, GET /channels) carries both and loads the same tracks.
    auto const again = channelFromJson(channelToJson(channel, true), nullptr);
    REQUIRE(again.egress.audioTracks.size() == 2);
    CHECK(again.egress.audioTracks[0].language == "eng");
    CHECK(again.egress.audioTracks[1].codec == "mp2");
    auto const preset = channelFromJson("{\"id\":\"out1\",\"egress\":{\"audio_preset\":\"8x-stereo-aac\"}}", &channel);
    CHECK(preset.egress.audioTracks.size() == 8);
    CHECK(preset.egress.preset == "veryfast");
    auto const legacy = channelFromJson("{\"id\":\"out1\",\"egress\":{\"preset\":\"16ch-302m\"}}", &channel);
    CHECK(legacy.egress.audioTracks.size() == 2);
    CHECK(legacy.egress.audioTracks[0].codec == "s302m");
    CHECK(legacy.egress.preset == "veryfast");
}

TEST_CASE("saved settings keep the environment and flag a restart only for an accepted change")
{
    ConfigStore store(loadFromSources({{"LOG_LEVEL", "info"}}, "[]", {{"WEB_PORT", "8300"}, {"NMOS_HOST_ADDRESS", "10.1.2.3"}}));
    store.replaceGlobals({{"LOG_LEVEL", "debug"}, {"DECODER", "auto"}});
    auto snapshot = store.snapshot();
    CHECK(snapshot.config.logLevel == "debug");
    CHECK(snapshot.config.webPort == 8300);
    CHECK(snapshot.origin.at("WEB_PORT") == ValueOrigin::Env);
    CHECK(snapshot.origin.at("LOG_LEVEL") == ValueOrigin::File);
    CHECK_FALSE(snapshot.restartRequired);
    CHECK_THROWS_AS(store.replaceGlobals({{"DECODER", "gpu"}}), ConfigError);
    CHECK_FALSE(store.snapshot().restartRequired);
    CHECK(store.snapshot().config.decoder == "auto");
    store.replaceGlobals({{"DECODER", "cpu"}});
    CHECK(store.snapshot().restartRequired);
    CHECK(store.snapshot().config.decoder == "cpu");
}

TEST_CASE("info names the versions, the node label and the config file")
{
    auto loaded = loadFromSources({}, "[]", {{"HOST_ID", "lab-1"}, {"NMOS_HOST_ADDRESS", "10.1.2.3"}});
    loaded.config.configFile = "/config/gateway.json";
    std::string error;
    auto const info = json::parse(infoJson(loaded.config), &error);
    CHECK(error.empty());
    CHECK(json::fieldString(info, "version") == kVersion);
    CHECK(json::fieldString(info, "mxl") == kMxlRef);
    CHECK(json::fieldString(info, "label") == "lab-1");
    CHECK(json::fieldString(info, "host_address") == "10.1.2.3");
    CHECK(json::fieldString(info, "config_file") == "/config/gateway.json");
    auto const labelled = loadFromSources({}, "[]", {{"NMOS_LABEL", "Edge SRT"}, {"NMOS_HOST_ADDRESS", "10.1.2.3"}});
    CHECK(json::fieldString(json::parse(infoJson(labelled.config), &error), "label") == "Edge SRT");
}

TEST_CASE("the domain list carries the flows and marks the own domain")
{
    auto const root = std::filesystem::temp_directory_path() / "srtgw-flows-test";
    std::filesystem::remove_all(root);
    auto const video = root / "player" / "aaaaaaaa-0000-4000-8000-000000000001.mxl-flow";
    auto const audio = root / "player" / "aaaaaaaa-0000-4000-8000-000000000002.mxl-flow";
    std::filesystem::create_directories(video);
    std::filesystem::create_directories(audio);
    std::filesystem::create_directories(root / "player" / "not-a-flow");
    std::ofstream(root / "player" / "domain_def.json") << "{\"id\":\"11111111-1111-4111-8111-111111111111\",\"label\":\"Player\"}";
    std::ofstream(video / "flow_def.json")
        << "{\"id\":\"aaaaaaaa-0000-4000-8000-000000000001\",\"label\":\"Cam 1\",\"format\":\"urn:x-nmos:format:video\",\"media_type\":\"video/v210\","
           "\"frame_width\":1920,\"frame_height\":1080,\"interlace_mode\":\"progressive\",\"grain_rate\":{\"numerator\":50,\"denominator\":1}}";
    std::ofstream(audio / "flow_def.json")
        << "{\"id\":\"aaaaaaaa-0000-4000-8000-000000000002\",\"label\":\"Cam 1 audio\",\"format\":\"urn:x-nmos:format:audio\",\"media_type\":\"audio/float32\",\"channel_count\":16}";
    std::string error;
    auto const listed = json::parse(domainsJson({DomainRecord{(root / "player").string(), "11111111-1111-4111-8111-111111111111", false}}, "11111111-1111-4111-8111-111111111111"), &error);
    CHECK(error.empty());
    auto const domains = json::field(listed, "domains");
    REQUIRE(domains);
    REQUIRE(domains->get<picojson::array>().size() == 1);
    auto const& domain = domains->get<picojson::array>()[0];
    CHECK(json::fieldString(domain, "label") == "Player");
    CHECK(json::fieldBool(domain, "own", false));
    auto const flows = json::field(domain, "flows");
    REQUIRE(flows);
    REQUIRE(flows->get<picojson::array>().size() == 2);
    int width = 0;
    int channels = 0;
    for (auto const& flow : flows->get<picojson::array>())
    {
        width = std::max(width, json::fieldInt(flow, "frame_width", 0));
        channels = std::max(channels, json::fieldInt(flow, "channel_count", 0));
    }
    CHECK(width == 1920);
    CHECK(channels == 16);
    std::filesystem::remove_all(root);
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

    // keepCuda: a chain that runs on the GPU end to end stays there; one that needs a
    // CPU step still downloads and ends in yuv422p10le.
    auto const same = planAdaptation(p50, p50, "bwdif", "letterbox", "bicubic", "auto");
    CHECK(ffmpegFilter(same, true, true) == "null");
    CHECK(ffmpegFilter(same, true).find("hwdownload") != std::string::npos);
    auto const fieldKept = ffmpegFilter(field, true, true);
    CHECK(fieldKept.find("bwdif_cuda=mode=send_field") != std::string::npos);
    CHECK(fieldKept.find("hwdownload") == std::string::npos);
    CHECK(fieldKept.find("format=yuv422p10le") == std::string::npos);
    auto const scaledKept = ffmpegFilter(scaled, true, true);
    CHECK(scaledKept.find("hwdownload") < scaledKept.find("crop=1280:720"));
    CHECK(scaledKept.find("format=yuv422p10le") != std::string::npos);
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

TEST_CASE("frame synchroniser resyncs when the next frames have left the buffer")
{
    // Output last showed frame 10; the decoder kept going and the buffer now
    // holds only frames 20..27 (eight frames, like the ingest queue).
    FrameSynchroniser sync(0, 1000000000);
    sync.commit(SyncDecision{false, false, 0, 10});
    std::vector<SourceFrame> frames;
    for (int i = 20; i < 28; ++i)
    {
        frames.push_back(SourceFrame{i, static_cast<std::int64_t>(i) * 20000000});
    }
    auto const decision = sync.choose(27 * 20000000, frames);
    CHECK_FALSE(decision.slate);
    CHECK(decision.sourceIndex == 27);
    CHECK(decision.dropped == 16);
    sync.commit(decision);
    frames.push_back(SourceFrame{28, 28 * 20000000});
    auto const next = sync.choose(28 * 20000000, frames);
    CHECK(next.sourceIndex == 28);
    CHECK(next.dropped == 0);
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
    // Rows padded to 48 pixels (128 bytes), as MXL lays them out.
    CHECK(v210Stride(1280) == 3456);
    CHECK(v210Stride(720) == 1920);
    CHECK(v210Stride(3840) == 10240);
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

namespace
{
// The CUDA kernels nv12ToV210 and v210ToNv12 (v210_cuda.cu), one 6-pixel group at a time, on
// planar chroma: the reference for the CPU conversions.
int refClamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void refChromaRows(int y, int chromaHeight, bool interlaced, int* near, int* far)
{
    if (interlaced)
    {
        int const field = y & 1;
        int const fieldRow = y >> 1;
        int const fieldChroma = (chromaHeight + 1 - field) / 2;
        int const c = fieldRow >> 1;
        int const other = (fieldRow & 1) ? c + 1 : c - 1;
        *near = std::min(refClamp(c, 0, fieldChroma - 1) * 2 + field, chromaHeight - 1);
        *far = std::min(refClamp(other, 0, fieldChroma - 1) * 2 + field, chromaHeight - 1);
        return;
    }
    int const c = y >> 1;
    *near = c;
    *far = refClamp((y & 1) ? c + 1 : c - 1, 0, chromaHeight - 1);
}

std::uint8_t refTo8(std::uint32_t v)
{
    std::uint32_t const r = (v + 2) >> 2;
    return static_cast<std::uint8_t>(r > 255 ? 255 : r);
}

std::vector<std::uint8_t> refToV210(std::vector<std::uint8_t> const& y, std::vector<std::uint8_t> const& cb, std::vector<std::uint8_t> const& cr, int width,
    int height, bool interlaced)
{
    int const cw = (width + 1) / 2;
    std::vector<std::uint8_t> out(v210Size(width, height));
    for (int row = 0; row < height; ++row)
    {
        int near = 0;
        int far = 0;
        refChromaRows(row, (height + 1) / 2, interlaced, &near, &far);
        for (int group = 0; group < (width + 5) / 6; ++group)
        {
            std::uint32_t ys[6];
            std::uint32_t bs[3];
            std::uint32_t rs[3];
            for (int i = 0; i < 6; ++i)
            {
                ys[i] = static_cast<std::uint32_t>(y[static_cast<std::size_t>(row * width + refClamp(group * 6 + i, 0, width - 1))]) << 2;
            }
            for (int i = 0; i < 3; ++i)
            {
                int const cx = refClamp(group * 3 + i, 0, cw - 1);
                bs[i] = ((3U * cb[static_cast<std::size_t>(near * cw + cx)] + cb[static_cast<std::size_t>(far * cw + cx)] + 2U) >> 2) << 2;
                rs[i] = ((3U * cr[static_cast<std::size_t>(near * cw + cx)] + cr[static_cast<std::size_t>(far * cw + cx)] + 2U) >> 2) << 2;
            }
            std::uint32_t const w[4] = {bs[0] | (ys[0] << 10) | (rs[0] << 20), ys[1] | (bs[1] << 10) | (ys[2] << 20), rs[1] | (ys[3] << 10) | (bs[2] << 20),
                ys[4] | (rs[2] << 10) | (ys[5] << 20)};
            std::memcpy(out.data() + v210Stride(width) * static_cast<std::size_t>(row) + static_cast<std::size_t>(group) * 16U, w, sizeof(w));
        }
    }
    return out;
}

void refToYuv420(std::vector<std::uint8_t> const& src, int width, int height, bool interlaced, std::vector<std::uint8_t>& y, std::vector<std::uint8_t>& cb,
    std::vector<std::uint8_t>& cr)
{
    int const cw = (width + 1) / 2;
    int const ch = (height + 1) / 2;
    y.assign(static_cast<std::size_t>(width * height), 0);
    cb.assign(static_cast<std::size_t>(cw * ch), 0);
    cr.assign(cb.size(), 0);
    auto word = [&](int row, int group, int k) {
        std::uint32_t w = 0;
        std::memcpy(&w, src.data() + v210Stride(width) * static_cast<std::size_t>(row) + static_cast<std::size_t>(group) * 16U + static_cast<std::size_t>(k) * 4U, 4);
        return w;
    };
    for (int chromaRow = 0; chromaRow < ch; ++chromaRow)
    {
        for (int group = 0; group < (width + 5) / 6; ++group)
        {
            int rowA = 2 * chromaRow;
            int rowB = rowA + 1;
            if (interlaced)
            {
                int const field = chromaRow & 1;
                int const fieldRow = (chromaRow >> 1) * 2;
                rowA = (fieldRow * 2) + field;
                rowB = rowA + 2;
            }
            rowA = refClamp(rowA, 0, height - 1);
            rowB = refClamp(rowB, 0, height - 1);
            std::uint32_t const cbA[3] = {word(rowA, group, 0) & 0x3ff, (word(rowA, group, 1) >> 10) & 0x3ff, (word(rowA, group, 2) >> 20) & 0x3ff};
            std::uint32_t const crA[3] = {(word(rowA, group, 0) >> 20) & 0x3ff, word(rowA, group, 2) & 0x3ff, (word(rowA, group, 3) >> 10) & 0x3ff};
            std::uint32_t const cbB[3] = {word(rowB, group, 0) & 0x3ff, (word(rowB, group, 1) >> 10) & 0x3ff, (word(rowB, group, 2) >> 20) & 0x3ff};
            std::uint32_t const crB[3] = {(word(rowB, group, 0) >> 20) & 0x3ff, word(rowB, group, 2) & 0x3ff, (word(rowB, group, 3) >> 10) & 0x3ff};
            for (int i = 0; i < 3; ++i)
            {
                int const cx = group * 3 + i;
                if (2 * cx < width)
                {
                    cb[static_cast<std::size_t>(chromaRow * cw + cx)] = refTo8((cbA[i] + cbB[i] + 1) >> 1);
                    cr[static_cast<std::size_t>(chromaRow * cw + cx)] = refTo8((crA[i] + crB[i] + 1) >> 1);
                }
            }
            for (int row = 2 * chromaRow; row < 2 * chromaRow + 2 && row < height; ++row)
            {
                std::uint32_t const ys[6] = {(word(row, group, 0) >> 10) & 0x3ff, word(row, group, 1) & 0x3ff, (word(row, group, 1) >> 20) & 0x3ff,
                    (word(row, group, 2) >> 10) & 0x3ff, word(row, group, 3) & 0x3ff, (word(row, group, 3) >> 20) & 0x3ff};
                for (int i = 0; i < 6 && group * 6 + i < width; ++i)
                {
                    y[static_cast<std::size_t>(row * width + group * 6 + i)] = refTo8(ys[i]);
                }
            }
        }
    }
}
} // namespace

TEST_CASE("v210 to and from 4:2:0 as the CUDA kernels do")
{
    std::uint32_t seed = 12345;
    auto next = [&seed]() {
        seed = seed * 1664525U + 1013904223U;
        return seed >> 24;
    };
    struct Raster
    {
        int width;
        int height;
        bool interlaced;
    };
    std::vector<Raster> const rasters = {Raster{1920, 1080, false}, Raster{1920, 1080, true}, Raster{1280, 720, false}, Raster{718, 9, false},
        Raster{718, 9, true}, Raster{8, 5, true}, Raster{6, 2, false}, Raster{14, 4, false}};
    for (bool const simd : {true, false})
    {
        setV210Simd(simd);
        for (Raster const& r : rasters)
        {
            CAPTURE(simd);
            CAPTURE(r.width);
            CAPTURE(r.height);
            CAPTURE(r.interlaced);
            int const cw = (r.width + 1) / 2;
            int const ch = (r.height + 1) / 2;
            std::vector<std::uint8_t> y(static_cast<std::size_t>(r.width * r.height));
            std::vector<std::uint8_t> cb(static_cast<std::size_t>(cw * ch));
            std::vector<std::uint8_t> cr(cb.size());
            for (auto* plane : {&y, &cb, &cr})
            {
                for (auto& v : *plane)
                {
                    v = static_cast<std::uint8_t>(next());
                }
            }
            std::vector<std::uint8_t> packed(v210Size(r.width, r.height));
            yuv420ToV210(y.data(), r.width, cb.data(), cr.data(), cw, r.width, r.height, r.interlaced, packed.data(), v210Stride(r.width));
            CHECK(packed == refToV210(y, cb, cr, r.width, r.height, r.interlaced));

            // Back, from a v210 picture with every 10-bit code (not only multiples of 4).
            for (auto& v : packed)
            {
                v = static_cast<std::uint8_t>(next());
            }
            for (std::size_t i = 3; i < packed.size(); i += 4)
            {
                packed[i] &= 0x3f; // bits 30 and 31 are unused
            }
            std::vector<std::uint8_t> y8(y.size());
            std::vector<std::uint8_t> cb8(cb.size());
            std::vector<std::uint8_t> cr8(cb.size());
            v210ToYuv420(packed.data(), v210Stride(r.width), r.width, r.height, r.interlaced, y8.data(), r.width, cb8.data(), cr8.data(), cw);
            std::vector<std::uint8_t> refY;
            std::vector<std::uint8_t> refCb;
            std::vector<std::uint8_t> refCr;
            refToYuv420(packed, r.width, r.height, r.interlaced, refY, refCb, refCr);
            CHECK(y8 == refY);
            CHECK(cb8 == refCb);
            CHECK(cr8 == refCr);
        }
    }
    setV210Simd(true);
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
