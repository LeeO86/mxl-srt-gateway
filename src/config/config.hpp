#pragma once

#include "media/format.hpp"
#include "media/matrix.hpp"
#include "srt/access.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace srtgw
{
struct ConfigError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct SrtEndpointConfig
{
    std::string mode = "listener";
    std::string localAddress;
    int localPort = 0;
    std::string remoteHost;
    int remotePort = 0;
    int latencyMs = 200;
    std::string passphrase;
    int pbkeylen = 0;
    std::string streamId;
    std::vector<std::string> acceptedStreamIds;
    std::int64_t maxBandwidth = 0;
    int overhead = 25;
    int payloadSize = 1316;
    int connectTimeoutMs = 3000;
    std::vector<std::string> peerAllow;
    std::string exposure = "internal";
};

struct BackupConfig
{
    bool enabled = false;
    SrtEndpointConfig endpoint;
    int failoverMs = 1000;
    int failbackMs = 5000;
    std::string force = "auto";
};

struct ProgramSelect
{
    std::string select = "first";
    int number = 0;
    std::string serviceName;
    int videoPid = 0;
    std::vector<int> audioPids;
};

struct EgressSettings
{
    std::string codec = "h264";
    int bitrate = 15000000;
    double gopSeconds = 1.0;
    int bframes = 0;
    std::string profile = "high";
    std::string level = "4.2";
    std::string preset = "veryfast";
    std::string tune = "zerolatency";
    int threads = 0; // libx264 threads; 0 = x264's own choice
    std::string nvencPreset = "p4";
    std::string nvencTune = "ull";
    std::vector<EgressAudioTrack> audioTracks;
    std::string serviceName = "SRTGW";
    std::string provider = "mxl-srt-gateway";
    int programNumber = 1;
    int pcrMs = 40;
    std::string mux = "cbr";
    int pmtPid = 4096;
    int videoPid = 256;
    int readOffsetGrains = 2;
};

struct ChannelConfig
{
    std::string id;
    std::string label;
    std::string direction = "ingest";
    bool enabled = true;
    SrtEndpointConfig srt;
    BackupConfig backup;
    ProgramSelect program;
    VideoFormat target;
    std::string sourceScan = "auto";
    std::string deinterlacer = "bwdif";
    std::string scale = "bicubic";
    std::string aspect = "letterbox";
    int syncLatencyMs = 120;
    int holdMs = 500;
    std::string lossMode = "slate";
    double audioOffsetMs = 0.0;
    std::vector<AudioOutput> audioOutputs;
    std::string decoder;
    std::string encoder;
    EgressSettings egress;
    // Filled from the process host address when a pipeline starts. Not part of the saved channel document.
    std::string announceAddress;
};

struct Config
{
    std::string hostId;
    std::string mxlDomainScanPath = "/Volumes/mxl";
    std::string mxlOutputDomainDir;
    std::string mxlOutputDomainId;
    std::string decoder = "auto";
    std::string encoder = "auto";
    int srtPortMin = 9000;
    int srtPortMax = 9099;
    std::string nmosRegistryAddress;
    int nmosRegistryPort = 3210;
    std::string nmosQueryAddress;
    int nmosQueryPort = 3211;
    bool nmosDnsSd = false;
    int nmosPort = 3272;
    std::string nmosSeed;
    std::string nmosLabel;
    std::string nmosTagsJson;
    int webPort = 8120;
    std::string logLevel = "info";
    // Address announced to NMOS, the UI and remote SRT callers.
    // NMOS_HOST_ADDRESS, or SRTGW_PUBLIC_IP when that is unset.
    std::string hostAddress;
    std::int64_t historyDurationNs = 1000000000;
    std::string stateDir = "/config";
    int shutdownTimeoutS = 10;
    bool cleanupOnExit = false;
    std::string configFile;
    std::vector<ChannelConfig> channels;
};

enum class ValueOrigin
{
    Default,
    File,
    Env,
};

struct LoadedConfig
{
    Config config;
    std::map<std::string, ValueOrigin> origin;
    bool restartRequired = false;
    // The environment loadFromSources was given (without SRTGW_CHANNELS_JSON), so a reload keeps its precedence.
    std::map<std::string, std::string> env;
};

ChannelConfig defaultIngest(std::string const& id, int port);
ChannelConfig defaultEgress(std::string const& id);

LoadedConfig loadFromSources(std::map<std::string, std::string> const& fileValues, std::string const& channelsJsonFromFile,
    std::map<std::string, std::string> const& envValues);
std::map<std::string, std::string> environmentValues();
void readConfigFile(std::string const& path, std::map<std::string, std::string>* globals, std::string* channelsJson);

std::string channelToJson(ChannelConfig const& channel, bool includeSecrets);
ChannelConfig channelFromJson(std::string const& json, ChannelConfig const* previous);
std::string configToJson(LoadedConfig const& loaded, bool includeSecrets);
std::string configToEnv(Config const& config);
// GET /api/v1/info: versions, the node label, the announced address and the config file.
std::string infoJson(Config const& config);

bool isRestartKey(std::string const& key);
std::vector<std::string> globalKeys();
void validateConfig(Config const& config);
std::string assignListenerPort(Config const& config, ChannelConfig const& channel);

AccessPolicy policyFor(SrtEndpointConfig const& endpoint);
} // namespace srtgw
