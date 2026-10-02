#include "channel/runtime.hpp"

#include "nmos/ids.hpp"
#include "util/logging.hpp"
#include "util/net.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace srtgw
{
namespace
{
std::string jsonEscape(std::string const& text)
{
    return log::jsonEscape(text);
}

double readRss()
{
    std::ifstream in("/proc/self/statm");
    double pages = 0;
    in >> pages >> pages;
    return pages * 4096.0;
}

void readGpu(int* nvenc, int* nvdec, double* memory)
{
    *nvenc = 0;
    *nvdec = 0;
    *memory = 0;
    FILE* pipe = popen("nvidia-smi --query-gpu=memory.used,encoder.stats.sessionCount,decoder.stats.sessionCount --format=csv,noheader,nounits 2>/dev/null", "r");
    if (pipe == nullptr)
    {
        return;
    }
    char line[256] = {};
    if (std::fgets(line, sizeof(line), pipe) != nullptr)
    {
        double mem = 0;
        int enc = 0;
        int dec = 0;
        if (std::sscanf(line, "%lf, %d, %d", &mem, &enc, &dec) >= 1)
        {
            *memory = mem * 1024.0 * 1024.0;
            *nvenc = enc;
            *nvdec = dec;
        }
    }
    pclose(pipe);
}
} // namespace

ChannelManager::ChannelManager(ConfigStore& store)
    : store_(store)
    , ids_(makeNmosIds(store.config().nmosSeed))
{
}

ChannelManager::~ChannelManager()
{
    stop();
}

NmosIds ChannelManager::ids() const
{
    return ids_;
}

std::shared_ptr<MxlDomain> ChannelManager::outputDomain() const
{
    return output_;
}

void ChannelManager::start()
{
    auto const config = store_.config();
    ids_ = makeNmosIds(config.nmosSeed);
    output_ = std::make_shared<MxlDomain>(config.mxlOutputDomainDir, config.mxlOutputDomainId, config.historyDurationNs, true);
    stop_.store(false);
    ready_.store(true);
    for (auto const& channel : config.channels)
    {
        if (channel.enabled)
        {
            launch(channel);
        }
    }
    supervisor_ = std::thread([this] { supervise(); });
    log::info("channels_started", {{"count", std::to_string(config.channels.size())}});
}

void ChannelManager::stop()
{
    stop_.store(true);
    if (supervisor_.joinable())
    {
        supervisor_.join();
    }
    std::lock_guard const lock{mu_};
    slots_.clear();
    ready_.store(false);
}

bool ChannelManager::ready() const
{
    return ready_.load();
}

void ChannelManager::launch(ChannelConfig const& channel)
{
    Slot slot;
    slot.signature = channelToJson(channel, true);
    auto const config = store_.config();
    if (channel.direction == "ingest")
    {
        std::vector<std::string> audioIds;
        for (std::size_t i = 0; i < channel.audioOutputs.size(); ++i)
        {
            audioIds.push_back(ids_.audioFlow(channel, static_cast<int>(i)));
        }
        auto decoder = channel.decoder.empty() ? config.decoder : channel.decoder;
        slot.ingest = std::make_unique<IngestPipeline>(channel, output_, ids_.videoFlow(channel), audioIds, decoder);
        }
    else
    {
        auto encoder = channel.encoder.empty() ? config.encoder : channel.encoder;
        slot.egress = std::make_unique<EgressPipeline>(channel, encoder, [this](std::string const& id, bool* mirror) { return openInput(id, mirror); },
            [this, id = channel.id](bool video) { return route(id, video); });
    }
    IngestPipeline* ingest = nullptr;
    EgressPipeline* egress = nullptr;
    {
        std::lock_guard const lock{mu_};
        auto const id = channel.id;
        slots_[id] = std::move(slot);
        ingest = slots_[id].ingest.get();
        egress = slots_[id].egress.get();
    }
    if (ingest != nullptr)
    {
        ingest->start();
    }
    if (egress != nullptr)
    {
        egress->start();
    }
}

std::shared_ptr<MxlDomain> ChannelManager::openInput(std::string const& domainId, bool* mirror)
{
    auto const config = store_.config();
    if (mirror != nullptr)
    {
        *mirror = false;
    }
    if (domainId.empty())
    {
        return nullptr;
    }
    if (domainId == config.mxlOutputDomainId)
    {
        return output_;
    }
    std::string path = resolveDomainPath(config.mxlDomainScanPath, domainId);
    bool isMirror = false;
    for (auto const& domain : scanDomains(config.mxlDomainScanPath))
    {
        if (domain.id == domainId)
        {
            isMirror = domain.mirror;
            path = domain.path;
        }
    }
    if (mirror != nullptr)
    {
        *mirror = isMirror;
    }
    if (path.empty())
    {
        return nullptr;
    }
    std::lock_guard const lock{mu_};
    auto const it = inputs_.find(path);
    if (it != inputs_.end())
    {
        return it->second;
    }
    try
    {
        auto domain = std::make_shared<MxlDomain>(path, domainId, config.historyDurationNs, false);
        inputs_[path] = domain;
        return domain;
    }
    catch (std::exception const& ex)
    {
        log::warn("mxl_input_domain_failed", {{"id", domainId}, {"error", ex.what()}});
        return nullptr;
    }
}

void ChannelManager::supervise()
{
    while (!stop_.load())
    {
        auto const config = store_.config();
        std::map<std::string, ChannelConfig> wanted;
        for (auto const& channel : config.channels)
        {
            if (channel.enabled)
            {
                wanted[channel.id] = channel;
            }
        }
        std::vector<ChannelConfig> starting;
        {
            std::lock_guard const lock{mu_};
            for (auto it = slots_.begin(); it != slots_.end();)
            {
                auto const found = wanted.find(it->first);
                if (found == wanted.end() || channelToJson(found->second, true) != it->second.signature)
                {
                    it = slots_.erase(it);
                }
                else
                {
                    wanted.erase(found);
                    ++it;
                }
            }
            for (auto const& item : wanted)
            {
                if (slots_.count(item.first) == 0)
                {
                    starting.push_back(item.second);
                }
            }
        }
        for (auto const& channel : starting)
        {
            try
            {
                launch(channel);
            }
            catch (std::exception const& ex)
            {
                log::error("channel_start_failed", {{"channel", channel.id}, {"error", ex.what()}});
            }
        }
        if (output_)
        {
            output_->garbageCollect();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
}

void ChannelManager::setRoute(std::string const& id, bool video, FlowRoute route)
{
    auto const config = store_.config();
    for (auto const& domain : scanDomains(config.mxlDomainScanPath))
    {
        if (domain.id == route.domainId)
        {
            route.mirror = domain.mirror;
        }
    }
    std::lock_guard const lock{mu_};
    if (video)
    {
        videoRoutes_[id] = std::move(route);
    }
    else
    {
        audioRoutes_[id] = std::move(route);
    }
    log::info("route_updated", {{"channel", id}, {"kind", video ? "video" : "audio"}, {"flow", video ? videoRoutes_[id].flowId : audioRoutes_[id].flowId}});
}

FlowRoute ChannelManager::route(std::string const& id, bool video) const
{
    std::lock_guard const lock{mu_};
    auto const& table = video ? videoRoutes_ : audioRoutes_;
    auto const it = table.find(id);
    if (it == table.end())
    {
        return {};
    }
    return it->second;
}

std::string ChannelManager::statusJson() const
{
    std::ostringstream out;
    out << "{\"ready\":" << (ready_.load() ? "true" : "false") << ",\"channels\":[";
    auto const config = store_.config();
    bool first = true;
    for (auto const& channel : config.channels)
    {
        if (!first)
        {
            out << ',';
        }
        first = false;
        out << channelJson(channel.id, true);
    }
    out << "]}";
    return out.str();
}

std::string ChannelManager::channelJson(std::string const& id, bool wrapped) const
{
    PipelineStatus status;
    bool found = false;
    {
        std::lock_guard const lock{mu_};
        auto const it = slots_.find(id);
        if (it != slots_.end())
        {
            found = true;
            status = it->second.ingest ? it->second.ingest->status() : it->second.egress->status();
        }
    }
    ChannelConfig channel;
    bool haveChannel = false;
    for (auto const& item : store_.config().channels)
    {
        if (item.id == id)
        {
            channel = item;
            haveChannel = true;
        }
    }
    if (!haveChannel)
    {
        return wrapped ? "{}" : "";
    }
    if (!found)
    {
        status.state = channel.enabled ? "connecting" : "idle";
        status.targetFormat = channel.target.label();
    }
    auto const videoRoute = route(id, true);
    auto const audioRoute = route(id, false);
    std::ostringstream out;
    out << "{\"id\":\"" << jsonEscape(id) << "\",\"label\":\"" << jsonEscape(channel.label) << "\",\"direction\":\"" << channel.direction
        << "\",\"enabled\":" << (channel.enabled ? "true" : "false") << ",\"state\":\"" << status.state << "\",\"error\":\"" << jsonEscape(status.error)
        << "\",\"peer\":\"" << jsonEscape(status.peer) << "\",\"source_format\":\"" << jsonEscape(status.sourceFormat) << "\",\"target_format\":\""
        << jsonEscape(status.targetFormat.empty() ? channel.target.label() : status.targetFormat) << "\",\"decoder\":\"" << jsonEscape(status.decoder)
        << "\",\"encoder\":\"" << jsonEscape(status.encoder) << "\",\"frames\":" << status.frames << ",\"repeats\":" << status.repeats << ",\"drops\":" << status.drops
        << ",\"audio_drift_ppm\":" << status.driftPpm << ",\"audio_fifo_ms\":" << status.fifoMs << ",\"decode_fps\":" << status.decodeFps
        << ",\"encode_fps\":" << status.encodeFps << ",\"failover_active\":" << (status.failover ? "true" : "false") << ",\"timecode\":\""
        << jsonEscape(status.timecode) << "\",\"request\":\"" << jsonEscape(status.request) << "\",\"srt\":{\"rtt_ms\":" << status.srt.rttMs
        << ",\"loss\":" << status.srt.pktLoss << ",\"retrans\":" << status.srt.pktRetrans << ",\"drop\":" << status.srt.pktDrop
        << ",\"bitrate_bps\":" << status.srt.bitrateBps << ",\"buffer_ms\":" << status.srt.bufferMs << ",\"connected\":"
        << (status.srt.connected ? "true" : "false") << ",\"streamid\":\"" << jsonEscape(status.srt.streamId) << "\"},\"meters\":[";
    for (std::size_t i = 0; i < status.meters.size(); ++i)
    {
        if (i != 0)
        {
            out << ',';
        }
        out << status.meters[i];
    }
    out << "],\"tracks\":[";
    for (std::size_t i = 0; i < status.tracks.size(); ++i)
    {
        if (i != 0)
        {
            out << ',';
        }
        auto const& track = status.tracks[i];
        out << "{\"pid\":" << track.pid << ",\"codec\":\"" << jsonEscape(track.codec) << "\",\"layout\":\"" << jsonEscape(track.layout)
            << "\",\"language\":\"" << jsonEscape(track.language) << "\",\"missing\":" << (track.missing ? "true" : "false") << "}";
    }
    out << "],\"alarms\":[";
    for (std::size_t i = 0; i < status.alarms.size(); ++i)
    {
        if (i != 0)
        {
            out << ',';
        }
        out << "\"" << jsonEscape(status.alarms[i]) << "\"";
    }
    out << "],\"flows\":{\"video\":\"" << (channel.direction == "ingest" ? ids_.videoFlow(channel) : videoRoute.flowId) << "\",\"audio\":[";
    if (channel.direction == "ingest")
    {
        for (std::size_t i = 0; i < channel.audioOutputs.size(); ++i)
        {
            if (i != 0)
            {
                out << ',';
            }
            out << "\"" << ids_.audioFlow(channel, static_cast<int>(i)) << "\"";
        }
    }
    else if (!audioRoute.flowId.empty())
    {
        out << "\"" << audioRoute.flowId << "\"";
    }
    out << "]},\"route\":{\"video_domain\":\"" << videoRoute.domainId << "\",\"video_flow\":\"" << videoRoute.flowId << "\",\"video_active\":"
        << (videoRoute.active ? "true" : "false") << ",\"audio_domain\":\"" << audioRoute.domainId << "\",\"audio_flow\":\"" << audioRoute.flowId
        << "\",\"audio_active\":" << (audioRoute.active ? "true" : "false") << "},\"thumbnail\":\"/api/v1/channels/" << jsonEscape(id) << "/thumbnail\"}";
    return out.str();
}

std::vector<std::uint8_t> ChannelManager::thumbnail(std::string const& id) const
{
    std::lock_guard const lock{mu_};
    auto const it = slots_.find(id);
    if (it == slots_.end())
    {
        return {};
    }
    auto const status = it->second.ingest ? it->second.ingest->status() : it->second.egress->status();
    return status.jpeg;
}

ProcessMetrics ChannelManager::metrics() const
{
    ProcessMetrics metrics;
    metrics.residentBytes = readRss();
    static int nvenc = 0;
    static int nvdec = 0;
    static double memory = 0;
    static std::int64_t last = 0;
    auto const now = monoNowMs();
    if (now - last > 2000)
    {
        readGpu(&nvenc, &nvdec, &memory);
        last = now;
    }
    metrics.nvencSessions = nvenc;
    metrics.nvdecSessions = nvdec;
    metrics.gpuMemoryBytes = memory;
    std::lock_guard const lock{mu_};
    for (auto const& slot : slots_)
    {
        auto const status = slot.second.ingest ? slot.second.ingest->status() : slot.second.egress->status();
        ChannelMetrics item;
        item.id = slot.first;
        item.state = stateCode(status.state);
        item.srtRttMs = status.srt.rttMs;
        item.pktLoss = status.srt.pktLoss;
        item.pktRetrans = status.srt.pktRetrans;
        item.pktDrop = status.srt.pktDrop;
        item.bitrateBps = status.srt.bitrateBps;
        item.bufferMs = status.srt.bufferMs;
        item.connected = status.srt.connected ? 1 : 0;
        item.videoFrames = status.frames;
        item.repeats = status.repeats;
        item.drops = status.drops;
        item.audioDriftPpm = status.driftPpm;
        item.audioFifoMs = status.fifoMs;
        item.decodeErrors = status.decodeErrors;
        item.decodeFps = status.decodeFps;
        item.encodeFps = status.encodeFps;
        item.encodeLatencyCount = status.encodeLatencyCount;
        item.encodeLatencySum = status.encodeLatencySum;
        item.encodeLatencyBuckets = status.latencyBuckets;
        item.failoverActive = status.failover ? 1 : 0;
        item.decoder = status.decoder;
        item.encoder = status.encoder;
        item.sourceFormat = status.sourceFormat;
        item.targetFormat = status.targetFormat;
        metrics.channels.push_back(std::move(item));
    }
    return metrics;
}
} // namespace srtgw
