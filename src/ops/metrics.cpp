#include "ops/metrics.hpp"

#include <fstream>
#include <sstream>

namespace srtgw
{
namespace
{
double const kLatencyBounds[] = {0.005, 0.01, 0.02, 0.04, 0.08, 0.16, 0.32, 0.64};

std::string labels(std::string const& id)
{
    return std::string("{channel=\"") + id + "\"}";
}

void gauge(std::ostringstream& out, char const* name, std::string const& id, double value)
{
    out << "mxl_srt_gateway_" << name << labels(id) << " " << value << "\n";
}

void counter(std::ostringstream& out, char const* name, std::string const& id, std::uint64_t value)
{
    out << "mxl_srt_gateway_" << name << labels(id) << " " << value << "\n";
}
} // namespace

int stateCode(std::string const& name)
{
    if (name == "connecting")
    {
        return 1;
    }
    if (name == "waiting")
    {
        return 2;
    }
    if (name == "no_signal")
    {
        return 3;
    }
    if (name == "running")
    {
        return 4;
    }
    if (name == "error")
    {
        return 5;
    }
    return 0;
}

std::string renderMetrics(ProcessMetrics const& metrics)
{
    std::ostringstream out;
    out << "# HELP mxl_srt_gateway_channel_state Channel state 0 idle 1 connecting 2 waiting 3 no_signal 4 running 5 error\n";
    out << "# TYPE mxl_srt_gateway_channel_state gauge\n";
    for (auto const& channel : metrics.channels)
    {
        auto const id = channel.id;
        gauge(out, "channel_state", id, channel.state);
        gauge(out, "srt_rtt_ms", id, channel.srtRttMs);
        counter(out, "srt_pkt_loss_total", id, channel.pktLoss);
        counter(out, "srt_pkt_retrans_total", id, channel.pktRetrans);
        counter(out, "srt_pkt_drop_total", id, channel.pktDrop);
        gauge(out, "srt_bitrate_bps", id, channel.bitrateBps);
        gauge(out, "srt_buffer_ms", id, channel.bufferMs);
        gauge(out, "srt_connected", id, channel.connected);
        counter(out, "video_frames_out_total", id, channel.videoFrames);
        counter(out, "framesync_repeats_total", id, channel.repeats);
        counter(out, "framesync_drops_total", id, channel.drops);
        gauge(out, "audio_drift_ppm", id, channel.audioDriftPpm);
        gauge(out, "audio_fifo_ms", id, channel.audioFifoMs);
        counter(out, "decode_errors_total", id, channel.decodeErrors);
        gauge(out, "decode_fps", id, channel.decodeFps);
        gauge(out, "encode_fps", id, channel.encodeFps);
        gauge(out, "failover_active", id, channel.failoverActive);
        out << "mxl_srt_gateway_codec_info{channel=\"" << id << "\",decoder=\"" << channel.decoder << "\",encoder=\"" << channel.encoder
            << "\",source=\"" << channel.sourceFormat << "\",target=\"" << channel.targetFormat << "\"} 1\n";
        auto const buckets = channel.encodeLatencyBuckets.size() == 8 ? channel.encodeLatencyBuckets : std::vector<std::uint64_t>(8, 0);
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < 8; ++i)
        {
            cumulative += buckets[i];
            out << "mxl_srt_gateway_encode_latency_seconds_bucket{channel=\"" << id << "\",le=\"" << kLatencyBounds[i] << "\"} " << cumulative << "\n";
        }
        out << "mxl_srt_gateway_encode_latency_seconds_bucket{channel=\"" << id << "\",le=\"+Inf\"} " << channel.encodeLatencyCount << "\n";
        out << "mxl_srt_gateway_encode_latency_seconds_sum{channel=\"" << id << "\"} " << channel.encodeLatencySum << "\n";
        out << "mxl_srt_gateway_encode_latency_seconds_count{channel=\"" << id << "\"} " << channel.encodeLatencyCount << "\n";
    }
    out << "mxl_srt_gateway_process_resident_bytes " << metrics.residentBytes << "\n";
    out << "mxl_srt_gateway_process_cpu_seconds " << metrics.cpuSeconds << "\n";
    out << "mxl_srt_gateway_nvenc_sessions " << metrics.nvencSessions << "\n";
    out << "mxl_srt_gateway_nvdec_sessions " << metrics.nvdecSessions << "\n";
    out << "mxl_srt_gateway_gpu_memory_bytes " << metrics.gpuMemoryBytes << "\n";
    return out.str();
}
} // namespace srtgw
