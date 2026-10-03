#include "config/config.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "util/net.hpp"
#include "util/uuid.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace srtgw
{
namespace
{
std::string lower(std::string text)
{
    for (char& c : text)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

bool truthy(std::string const& text)
{
    auto const value = lower(text);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

int envInt(std::map<std::string, std::string> const& values, std::string const& key, int fallback, std::string* error)
{
    auto const it = values.find(key);
    if (it == values.end() || it->second.empty())
    {
        return fallback;
    }
    try
    {
        std::size_t used = 0;
        int const parsed = std::stoi(it->second, &used);
        if (used != it->second.size())
        {
            if (error != nullptr)
            {
                *error += key + " is not an integer; ";
            }
            return fallback;
        }
        return parsed;
    }
    catch (...)
    {
        if (error != nullptr)
        {
            *error += key + " is not an integer; ";
        }
        return fallback;
    }
}

void parsePortRange(std::string const& text, int* minPort, int* maxPort, std::string* error)
{
    auto const dash = text.find('-');
    if (dash == std::string::npos)
    {
        *error += "SRT_PORT_RANGE must look like 9000-9099; ";
        return;
    }
    try
    {
        *minPort = std::stoi(text.substr(0, dash));
        *maxPort = std::stoi(text.substr(dash + 1));
    }
    catch (...)
    {
        *error += "SRT_PORT_RANGE must look like 9000-9099; ";
        return;
    }
    if (*minPort < 1 || *maxPort > 65535 || *minPort > *maxPort)
    {
        *error += "SRT_PORT_RANGE is outside 1-65535; ";
    }
}

VideoFormat targetFromJson(picojson::value const& value, VideoFormat fallback)
{
    VideoFormat format = fallback;
    format.width = json::fieldInt(value, "width", format.width);
    format.height = json::fieldInt(value, "height", format.height);
    auto const scan = json::fieldString(value, "scan", format.interlaced ? "interlaced" : "progressive");
    format.interlaced = scan == "interlaced" || scan == "i";
    format.fieldOrder = json::fieldString(value, "field_order", format.interlaced ? "tff" : "progressive");
    auto const rateText = json::fieldString(value, "rate", format.rate.name);
    Rate parsed;
    if (parseRate(rateText, &parsed))
    {
        format.rate = parsed;
    }
    format.color = json::fieldString(value, "color", "bt709");
    return format;
}

picojson::value targetToJson(VideoFormat const& format)
{
    picojson::object obj;
    obj["width"] = picojson::value(static_cast<double>(format.width));
    obj["height"] = picojson::value(static_cast<double>(format.height));
    obj["scan"] = picojson::value(format.interlaced ? "interlaced" : "progressive");
    obj["field_order"] = picojson::value(format.fieldOrder);
    obj["rate"] = picojson::value(format.rate.name);
    obj["color"] = picojson::value(format.color);
    return picojson::value(obj);
}

SrtEndpointConfig srtFromJson(picojson::value const& value, SrtEndpointConfig fallback)
{
    SrtEndpointConfig srt = fallback;
    srt.mode = json::fieldString(value, "mode", srt.mode);
    srt.localAddress = json::fieldString(value, "local_address", srt.localAddress);
    srt.localPort = json::fieldInt(value, "local_port", srt.localPort);
    srt.remoteHost = json::fieldString(value, "remote_host", srt.remoteHost);
    srt.remotePort = json::fieldInt(value, "remote_port", srt.remotePort);
    srt.latencyMs = json::fieldInt(value, "latency_ms", srt.latencyMs);
    if (json::has(value, "passphrase"))
    {
        srt.passphrase = json::fieldString(value, "passphrase", "");
    }
    srt.pbkeylen = json::fieldInt(value, "pbkeylen", srt.pbkeylen);
    srt.streamId = json::fieldString(value, "streamid", srt.streamId);
    if (json::has(value, "accepted_streamids"))
    {
        srt.acceptedStreamIds = json::fieldStringList(value, "accepted_streamids");
    }
    srt.maxBandwidth = json::fieldInt64(value, "max_bandwidth", srt.maxBandwidth);
    srt.overhead = json::fieldInt(value, "overhead", srt.overhead);
    srt.payloadSize = json::fieldInt(value, "payload_size", srt.payloadSize);
    srt.connectTimeoutMs = json::fieldInt(value, "connect_timeout_ms", srt.connectTimeoutMs);
    if (json::has(value, "peer_allow"))
    {
        srt.peerAllow = json::fieldStringList(value, "peer_allow");
    }
    srt.exposure = json::fieldString(value, "exposure", srt.exposure);
    return srt;
}

picojson::value srtToJson(SrtEndpointConfig const& srt, bool includeSecrets)
{
    picojson::object obj;
    obj["mode"] = picojson::value(srt.mode);
    obj["local_address"] = picojson::value(srt.localAddress);
    obj["local_port"] = picojson::value(static_cast<double>(srt.localPort));
    obj["remote_host"] = picojson::value(srt.remoteHost);
    obj["remote_port"] = picojson::value(static_cast<double>(srt.remotePort));
    obj["latency_ms"] = picojson::value(static_cast<double>(srt.latencyMs));
    obj["passphrase_set"] = picojson::value(!srt.passphrase.empty());
    if (includeSecrets)
    {
        obj["passphrase"] = picojson::value(srt.passphrase);
    }
    obj["pbkeylen"] = picojson::value(static_cast<double>(srt.pbkeylen));
    obj["streamid"] = picojson::value(srt.streamId);
    obj["accepted_streamids"] = json::stringList(srt.acceptedStreamIds);
    obj["max_bandwidth"] = picojson::value(static_cast<double>(srt.maxBandwidth));
    obj["overhead"] = picojson::value(static_cast<double>(srt.overhead));
    obj["payload_size"] = picojson::value(static_cast<double>(srt.payloadSize));
    obj["connect_timeout_ms"] = picojson::value(static_cast<double>(srt.connectTimeoutMs));
    obj["peer_allow"] = json::stringList(srt.peerAllow);
    obj["exposure"] = picojson::value(srt.exposure);
    return picojson::value(obj);
}

picojson::value routesToJson(AudioOutput const& output)
{
    picojson::array routes;
    for (auto const& row : output.routes)
    {
        picojson::array taps;
        for (auto const& item : row)
        {
            picojson::object tap;
            tap["track"] = picojson::value(static_cast<double>(item.track));
            tap["channel"] = picojson::value(static_cast<double>(item.channel));
            tap["gain_db"] = picojson::value(item.gainDb);
            tap["mute"] = picojson::value(item.mute);
            taps.emplace_back(tap);
        }
        routes.emplace_back(taps);
    }
    return picojson::value(routes);
}

AudioOutput audioFromJson(picojson::value const& value)
{
    int const channels = json::fieldInt(value, "channels", 16);
    auto const preset = json::fieldString(value, "preset", "sequential");
    AudioOutput output = presetIngest(preset, channels);
    output.channels = channels;
    if (!json::has(value, "routes"))
    {
        return output;
    }
    output.routes.clear();
    auto const routes = json::field(value, "routes");
    if (!routes || !routes->is<picojson::array>())
    {
        return output;
    }
    for (auto const& row : routes->get<picojson::array>())
    {
        std::vector<RouteTap> taps;
        if (row.is<picojson::array>())
        {
            for (auto const& item : row.get<picojson::array>())
            {
                RouteTap tap;
                tap.track = json::fieldInt(item, "track", 0);
                tap.channel = json::fieldInt(item, "channel", 0);
                tap.gainDb = json::fieldDouble(item, "gain_db", 0.0);
                tap.mute = json::fieldBool(item, "mute", false);
                taps.push_back(tap);
            }
        }
        output.routes.push_back(std::move(taps));
    }
    return output;
}

EgressAudioTrack trackFromJson(picojson::value const& value)
{
    EgressAudioTrack track;
    track.codec = json::fieldString(value, "codec", "aac");
    track.layout = json::fieldString(value, "layout", "stereo");
    track.channels = json::fieldIntList(value, "channels");
    track.bitrate = json::fieldInt(value, "bitrate", 0);
    track.language = json::fieldString(value, "language", "und");
    track.gainDb = json::fieldDouble(value, "gain_db", 0.0);
    track.mute = json::fieldBool(value, "mute", false);
    track.pid = json::fieldInt(value, "pid", 0);
    if (track.channels.empty())
    {
        int const count = layoutChannels(track.layout);
        for (int i = 0; i < count; ++i)
        {
            track.channels.push_back(i);
        }
    }
    if (track.bitrate == 0)
    {
        track.bitrate = defaultAudioBitrate(track.codec, track.layout);
    }
    return track;
}

picojson::value trackToJson(EgressAudioTrack const& track)
{
    picojson::object obj;
    obj["codec"] = picojson::value(track.codec);
    obj["layout"] = picojson::value(track.layout);
    obj["channels"] = json::intList(track.channels);
    obj["bitrate"] = picojson::value(static_cast<double>(track.bitrate));
    obj["language"] = picojson::value(track.language);
    obj["gain_db"] = picojson::value(track.gainDb);
    obj["mute"] = picojson::value(track.mute);
    obj["pid"] = picojson::value(static_cast<double>(track.pid));
    return picojson::value(obj);
}

bool oneOf(std::string const& value, std::initializer_list<char const*> allowed)
{
    for (auto const* item : allowed)
    {
        if (value == item)
        {
            return true;
        }
    }
    return false;
}
} // namespace

ChannelConfig defaultIngest(std::string const& id, int port)
{
    ChannelConfig channel;
    channel.id = id;
    channel.label = id;
    channel.direction = "ingest";
    channel.srt.mode = "listener";
    channel.srt.localPort = port;
    channel.target.width = 1920;
    channel.target.height = 1080;
    channel.target.interlaced = false;
    channel.target.fieldOrder = "progressive";
    channel.target.rate = Rate{50, 1, "50"};
    channel.audioOutputs.push_back(presetIngest("sequential", 16));
    channel.egress.audioTracks = presetEgress("8x-stereo-aac");
    return channel;
}

ChannelConfig defaultEgress(std::string const& id)
{
    ChannelConfig channel = defaultIngest(id, 0);
    channel.direction = "egress";
    channel.srt.mode = "caller";
    channel.srt.localPort = 0;
    channel.egress.audioTracks = presetEgress("stereo");
    return channel;
}

std::vector<std::string> globalKeys()
{
    return {"HOST_ID", "MXL_DOMAIN_SCAN_PATH", "MXL_OUTPUT_DOMAIN_DIR", "MXL_OUTPUT_DOMAIN_ID", "DECODER", "ENCODER", "SRT_PORT_RANGE",
        "NMOS_REGISTRY_ADDRESS", "NMOS_REGISTRY_PORT", "NMOS_QUERY_ADDRESS", "NMOS_QUERY_PORT", "NMOS_DNS_SD", "NMOS_PORT", "NMOS_SEED",
        "NMOS_LABEL", "NMOS_TAGS", "WEB_PORT", "LOG_LEVEL", "NMOS_HOST_ADDRESS", "SRTGW_PUBLIC_IP", "MXL_HISTORY_DURATION_MS",
        "SRTGW_HISTORY_DURATION_NS", "STATE_DIR", "SHUTDOWN_TIMEOUT_S", "MXL_CLEANUP_ON_EXIT"};
}

bool isRestartKey(std::string const& key)
{
    return key != "LOG_LEVEL";
}

AccessPolicy policyFor(SrtEndpointConfig const& endpoint)
{
    AccessPolicy policy;
    policy.exposure = endpoint.exposure;
    policy.mode = endpoint.mode;
    policy.passphraseSet = !endpoint.passphrase.empty();
    policy.pbkeylen = endpoint.pbkeylen;
    policy.acceptedStreamIds = endpoint.acceptedStreamIds;
    policy.peerAllow = endpoint.peerAllow;
    return policy;
}

void validateConfig(Config const& config)
{
    std::string error;
    if (!oneOf(config.decoder, {"auto", "nvdec", "cpu"}))
    {
        error += "DECODER must be auto, nvdec or cpu; ";
    }
    if (!oneOf(config.encoder, {"auto", "nvenc", "cpu"}))
    {
        error += "ENCODER must be auto, nvenc or cpu; ";
    }
    if (config.webPort < 1 || config.webPort > 65535)
    {
        error += "WEB_PORT is invalid; ";
    }
    if (config.nmosPort < 1 || config.nmosPort > 65534)
    {
        error += "NMOS_PORT is invalid; ";
    }
    if (config.webPort == config.nmosPort || config.webPort == config.nmosPort + 1)
    {
        error += "WEB_PORT collides with the NMOS node port; ";
    }
    if (config.nmosQueryPort < 1 || config.nmosQueryPort > 65535)
    {
        error += "NMOS_QUERY_PORT is invalid; ";
    }
    if (config.shutdownTimeoutS < 1 || config.shutdownTimeoutS > 600)
    {
        error += "SHUTDOWN_TIMEOUT_S is invalid; ";
    }
    if (config.stateDir.empty() || config.stateDir.front() != '/')
    {
        error += "STATE_DIR must be an absolute path; ";
    }
    if (!config.mxlOutputDomainId.empty() && !isUuid(config.mxlOutputDomainId))
    {
        error += "MXL_OUTPUT_DOMAIN_ID is not a UUID; ";
    }
    if (!oneOf(config.logLevel, {"error", "warn", "warning", "info", "debug", "trace"}))
    {
        error += "LOG_LEVEL is invalid; ";
    }
    std::map<int, std::string> ports;
    std::map<std::string, int> ids;
    for (auto const& channel : config.channels)
    {
        if (channel.id.empty() || channel.id.find('/') != std::string::npos)
        {
            error += "channel id is empty or contains '/'; ";
            continue;
        }
        if (++ids[channel.id] > 1)
        {
            error += "duplicate channel id " + channel.id + "; ";
        }
        if (!oneOf(channel.direction, {"ingest", "egress"}))
        {
            error += channel.id + " direction must be ingest or egress; ";
        }
        if (!oneOf(channel.srt.mode, {"caller", "listener", "rendezvous"}))
        {
            error += channel.id + " srt mode is invalid; ";
        }
        if (!oneOf(channel.srt.exposure, {"internal", "internet"}))
        {
            error += channel.id + " exposure must be internal or internet; ";
        }
        if (channel.srt.mode != "caller" && channel.srt.localPort != 0)
        {
            if (channel.srt.localPort < config.srtPortMin || channel.srt.localPort > config.srtPortMax)
            {
                error += channel.id + " listener port is outside SRT_PORT_RANGE; ";
            }
            if (!ports.emplace(channel.srt.localPort, channel.id).second)
            {
                error += "listener port " + std::to_string(channel.srt.localPort) + " is used twice; ";
            }
        }
        if (channel.srt.mode != "listener" && (channel.srt.remoteHost.empty() || channel.srt.remotePort <= 0))
        {
            error += channel.id + " caller requires remote_host and remote_port; ";
        }
        if (channel.srt.exposure == "internet" && channel.srt.passphrase.empty())
        {
            error += channel.id + " internet exposure requires a passphrase; ";
        }
        if (channel.srt.exposure == "internet" && channel.srt.mode == "listener" && channel.srt.acceptedStreamIds.empty())
        {
            error += channel.id + " internet listener requires an accepted streamid list; ";
        }
        if (channel.srt.pbkeylen != 0 && channel.srt.pbkeylen != 16 && channel.srt.pbkeylen != 24 && channel.srt.pbkeylen != 32)
        {
            error += channel.id + " pbkeylen must be 0, 16, 24 or 32; ";
        }
        if (channel.srt.payloadSize < 100 || channel.srt.payloadSize > 1456)
        {
            error += channel.id + " payload size is invalid; ";
        }
        if (!allowedRaster(channel.target.width, channel.target.height) || !allowedInterlace(channel.target))
        {
            error += channel.id + " target format is not a supported raster/rate; ";
        }
        if (!oneOf(channel.deinterlacer, {"bwdif", "yadif", "weave"}))
        {
            error += channel.id + " deinterlacer is invalid; ";
        }
        if (!oneOf(channel.aspect, {"letterbox", "fill"}))
        {
            error += channel.id + " aspect must be letterbox or fill; ";
        }
        if (!oneOf(channel.lossMode, {"slate", "black"}))
        {
            error += channel.id + " loss mode must be slate or black; ";
        }
        if (channel.audioOutputs.empty())
        {
            error += channel.id + " needs an audio output; ";
        }
        for (auto const& output : channel.audioOutputs)
        {
            if (output.channels < 2 || output.channels > 64)
            {
                error += channel.id + " audio channel count must be 2-64; ";
            }
        }
        if (channel.direction == "egress")
        {
            if (!oneOf(channel.egress.codec, {"h264", "hevc"}))
            {
                error += channel.id + " video codec must be h264 or hevc; ";
            }
            if (channel.egress.audioTracks.size() > 16)
            {
                error += channel.id + " has more than 16 audio tracks; ";
            }
            if (channel.egress.codec == "hevc" && channel.target.interlaced)
            {
                error += channel.id + " HEVC interlaced encoding is not supported; ";
            }
            if (!oneOf(channel.egress.mux, {"cbr", "vbr"}))
            {
                error += channel.id + " mux mode must be cbr or vbr; ";
            }
        }
        if (!channel.decoder.empty() && !oneOf(channel.decoder, {"auto", "nvdec", "cpu"}))
        {
            error += channel.id + " decoder override is invalid; ";
        }
        if (!channel.encoder.empty() && !oneOf(channel.encoder, {"auto", "nvenc", "cpu"}))
        {
            error += channel.id + " encoder override is invalid; ";
        }
    }
    if (!error.empty())
    {
        throw ConfigError(error);
    }
}

std::string assignListenerPort(Config const& config, ChannelConfig const& channel)
{
    if (channel.srt.mode == "caller" || channel.srt.localPort != 0)
    {
        return {};
    }
    for (int port = config.srtPortMin; port <= config.srtPortMax; ++port)
    {
        bool used = false;
        for (auto const& other : config.channels)
        {
            if (other.id != channel.id && other.srt.mode != "caller" && other.srt.localPort == port)
            {
                used = true;
                break;
            }
        }
        if (!used)
        {
            return std::to_string(port);
        }
    }
    return {};
}

LoadedConfig loadFromSources(std::map<std::string, std::string> const& fileValues, std::string const& channelsJsonFromFile,
    std::map<std::string, std::string> const& envValues)
{
    LoadedConfig loaded;
    auto take = [&](std::string const& key, std::string const& fallback) {
        if (envValues.count(key) != 0)
        {
            loaded.origin[key] = ValueOrigin::Env;
            return envValues.at(key);
        }
        if (fileValues.count(key) != 0)
        {
            loaded.origin[key] = ValueOrigin::File;
            return fileValues.at(key);
        }
        loaded.origin[key] = ValueOrigin::Default;
        return fallback;
    };

    std::string error;
    auto const hostDefault = hostname();
    loaded.config.hostId = take("HOST_ID", hostDefault);
    loaded.config.mxlDomainScanPath = take("MXL_DOMAIN_SCAN_PATH", "/Volumes/mxl");
    loaded.config.decoder = lower(take("DECODER", "auto"));
    loaded.config.encoder = lower(take("ENCODER", "auto"));
    loaded.config.nmosRegistryAddress = take("NMOS_REGISTRY_ADDRESS", "");
    loaded.config.nmosRegistryPort = envInt({{"NMOS_REGISTRY_PORT", take("NMOS_REGISTRY_PORT", "3210")}}, "NMOS_REGISTRY_PORT", 3210, &error);
    loaded.config.nmosQueryAddress = take("NMOS_QUERY_ADDRESS", "");
    if (loaded.config.nmosQueryAddress.empty())
    {
        loaded.config.nmosQueryAddress = loaded.config.nmosRegistryAddress;
        loaded.origin["NMOS_QUERY_ADDRESS"] = ValueOrigin::Default;
    }
    auto const querySet = envValues.count("NMOS_QUERY_PORT") != 0 || fileValues.count("NMOS_QUERY_PORT") != 0;
    if (querySet)
    {
        loaded.config.nmosQueryPort = envInt({{"NMOS_QUERY_PORT", take("NMOS_QUERY_PORT", "0")}}, "NMOS_QUERY_PORT", 0, &error);
    }
    else
    {
        loaded.config.nmosQueryPort = loaded.config.nmosRegistryPort + 1;
        loaded.origin["NMOS_QUERY_PORT"] = ValueOrigin::Default;
    }
    loaded.config.nmosDnsSd = truthy(take("NMOS_DNS_SD", "false"));
    loaded.config.nmosPort = envInt({{"NMOS_PORT", take("NMOS_PORT", "3272")}}, "NMOS_PORT", 3272, &error);
    loaded.config.nmosSeed = take("NMOS_SEED", loaded.config.hostId + "-srtgw");
    loaded.config.nmosLabel = take("NMOS_LABEL", "");
    loaded.config.nmosTagsJson = take("NMOS_TAGS", "");
    if (!loaded.config.nmosTagsJson.empty())
    {
        std::string tagsError;
        auto const tags = json::parse(loaded.config.nmosTagsJson, &tagsError);
        if (!tagsError.empty() || !tags.is<picojson::object>())
        {
            error += "NMOS_TAGS must be a JSON object of string arrays; ";
        }
        else
        {
            for (auto const& item : tags.get<picojson::object>())
            {
                if (!item.second.is<picojson::array>())
                {
                    error += "NMOS_TAGS value for " + item.first + " must be an array; ";
                    continue;
                }
                for (auto const& entry : item.second.get<picojson::array>())
                {
                    if (!entry.is<std::string>())
                    {
                        error += "NMOS_TAGS value for " + item.first + " must be strings; ";
                    }
                }
            }
        }
    }
    loaded.config.webPort = envInt({{"WEB_PORT", take("WEB_PORT", "8120")}}, "WEB_PORT", 8120, &error);
    loaded.config.logLevel = lower(take("LOG_LEVEL", "info"));
    loaded.config.stateDir = take("STATE_DIR", "/config");
    loaded.config.shutdownTimeoutS = envInt({{"SHUTDOWN_TIMEOUT_S", take("SHUTDOWN_TIMEOUT_S", "10")}}, "SHUTDOWN_TIMEOUT_S", 10, &error);
    loaded.config.cleanupOnExit = truthy(take("MXL_CLEANUP_ON_EXIT", "false"));
    auto const hostSet = envValues.count("NMOS_HOST_ADDRESS") != 0 || fileValues.count("NMOS_HOST_ADDRESS") != 0;
    auto const aliasSet = envValues.count("SRTGW_PUBLIC_IP") != 0 || fileValues.count("SRTGW_PUBLIC_IP") != 0;
    if (hostSet)
    {
        loaded.config.hostAddress = take("NMOS_HOST_ADDRESS", "");
    }
    else if (aliasSet)
    {
        loaded.config.hostAddress = take("SRTGW_PUBLIC_IP", "");
        loaded.origin["NMOS_HOST_ADDRESS"] = loaded.origin["SRTGW_PUBLIC_IP"];
    }
    else
    {
        loaded.config.hostAddress = primaryIpv4();
        loaded.origin["NMOS_HOST_ADDRESS"] = ValueOrigin::Default;
        loaded.origin["SRTGW_PUBLIC_IP"] = ValueOrigin::Default;
    }
    if ((hostSet || aliasSet) && !isAnnounceIpv4(loaded.config.hostAddress))
    {
        error += "NMOS_HOST_ADDRESS must be a non-loopback IPv4 address (SRTGW_PUBLIC_IP is the same setting); ";
    }
    else if (!hostSet && !aliasSet && !isAnnounceIpv4(loaded.config.hostAddress))
    {
        log::warn("announce_address_loopback", {{"address", loaded.config.hostAddress}, {"detail", "set NMOS_HOST_ADDRESS to the pod or node IP"}});
    }
    auto const historyMsSet = envValues.count("MXL_HISTORY_DURATION_MS") != 0 || fileValues.count("MXL_HISTORY_DURATION_MS") != 0;
    auto const historyNsSet = envValues.count("SRTGW_HISTORY_DURATION_NS") != 0 || fileValues.count("SRTGW_HISTORY_DURATION_NS") != 0;
    try
    {
        if (historyMsSet)
        {
            loaded.config.historyDurationNs = std::stoll(take("MXL_HISTORY_DURATION_MS", "1000")) * 1000000LL;
        }
        else if (historyNsSet)
        {
            loaded.config.historyDurationNs = std::stoll(take("SRTGW_HISTORY_DURATION_NS", "1000000000"));
            loaded.origin["MXL_HISTORY_DURATION_MS"] = loaded.origin["SRTGW_HISTORY_DURATION_NS"];
        }
        else
        {
            loaded.config.historyDurationNs = 1000000000;
            loaded.origin["MXL_HISTORY_DURATION_MS"] = ValueOrigin::Default;
        }
    }
    catch (...)
    {
        error += "MXL_HISTORY_DURATION_MS / SRTGW_HISTORY_DURATION_NS is not an integer; ";
    }
    auto const range = take("SRT_PORT_RANGE", "9000-9099");
    parsePortRange(range, &loaded.config.srtPortMin, &loaded.config.srtPortMax, &error);
    if (loaded.config.mxlOutputDomainId.empty())
    {
        loaded.config.mxlOutputDomainId = take("MXL_OUTPUT_DOMAIN_ID", "");
    }
    if (loaded.config.mxlOutputDomainId.empty())
    {
        loaded.config.mxlOutputDomainId = uuidV5(kUuidNamespaceUrl, loaded.config.nmosSeed + "/domain");
        loaded.origin["MXL_OUTPUT_DOMAIN_ID"] = ValueOrigin::Default;
    }
    auto const dirDefault = loaded.config.mxlDomainScanPath + "/srtgw-" + shortId(loaded.config.mxlOutputDomainId);
    loaded.config.mxlOutputDomainDir = take("MXL_OUTPUT_DOMAIN_DIR", dirDefault);

    std::string channelsJson = channelsJsonFromFile;
    if (envValues.count("SRTGW_CHANNELS_JSON") != 0)
    {
        channelsJson = envValues.at("SRTGW_CHANNELS_JSON");
        loaded.origin["channels"] = ValueOrigin::Env;
    }
    else if (!channelsJsonFromFile.empty())
    {
        loaded.origin["channels"] = ValueOrigin::File;
    }
    else
    {
        loaded.origin["channels"] = ValueOrigin::Default;
    }
    if (!channelsJson.empty())
    {
        std::string parseError;
        auto const root = json::parse(channelsJson, &parseError);
        if (!parseError.empty() || !root.is<picojson::array>())
        {
            error += "channels JSON is invalid; ";
        }
        else
        {
            for (auto const& item : root.get<picojson::array>())
            {
                loaded.config.channels.push_back(channelFromJson(item.serialize(), nullptr));
            }
        }
    }
    if (!error.empty())
    {
        throw ConfigError(error);
    }
    validateConfig(loaded.config);
    return loaded;
}

std::map<std::string, std::string> environmentValues()
{
    std::map<std::string, std::string> values;
    for (auto const& key : globalKeys())
    {
        if (char const* value = std::getenv(key.c_str()))
        {
            if (value[0] != '\0')
            {
                values[key] = value;
            }
        }
    }
    if (char const* file = std::getenv("SRTGW_CONFIG_FILE"))
    {
        values["SRTGW_CONFIG_FILE"] = file;
    }
    if (char const* channels = std::getenv("SRTGW_CHANNELS_JSON"))
    {
        values["SRTGW_CHANNELS_JSON"] = channels;
    }
    return values;
}

void readConfigFile(std::string const& path, std::map<std::string, std::string>* globals, std::string* channelsJson)
{
    std::ifstream in(path);
    if (!in)
    {
        throw ConfigError("cannot read config file " + path);
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string error;
    auto const root = json::parse(buffer.str(), &error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        throw ConfigError("config file is not a JSON object: " + error);
    }
    for (auto const& key : globalKeys())
    {
        if (!json::has(root, key))
        {
            continue;
        }
        auto const value = json::field(root, key);
        if (value && (value->is<picojson::object>() || value->is<picojson::array>()))
        {
            (*globals)[key] = value->serialize();
        }
        else
        {
            (*globals)[key] = json::fieldString(root, key, "");
        }
    }
    if (json::has(root, "channels"))
    {
        auto const channels = json::field(root, "channels");
        if (!channels || !channels->is<picojson::array>())
        {
            throw ConfigError("channels must be an array");
        }
        *channelsJson = channels->serialize();
    }
}

std::string channelToJson(ChannelConfig const& channel, bool includeSecrets)
{
    picojson::object obj;
    obj["id"] = picojson::value(channel.id);
    obj["label"] = picojson::value(channel.label);
    obj["direction"] = picojson::value(channel.direction);
    obj["enabled"] = picojson::value(channel.enabled);
    obj["srt"] = srtToJson(channel.srt, includeSecrets);
    picojson::object backup;
    backup["enabled"] = picojson::value(channel.backup.enabled);
    backup["endpoint"] = srtToJson(channel.backup.endpoint, includeSecrets);
    backup["failover_ms"] = picojson::value(static_cast<double>(channel.backup.failoverMs));
    backup["failback_ms"] = picojson::value(static_cast<double>(channel.backup.failbackMs));
    backup["force"] = picojson::value(channel.backup.force);
    obj["backup"] = picojson::value(backup);
    picojson::object program;
    program["select"] = picojson::value(channel.program.select);
    program["number"] = picojson::value(static_cast<double>(channel.program.number));
    program["service_name"] = picojson::value(channel.program.serviceName);
    program["video_pid"] = picojson::value(static_cast<double>(channel.program.videoPid));
    program["audio_pids"] = json::intList(channel.program.audioPids);
    obj["program"] = picojson::value(program);
    obj["target"] = targetToJson(channel.target);
    obj["source_scan"] = picojson::value(channel.sourceScan);
    obj["deinterlacer"] = picojson::value(channel.deinterlacer);
    obj["scale"] = picojson::value(channel.scale);
    obj["aspect"] = picojson::value(channel.aspect);
    obj["sync_latency_ms"] = picojson::value(static_cast<double>(channel.syncLatencyMs));
    obj["hold_ms"] = picojson::value(static_cast<double>(channel.holdMs));
    obj["loss_mode"] = picojson::value(channel.lossMode);
    obj["audio_offset_ms"] = picojson::value(channel.audioOffsetMs);
    picojson::array outputs;
    for (auto const& output : channel.audioOutputs)
    {
        picojson::object item;
        item["channels"] = picojson::value(static_cast<double>(output.channels));
        item["preset"] = picojson::value(output.preset);
        item["routes"] = routesToJson(output);
        outputs.emplace_back(item);
    }
    obj["audio_outputs"] = picojson::value(outputs);
    obj["decoder"] = picojson::value(channel.decoder);
    obj["encoder"] = picojson::value(channel.encoder);
    picojson::object egress;
    egress["codec"] = picojson::value(channel.egress.codec);
    egress["bitrate"] = picojson::value(static_cast<double>(channel.egress.bitrate));
    egress["gop_seconds"] = picojson::value(channel.egress.gopSeconds);
    egress["bframes"] = picojson::value(static_cast<double>(channel.egress.bframes));
    egress["profile"] = picojson::value(channel.egress.profile);
    egress["level"] = picojson::value(channel.egress.level);
    egress["preset"] = picojson::value(channel.egress.preset);
    egress["tune"] = picojson::value(channel.egress.tune);
    egress["nvenc_preset"] = picojson::value(channel.egress.nvencPreset);
    egress["nvenc_tune"] = picojson::value(channel.egress.nvencTune);
    picojson::array tracks;
    for (auto const& track : channel.egress.audioTracks)
    {
        tracks.push_back(trackToJson(track));
    }
    egress["audio_tracks"] = picojson::value(tracks);
    egress["service_name"] = picojson::value(channel.egress.serviceName);
    egress["provider"] = picojson::value(channel.egress.provider);
    egress["program_number"] = picojson::value(static_cast<double>(channel.egress.programNumber));
    egress["pcr_ms"] = picojson::value(static_cast<double>(channel.egress.pcrMs));
    egress["mux"] = picojson::value(channel.egress.mux);
    egress["pmt_pid"] = picojson::value(static_cast<double>(channel.egress.pmtPid));
    egress["video_pid"] = picojson::value(static_cast<double>(channel.egress.videoPid));
    egress["read_offset_grains"] = picojson::value(static_cast<double>(channel.egress.readOffsetGrains));
    obj["egress"] = picojson::value(egress);
    return picojson::value(obj).serialize();
}

ChannelConfig channelFromJson(std::string const& text, ChannelConfig const* previous)
{
    std::string error;
    auto const root = json::parse(text, &error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        throw ConfigError("channel JSON is invalid: " + error);
    }
    ChannelConfig channel = previous != nullptr ? *previous : defaultIngest("ch", 0);
    channel.id = json::fieldString(root, "id", channel.id);
    channel.label = json::fieldString(root, "label", channel.label);
    channel.direction = json::fieldString(root, "direction", channel.direction);
    channel.enabled = json::fieldBool(root, "enabled", channel.enabled);
    if (channel.direction == "egress" && previous == nullptr && !json::has(root, "srt"))
    {
        channel.srt.mode = "caller";
        channel.srt.localPort = 0;
    }
    if (json::has(root, "srt"))
    {
        channel.srt = srtFromJson(*json::field(root, "srt"), channel.srt);
    }
    if (json::has(root, "backup"))
    {
        auto const backup = *json::field(root, "backup");
        channel.backup.enabled = json::fieldBool(backup, "enabled", channel.backup.enabled);
        channel.backup.failoverMs = json::fieldInt(backup, "failover_ms", channel.backup.failoverMs);
        channel.backup.failbackMs = json::fieldInt(backup, "failback_ms", channel.backup.failbackMs);
        channel.backup.force = json::fieldString(backup, "force", channel.backup.force);
        if (json::has(backup, "endpoint"))
        {
            channel.backup.endpoint = srtFromJson(*json::field(backup, "endpoint"), channel.backup.endpoint);
        }
    }
    if (json::has(root, "program"))
    {
        auto const program = *json::field(root, "program");
        channel.program.select = json::fieldString(program, "select", channel.program.select);
        channel.program.number = json::fieldInt(program, "number", channel.program.number);
        channel.program.serviceName = json::fieldString(program, "service_name", channel.program.serviceName);
        channel.program.videoPid = json::fieldInt(program, "video_pid", channel.program.videoPid);
        if (json::has(program, "audio_pids"))
        {
            channel.program.audioPids = json::fieldIntList(program, "audio_pids");
        }
    }
    if (json::has(root, "target"))
    {
        channel.target = targetFromJson(*json::field(root, "target"), channel.target);
    }
    channel.sourceScan = json::fieldString(root, "source_scan", channel.sourceScan);
    channel.deinterlacer = json::fieldString(root, "deinterlacer", channel.deinterlacer);
    channel.scale = json::fieldString(root, "scale", channel.scale);
    channel.aspect = json::fieldString(root, "aspect", channel.aspect);
    channel.syncLatencyMs = json::fieldInt(root, "sync_latency_ms", channel.syncLatencyMs);
    channel.holdMs = json::fieldInt(root, "hold_ms", channel.holdMs);
    channel.lossMode = json::fieldString(root, "loss_mode", channel.lossMode);
    channel.audioOffsetMs = json::fieldDouble(root, "audio_offset_ms", channel.audioOffsetMs);
    if (json::has(root, "audio_outputs"))
    {
        channel.audioOutputs.clear();
        auto const outputs = json::field(root, "audio_outputs");
        if (outputs && outputs->is<picojson::array>())
        {
            for (auto const& item : outputs->get<picojson::array>())
            {
                channel.audioOutputs.push_back(audioFromJson(item));
            }
        }
    }
    else if (previous == nullptr)
    {
        channel.audioOutputs.clear();
        channel.audioOutputs.push_back(presetIngest("sequential", 16));
    }
    if (json::has(root, "audio_preset"))
    {
        int const channels = channel.audioOutputs.empty() ? 16 : channel.audioOutputs.front().channels;
        channel.audioOutputs.clear();
        channel.audioOutputs.push_back(presetIngest(json::fieldString(root, "audio_preset", "sequential"), channels));
    }
    channel.decoder = json::fieldString(root, "decoder", channel.decoder);
    channel.encoder = json::fieldString(root, "encoder", channel.encoder);
    if (json::has(root, "egress"))
    {
        auto const egress = *json::field(root, "egress");
        channel.egress.codec = json::fieldString(egress, "codec", channel.egress.codec);
        channel.egress.bitrate = json::fieldInt(egress, "bitrate", channel.egress.bitrate);
        channel.egress.gopSeconds = json::fieldDouble(egress, "gop_seconds", channel.egress.gopSeconds);
        channel.egress.bframes = json::fieldInt(egress, "bframes", channel.egress.bframes);
        channel.egress.profile = json::fieldString(egress, "profile", channel.egress.profile);
        channel.egress.level = json::fieldString(egress, "level", channel.egress.level);
        channel.egress.preset = json::fieldString(egress, "preset", channel.egress.preset);
        channel.egress.tune = json::fieldString(egress, "tune", channel.egress.tune);
        channel.egress.nvencPreset = json::fieldString(egress, "nvenc_preset", channel.egress.nvencPreset);
        channel.egress.nvencTune = json::fieldString(egress, "nvenc_tune", channel.egress.nvencTune);
        channel.egress.serviceName = json::fieldString(egress, "service_name", channel.egress.serviceName);
        channel.egress.provider = json::fieldString(egress, "provider", channel.egress.provider);
        channel.egress.programNumber = json::fieldInt(egress, "program_number", channel.egress.programNumber);
        channel.egress.pcrMs = json::fieldInt(egress, "pcr_ms", channel.egress.pcrMs);
        channel.egress.mux = json::fieldString(egress, "mux", channel.egress.mux);
        channel.egress.pmtPid = json::fieldInt(egress, "pmt_pid", channel.egress.pmtPid);
        channel.egress.videoPid = json::fieldInt(egress, "video_pid", channel.egress.videoPid);
        channel.egress.readOffsetGrains = json::fieldInt(egress, "read_offset_grains", channel.egress.readOffsetGrains);
        if (json::has(egress, "audio_tracks"))
        {
            channel.egress.audioTracks.clear();
            auto const tracks = json::field(egress, "audio_tracks");
            if (tracks && tracks->is<picojson::array>())
            {
                for (auto const& item : tracks->get<picojson::array>())
                {
                    channel.egress.audioTracks.push_back(trackFromJson(item));
                }
            }
        }
        if (json::has(egress, "preset"))
        {
            channel.egress.audioTracks = presetEgress(json::fieldString(egress, "preset", "8x-stereo-aac"));
        }
    }
    else if (previous == nullptr && channel.egress.audioTracks.empty())
    {
        channel.egress.audioTracks = presetEgress(channel.direction == "egress" ? "stereo" : "8x-stereo-aac");
    }
    return channel;
}

std::string configToJson(LoadedConfig const& loaded, bool includeSecrets)
{
    picojson::object obj;
    auto add = [&](std::string const& key, std::string const& value) { obj[key] = picojson::value(value); };
    auto const& cfg = loaded.config;
    add("HOST_ID", cfg.hostId);
    add("MXL_DOMAIN_SCAN_PATH", cfg.mxlDomainScanPath);
    add("MXL_OUTPUT_DOMAIN_DIR", cfg.mxlOutputDomainDir);
    add("MXL_OUTPUT_DOMAIN_ID", cfg.mxlOutputDomainId);
    add("DECODER", cfg.decoder);
    add("ENCODER", cfg.encoder);
    add("SRT_PORT_RANGE", std::to_string(cfg.srtPortMin) + "-" + std::to_string(cfg.srtPortMax));
    add("NMOS_REGISTRY_ADDRESS", cfg.nmosRegistryAddress);
    obj["NMOS_REGISTRY_PORT"] = picojson::value(static_cast<double>(cfg.nmosRegistryPort));
    add("NMOS_QUERY_ADDRESS", cfg.nmosQueryAddress);
    obj["NMOS_QUERY_PORT"] = picojson::value(static_cast<double>(cfg.nmosQueryPort));
    obj["NMOS_DNS_SD"] = picojson::value(cfg.nmosDnsSd);
    obj["NMOS_PORT"] = picojson::value(static_cast<double>(cfg.nmosPort));
    add("NMOS_SEED", cfg.nmosSeed);
    add("NMOS_LABEL", cfg.nmosLabel);
    add("NMOS_TAGS", cfg.nmosTagsJson);
    obj["WEB_PORT"] = picojson::value(static_cast<double>(cfg.webPort));
    add("LOG_LEVEL", cfg.logLevel);
    add("NMOS_HOST_ADDRESS", cfg.hostAddress);
    add("SRTGW_PUBLIC_IP", cfg.hostAddress);
    obj["MXL_HISTORY_DURATION_MS"] = picojson::value(static_cast<double>(cfg.historyDurationNs / 1000000));
    obj["SRTGW_HISTORY_DURATION_NS"] = picojson::value(static_cast<double>(cfg.historyDurationNs));
    add("STATE_DIR", cfg.stateDir);
    obj["SHUTDOWN_TIMEOUT_S"] = picojson::value(static_cast<double>(cfg.shutdownTimeoutS));
    obj["MXL_CLEANUP_ON_EXIT"] = picojson::value(cfg.cleanupOnExit);
    picojson::object origin;
    for (auto const& item : loaded.origin)
    {
        char const* name = "default";
        if (item.second == ValueOrigin::File)
        {
            name = "file";
        }
        else if (item.second == ValueOrigin::Env)
        {
            name = "env";
        }
        origin[item.first] = picojson::value(std::string(name));
    }
    obj["origin"] = picojson::value(origin);
    obj["restart_required"] = picojson::value(loaded.restartRequired);
    picojson::array channels;
    for (auto const& channel : cfg.channels)
    {
        std::string parseError;
        channels.push_back(json::parse(channelToJson(channel, includeSecrets), &parseError));
    }
    obj["channels"] = picojson::value(channels);
    return picojson::value(obj).serialize();
}

std::string configToEnv(Config const& config)
{
    std::ostringstream out;
    out << "HOST_ID=" << config.hostId << "\n";
    out << "MXL_DOMAIN_SCAN_PATH=" << config.mxlDomainScanPath << "\n";
    out << "MXL_OUTPUT_DOMAIN_DIR=" << config.mxlOutputDomainDir << "\n";
    out << "MXL_OUTPUT_DOMAIN_ID=" << config.mxlOutputDomainId << "\n";
    out << "DECODER=" << config.decoder << "\n";
    out << "ENCODER=" << config.encoder << "\n";
    out << "SRT_PORT_RANGE=" << config.srtPortMin << "-" << config.srtPortMax << "\n";
    out << "NMOS_REGISTRY_ADDRESS=" << config.nmosRegistryAddress << "\n";
    out << "NMOS_REGISTRY_PORT=" << config.nmosRegistryPort << "\n";
    out << "NMOS_QUERY_ADDRESS=" << config.nmosQueryAddress << "\n";
    out << "NMOS_QUERY_PORT=" << config.nmosQueryPort << "\n";
    out << "NMOS_DNS_SD=" << (config.nmosDnsSd ? "true" : "false") << "\n";
    out << "NMOS_PORT=" << config.nmosPort << "\n";
    out << "NMOS_SEED=" << config.nmosSeed << "\n";
    out << "NMOS_LABEL=" << config.nmosLabel << "\n";
    out << "WEB_PORT=" << config.webPort << "\n";
    out << "LOG_LEVEL=" << config.logLevel << "\n";
    out << "NMOS_HOST_ADDRESS=" << config.hostAddress << "\n";
    out << "SRTGW_PUBLIC_IP=" << config.hostAddress << "\n";
    out << "MXL_HISTORY_DURATION_MS=" << (config.historyDurationNs / 1000000) << "\n";
    out << "STATE_DIR=" << config.stateDir << "\n";
    out << "SHUTDOWN_TIMEOUT_S=" << config.shutdownTimeoutS << "\n";
    out << "MXL_CLEANUP_ON_EXIT=" << (config.cleanupOnExit ? "true" : "false") << "\n";
    return out.str();
}
} // namespace srtgw
