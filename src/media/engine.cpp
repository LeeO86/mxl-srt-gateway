#include "media/engine.hpp"

#include "media/adapt.hpp"
#include "media/cuda_convert.hpp"
#include "media/framesync.hpp"
#include "media/matrix.hpp"
#include "media/slate.hpp"
#include "media/v210.hpp"
#include "util/logging.hpp"
#include "util/net.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <sstream>

namespace srtgw
{
namespace
{
struct Once
{
    Once()
    {
        av_log_set_level(AV_LOG_ERROR);
    }
};

void ffmpegOnce()
{
    static Once once;
    (void)once;
}

std::string ffError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

bool cudaFilterChainAvailable(AdaptationPlan const& plan)
{
    bool const bwdif = plan.deint == Deint::BwdifField || plan.deint == Deint::BwdifFrame;
    bool const yadif = plan.deint == Deint::YadifField || plan.deint == Deint::YadifFrame;
    bool const otherDeint = plan.deint != Deint::None && !bwdif && !yadif;
    if (otherDeint)
    {
        return false;
    }
    if (bwdif && avfilter_get_by_name("bwdif_cuda") == nullptr)
    {
        return false;
    }
    if (yadif && avfilter_get_by_name("yadif_cuda") == nullptr)
    {
        return false;
    }
    bool const scaleOnGpu = plan.scale && !plan.fieldShift && !plan.anamorphic;
    if (scaleOnGpu && avfilter_get_by_name("scale_cuda") == nullptr)
    {
        return false;
    }
    if ((plan.fieldShift || plan.anamorphic) && !bwdif && !yadif)
    {
        return false;
    }
    return bwdif || yadif || scaleOnGpu;
}

int configureFilterGraph(AVFilterGraph** graph, AVFilterContext** source, AVFilterContext** sink, AVFrame* sample, AVRational timeBase, int sarNum,
    int sarDen, std::string const& desc, bool cuda, AVPixelFormat sinkFmt)
{
    avfilter_graph_free(graph);
    *source = nullptr;
    *sink = nullptr;
    if (sample == nullptr || sample->width <= 0 || sample->height <= 0 || desc.empty())
    {
        return AVERROR(EINVAL);
    }
    *graph = avfilter_graph_alloc();
    if (*graph == nullptr)
    {
        return AVERROR(ENOMEM);
    }
    char args[256];
    int const pix = cuda ? AV_PIX_FMT_CUDA : sample->format;
    std::snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", sample->width, sample->height, pix, timeBase.num,
        timeBase.den, std::max(1, sarNum), std::max(1, sarDen));
    int const srcOk = avfilter_graph_create_filter(source, avfilter_get_by_name("buffer"), "in", args, nullptr, *graph);
    int const sinkOk = avfilter_graph_create_filter(sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, *graph);
    if (cuda && *source != nullptr && sample->hw_frames_ctx != nullptr)
    {
        AVBufferSrcParameters* par = av_buffersrc_parameters_alloc();
        if (par != nullptr)
        {
            par->format = AV_PIX_FMT_CUDA;
            par->hw_frames_ctx = sample->hw_frames_ctx;
            par->width = sample->width;
            par->height = sample->height;
            par->time_base = timeBase;
            par->sample_aspect_ratio = AVRational{std::max(1, sarNum), std::max(1, sarDen)};
            av_buffersrc_parameters_set(*source, par);
            av_freep(&par);
        }
    }
    if (*sink != nullptr)
    {
        enum AVPixelFormat const pixfmts[] = {sinkFmt, AV_PIX_FMT_NONE};
        av_opt_set_int_list(*sink, "pix_fmts", pixfmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    }
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    if (outputs == nullptr || inputs == nullptr || srcOk < 0 || sinkOk < 0 || *source == nullptr || *sink == nullptr)
    {
        int const error = srcOk < 0 ? srcOk : (sinkOk < 0 ? sinkOk : AVERROR(ENOMEM));
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(graph);
        *source = nullptr;
        *sink = nullptr;
        return error;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = *source;
    outputs->pad_idx = 0;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = *sink;
    inputs->pad_idx = 0;
    int const parsed = avfilter_graph_parse_ptr(*graph, desc.c_str(), &inputs, &outputs, nullptr);
    int const configured = parsed >= 0 ? avfilter_graph_config(*graph, nullptr) : parsed;
    if (configured < 0)
    {
        avfilter_graph_free(graph);
        *source = nullptr;
        *sink = nullptr;
        return configured;
    }
    return 0;
}

int latencyBucket(double seconds)
{
    double const bounds[] = {0.005, 0.01, 0.02, 0.04, 0.08, 0.16, 0.32, 0.64};
    for (int i = 0; i < 8; ++i)
    {
        if (seconds <= bounds[i])
        {
            return i;
        }
    }
    return 7;
}

std::string publicRequest(ChannelConfig const& config, std::string const& nodeIp)
{
    std::ostringstream out;
    auto const& srt = config.srt;
    if (srt.mode == "listener")
    {
        out << "UDP " << nodeIp << ":" << srt.localPort;
        if (srt.exposure == "internet")
        {
            out << " NAT to this listener. Require AES-" << (effectiveKeyLength(policyFor(srt)) * 8) << " and streamid";
            if (!srt.acceptedStreamIds.empty())
            {
                out << " " << srt.acceptedStreamIds.front();
            }
            out << ".";
        }
        else
        {
            out << " listener.";
        }
    }
    else
    {
        out << "Outbound UDP to " << srt.remoteHost << ":" << srt.remotePort << ".";
    }
    return out.str();
}

struct BytePipe
{
    std::mutex mu;
    std::vector<std::uint8_t> bytes;
    std::int64_t lastMs = 0;
    bool up = false;
    std::string peer;
    SrtStats stats;

    int readSome(std::uint8_t* dst, int size, std::atomic<bool> const& stop)
    {
        for (int i = 0; i < 10 && !stop.load(); ++i)
        {
            std::unique_lock lock{mu};
            if (!bytes.empty())
            {
                int const n = std::min(size, static_cast<int>(bytes.size()));
                std::memcpy(dst, bytes.data(), static_cast<std::size_t>(n));
                bytes.erase(bytes.begin(), bytes.begin() + n);
                return n;
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard const lock{mu};
        if (bytes.empty())
        {
            return 0;
        }
        int const n = std::min(size, static_cast<int>(bytes.size()));
        std::memcpy(dst, bytes.data(), static_cast<std::size_t>(n));
        bytes.erase(bytes.begin(), bytes.begin() + n);
        return n;
    }
};

void appendPipe(BytePipe& pipe, std::uint8_t const* data, int size)
{
    std::lock_guard const lock{pipe.mu};
    pipe.bytes.insert(pipe.bytes.end(), data, data + size);
    if (pipe.bytes.size() > 4 * 1024 * 1024)
    {
        pipe.bytes.erase(pipe.bytes.begin(), pipe.bytes.begin() + static_cast<std::ptrdiff_t>(pipe.bytes.size() - 2 * 1024 * 1024));
    }
    pipe.lastMs = monoNowMs();
    pipe.up = true;
}

// The v210 codecs are intra-only and cheap to run but not to open: one context per
// thread and raster instead of one per frame.
struct V210Codec
{
    AVCodecContext* ctx = nullptr;
    int width = 0;
    int height = 0;

    ~V210Codec()
    {
        avcodec_free_context(&ctx);
    }

    AVCodecContext* get(bool encoder, int w, int h)
    {
        if (ctx != nullptr && width == w && height == h)
        {
            return ctx;
        }
        avcodec_free_context(&ctx);
        AVCodec const* codec = encoder ? avcodec_find_encoder(AV_CODEC_ID_V210) : avcodec_find_decoder(AV_CODEC_ID_V210);
        if (codec == nullptr || (ctx = avcodec_alloc_context3(codec)) == nullptr)
        {
            return nullptr;
        }
        ctx->width = w;
        ctx->height = h;
        if (encoder)
        {
            ctx->pix_fmt = AV_PIX_FMT_YUV422P10LE;
            ctx->time_base = AVRational{1, 25};
        }
        if (avcodec_open2(ctx, codec, nullptr) < 0)
        {
            avcodec_free_context(&ctx);
            return nullptr;
        }
        width = w;
        height = h;
        return ctx;
    }
};

std::vector<std::uint8_t> packV210(AVFrame* frame)
{
    std::vector<std::uint8_t> packed;
    if (frame == nullptr)
    {
        return packed;
    }
    thread_local V210Codec encoder;
    AVCodecContext* ctx = encoder.get(true, frame->width, frame->height);
    if (ctx == nullptr)
    {
        return packed;
    }
    AVPacket* packet = av_packet_alloc();
    if (avcodec_send_frame(ctx, frame) >= 0 && avcodec_receive_packet(ctx, packet) >= 0)
    {
        packed.assign(packet->data, packet->data + packet->size);
    }
    av_packet_free(&packet);
    return packed;
}

AVFrame* unpackV210(std::uint8_t const* data, int size, int width, int height)
{
    if (data == nullptr || size <= 0)
    {
        return nullptr;
    }
    thread_local V210Codec decoder;
    AVCodecContext* ctx = decoder.get(false, width, height);
    if (ctx == nullptr)
    {
        return nullptr;
    }
    AVPacket* packet = av_packet_alloc();
    packet->data = const_cast<std::uint8_t*>(data);
    packet->size = size;
    AVFrame* frame = av_frame_alloc();
    if (avcodec_send_packet(ctx, packet) < 0 || avcodec_receive_frame(ctx, frame) < 0)
    {
        av_frame_free(&frame);
    }
    packet->data = nullptr;
    packet->size = 0;
    av_packet_free(&packet);
    return frame;
}

std::vector<std::uint8_t> encodeJpeg(std::uint8_t const* v210, int width, int height)
{
    std::vector<std::uint8_t> jpeg;
    if (v210 == nullptr || width < 16 || height < 16)
    {
        return jpeg;
    }
    AVFrame* decoded = unpackV210(v210, static_cast<int>(v210Size(width, height)), width, height);
    if (decoded == nullptr)
    {
        return jpeg;
    }
    int const outW = 320;
    int const outH = std::max(16, height * outW / width);
    SwsContext* sws = sws_getContext(width, height, static_cast<AVPixelFormat>(decoded->format), outW, outH, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (sws == nullptr)
    {
        av_frame_free(&decoded);
        return jpeg;
    }
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUVJ420P;
    frame->width = outW;
    frame->height = outH;
    if (av_frame_get_buffer(frame, 0) < 0)
    {
        av_frame_free(&frame);
        sws_freeContext(sws);
        return jpeg;
    }
    sws_scale(sws, decoded->data, decoded->linesize, 0, height, frame->data, frame->linesize);
    sws_freeContext(sws);
    av_frame_free(&decoded);
    AVCodec const* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext* ctx = codec == nullptr ? nullptr : avcodec_alloc_context3(codec);
    if (ctx == nullptr)
    {
        av_frame_free(&frame);
        return jpeg;
    }
    ctx->width = outW;
    ctx->height = outH;
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->time_base = AVRational{1, 1};
    ctx->color_range = AVCOL_RANGE_JPEG;
    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        avcodec_free_context(&ctx);
        av_frame_free(&frame);
        return jpeg;
    }
    AVPacket* packet = av_packet_alloc();
    if (avcodec_send_frame(ctx, frame) >= 0 && avcodec_receive_packet(ctx, packet) >= 0)
    {
        jpeg.assign(packet->data, packet->data + packet->size);
    }
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    av_frame_free(&frame);
    return jpeg;
}

const AVCodec* findVideoDecoder(AVCodecID id, std::string const& prefer, std::string* used, bool* hardware)
{
    *hardware = false;
    if (prefer != "cpu")
    {
        char const* name = nullptr;
        if (id == AV_CODEC_ID_H264)
        {
            name = "h264_cuvid";
        }
        else if (id == AV_CODEC_ID_HEVC)
        {
            name = "hevc_cuvid";
        }
        else if (id == AV_CODEC_ID_MPEG2VIDEO)
        {
            name = "mpeg2_cuvid";
        }
        else if (id == AV_CODEC_ID_AV1)
        {
            name = "av1_cuvid";
        }
        if (name != nullptr)
        {
            if (AVCodec const* codec = avcodec_find_decoder_by_name(name))
            {
                *used = name;
                *hardware = true;
                return codec;
            }
        }
        if (prefer == "nvdec")
        {
            return nullptr;
        }
    }
    AVCodec const* codec = avcodec_find_decoder(id);
    if (codec != nullptr)
    {
        *used = std::string(avcodec_get_name(id)) + "-cpu";
    }
    return codec;
}

// NV12 CUDA frames for NVENC input, in the GPU's primary context.
AVBufferRef* cudaFramePool(int width, int height)
{
    AVBufferRef* device = nullptr;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "primary_ctx", "1", 0);
    int const created = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, nullptr, options, 0);
    av_dict_free(&options);
    if (created < 0)
    {
        return nullptr;
    }
    AVBufferRef* frames = av_hwframe_ctx_alloc(device);
    av_buffer_unref(&device);
    if (frames == nullptr)
    {
        return nullptr;
    }
    auto* ctx = reinterpret_cast<AVHWFramesContext*>(frames->data);
    ctx->format = AV_PIX_FMT_CUDA;
    ctx->sw_format = AV_PIX_FMT_NV12;
    ctx->width = width;
    ctx->height = height;
    if (av_hwframe_ctx_init(frames) < 0)
    {
        av_buffer_unref(&frames);
    }
    return frames;
}

bool openDecoder(AVCodecContext** ctx, AVCodec const* codec, AVCodecParameters const* params, AVBufferRef* hw)
{
    *ctx = avcodec_alloc_context3(codec);
    if (*ctx == nullptr || avcodec_parameters_to_context(*ctx, params) < 0)
    {
        avcodec_free_context(ctx);
        return false;
    }
    if (hw != nullptr)
    {
        (*ctx)->hw_device_ctx = av_buffer_ref(hw);
    }
    (*ctx)->pkt_timebase = params->sample_aspect_ratio.num == 0 ? AVRational{1, 90000} : (*ctx)->pkt_timebase;
    if (avcodec_open2(*ctx, codec, nullptr) < 0)
    {
        avcodec_free_context(ctx);
        return false;
    }
    return true;
}
} // namespace

struct IngestPipeline::Shared
{
    BytePipe mainPipe;
    BytePipe backupPipe;
    int active = 0;
    std::int64_t mainStableSince = 0;
    std::mutex mediaMu;
    struct Frame
    {
        // Shared: the writer takes a frame per output grain without copying the picture.
        std::shared_ptr<std::vector<std::uint8_t> const> bytes;
        std::int64_t ptsNs = 0;
        std::int64_t index = 0;
        std::string timecode;
        int width = 0;
        int height = 0;
    };
    std::deque<Frame> frames;
    struct AudioQ
    {
        int channels = 0;
        std::vector<float> samples;
        bool missing = true;
        TrackStatus info;
        std::int64_t lastMs = 0;
    };
    std::vector<AudioQ> audio;
    std::string decoder;
    std::string sourceFormat;
    VideoFormat source;
    bool sourceKnown = false;
    std::uint64_t decoded = 0;
    std::int64_t decodeWindowMs = 0;
    std::uint64_t decodeWindowFrames = 0;
    std::string sessionError;
};

IngestPipeline::IngestPipeline(ChannelConfig config, std::shared_ptr<MxlDomain> domain, std::string videoFlow, std::vector<std::string> audioFlows,
    std::string decoder)
    : config_(std::move(config))
    , domain_(std::move(domain))
    , videoFlow_(std::move(videoFlow))
    , audioFlows_(std::move(audioFlows))
    , decoderPref_(std::move(decoder))
    , shared_(std::make_unique<Shared>())
{
    status_.targetFormat = config_.target.label();
    status_.request = publicRequest(config_, config_.announceAddress.empty() ? config_.srt.localAddress : config_.announceAddress);
}

IngestPipeline::~IngestPipeline()
{
    stop();
}

void IngestPipeline::publish(PipelineStatus const& status)
{
    std::lock_guard const lock{statusMu_};
    auto jpeg = status_.jpeg;
    status_ = status;
    if (status.jpeg.empty())
    {
        status_.jpeg = std::move(jpeg);
    }
}

PipelineStatus IngestPipeline::status() const
{
    std::lock_guard const lock{statusMu_};
    return status_;
}

void IngestPipeline::start()
{
    stop_.store(false);
    mainLink_ = std::thread([this] { runLink(false); });
    if (config_.backup.enabled)
    {
        backupLink_ = std::thread([this] { runLink(true); });
    }
    io_ = std::thread([this] { runIo(); });
    clock_ = std::thread([this] { runClock(); });
}

void IngestPipeline::stop()
{
    stop_.store(true);
    if (mainLink_.joinable())
    {
        mainLink_.join();
    }
    if (backupLink_.joinable())
    {
        backupLink_.join();
    }
    if (io_.joinable())
    {
        io_.join();
    }
    if (clock_.joinable())
    {
        clock_.join();
    }
}

void IngestPipeline::runLink(bool backup)
{
    SrtLink link;
    auto endpoint = backup ? config_.backup.endpoint : config_.srt;
    if (backup && endpoint.mode.empty())
    {
        endpoint.mode = config_.srt.mode;
    }
    link.configure(endpoint, config_.id + (backup ? "/backup" : ""));
    AttemptLimiter limiter;
    int backoffMs = 1000;
    while (!stop_.load() && !srtInterrupted())
    {
        if (!link.establish(stop_, limiter))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(stop_.load() ? 1 : backoffMs));
            backoffMs = std::min(10000, backoffMs * 2);
            continue;
        }
        backoffMs = 1000;
        auto& pipe = backup ? shared_->backupPipe : shared_->mainPipe;
        {
            std::lock_guard const lock{pipe.mu};
            pipe.up = true;
            pipe.peer = link.stats().peer;
        }
        while (!stop_.load() && link.connected())
        {
            if (!backup)
            {
                link.rejectPending(limiter);
            }
            std::uint8_t buffer[1500];
            int const n = link.recv(buffer, static_cast<int>(sizeof(buffer)));
            if (n > 0)
            {
                appendPipe(pipe, buffer, n);
            }
            else if (n < 0)
            {
                break;
            }
            auto const stats = link.stats();
            std::lock_guard const lock{pipe.mu};
            pipe.stats = stats;
            pipe.peer = stats.peer;
            pipe.up = true;
        }
        std::lock_guard const lock{pipe.mu};
        pipe.up = false;
        link.closeData();
    }
    link.closeAll();
}

int readActive(void* opaque, std::uint8_t* buf, int bufSize)
{
    auto* self = static_cast<IngestPipeline*>(opaque);
    auto& shared = *self->shared_;
    BytePipe* pipe = &shared.mainPipe;
    if (self->config_.backup.enabled)
    {
        auto const now = monoNowMs();
        if (self->config_.backup.force == "backup")
        {
            shared.active = 1;
        }
        else if (self->config_.backup.force == "main")
        {
            shared.active = 0;
        }
        else
        {
            bool const mainDead = shared.mainPipe.lastMs == 0 || now - shared.mainPipe.lastMs > self->config_.backup.failoverMs;
            bool const backupLive = shared.backupPipe.lastMs != 0 && now - shared.backupPipe.lastMs < self->config_.backup.failoverMs;
            if (shared.active == 0 && mainDead && backupLive)
            {
                shared.active = 1;
                shared.mainStableSince = 0;
                log::warn("srt_failover", {{"channel", self->config_.id}, {"active", "backup"}});
            }
            if (shared.active == 1)
            {
                bool const mainLive = shared.mainPipe.lastMs != 0 && now - shared.mainPipe.lastMs < 400;
                if (mainLive)
                {
                    if (shared.mainStableSince == 0)
                    {
                        shared.mainStableSince = now;
                    }
                    if (now - shared.mainStableSince >= self->config_.backup.failbackMs)
                    {
                        shared.active = 0;
                        log::info("srt_failback", {{"channel", self->config_.id}});
                    }
                }
                else
                {
                    shared.mainStableSince = 0;
                }
            }
        }
        pipe = shared.active == 1 ? &shared.backupPipe : &shared.mainPipe;
    }
    if (self->stop_.load())
    {
        return AVERROR_EXIT;
    }
    int const n = pipe->readSome(buf, bufSize, self->stop_);
    if (n == 0)
    {
        return AVERROR(EAGAIN);
    }
    return n;
}

void IngestPipeline::runIo()
{
    ffmpegOnce();
    while (!stop_.load())
    {
        AVFormatContext* fmt = avformat_alloc_context();
        if (fmt == nullptr)
        {
            return;
        }
        int const bufferSize = 64 * 1024;
        auto* avioBuffer = static_cast<std::uint8_t*>(av_malloc(static_cast<std::size_t>(bufferSize)));
        AVIOContext* avio = avio_alloc_context(avioBuffer, bufferSize, 0, this, readActive, nullptr, nullptr);
        fmt->pb = avio;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        fmt->probesize = 2 * 1024 * 1024;
        fmt->max_analyze_duration = 800000;
        fmt->interrupt_callback.callback = [](void* opaque) { return static_cast<std::atomic<bool>*>(opaque)->load() ? 1 : 0; };
        fmt->interrupt_callback.opaque = &stop_;
        if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0)
        {
            if (avio != nullptr)
            {
                av_freep(&avio->buffer);
                avio_context_free(&avio);
            }
            avformat_free_context(fmt);
            if (stop_.load())
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        avformat_find_stream_info(fmt, nullptr);
        int programIndex = 0;
        if (config_.program.select == "number")
        {
            for (unsigned i = 0; i < fmt->nb_programs; ++i)
            {
                if (fmt->programs[i]->id == config_.program.number)
                {
                    programIndex = static_cast<int>(i);
                }
            }
        }
        else if (config_.program.select == "name")
        {
            for (unsigned i = 0; i < fmt->nb_programs; ++i)
            {
                AVDictionaryEntry* service = av_dict_get(fmt->programs[i]->metadata, "service_name", nullptr, 0);
                if (service != nullptr && config_.program.serviceName == service->value)
                {
                    programIndex = static_cast<int>(i);
                }
            }
        }
        std::vector<int> streams;
        if (fmt->nb_programs > 0)
        {
            auto* program = fmt->programs[programIndex];
            for (unsigned i = 0; i < program->nb_stream_indexes; ++i)
            {
                streams.push_back(static_cast<int>(program->stream_index[i]));
            }
        }
        else
        {
            for (unsigned i = 0; i < fmt->nb_streams; ++i)
            {
                streams.push_back(static_cast<int>(i));
            }
        }
        int videoStream = -1;
        std::vector<int> audioStreams;
        for (int index : streams)
        {
            auto const type = fmt->streams[index]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO && videoStream < 0)
            {
                if (config_.program.videoPid == 0 || fmt->streams[index]->id == config_.program.videoPid)
                {
                    videoStream = index;
                }
            }
            else if (type == AVMEDIA_TYPE_AUDIO)
            {
                if (config_.program.audioPids.empty() ||
                    std::find(config_.program.audioPids.begin(), config_.program.audioPids.end(), fmt->streams[index]->id) != config_.program.audioPids.end())
                {
                    audioStreams.push_back(index);
                }
            }
        }
        if (videoStream < 0)
        {
            avformat_close_input(&fmt);
            if (avio != nullptr)
            {
                av_freep(&avio->buffer);
                avio_context_free(&avio);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }

        AVBufferRef* hw = nullptr;
        std::string decoderName;
        bool hardware = false;
        AVCodec const* videoCodec = findVideoDecoder(fmt->streams[videoStream]->codecpar->codec_id, decoderPref_, &decoderName, &hardware);
        AVCodecContext* videoCtx = nullptr;
        if (hardware)
        {
            // Every channel shares the GPU's primary context. A context per connection
            // costs device memory, and kernels of different contexts never overlap.
            AVDictionary* options = nullptr;
            av_dict_set(&options, "primary_ctx", "1", 0);
            int const created = av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_CUDA, nullptr, options, 0);
            av_dict_free(&options);
            if (created < 0)
            {
                hw = nullptr;
                hardware = false;
                videoCodec = avcodec_find_decoder(fmt->streams[videoStream]->codecpar->codec_id);
                decoderName = std::string(avcodec_get_name(fmt->streams[videoStream]->codecpar->codec_id)) + "-cpu";
                log::info("nvdec_unavailable", {{"channel", config_.id}, {"fallback", "cpu"}});
            }
        }
        if (videoCodec == nullptr || !openDecoder(&videoCtx, videoCodec, fmt->streams[videoStream]->codecpar, hardware ? hw : nullptr))
        {
            if (hardware)
            {
                log::info("nvdec_fallback", {{"channel", config_.id}, {"reason", "open_failed"}});
                hardware = false;
                videoCodec = avcodec_find_decoder(fmt->streams[videoStream]->codecpar->codec_id);
                decoderName = std::string(avcodec_get_name(fmt->streams[videoStream]->codecpar->codec_id)) + "-cpu";
                openDecoder(&videoCtx, videoCodec, fmt->streams[videoStream]->codecpar, nullptr);
            }
        }
        if (videoCtx == nullptr)
        {
            avformat_close_input(&fmt);
            av_buffer_unref(&hw);
            if (avio != nullptr)
            {
                av_freep(&avio->buffer);
                avio_context_free(&avio);
            }
            PipelineStatus failed;
            failed.state = "error";
            failed.error = "video decoder failed to open";
            failed.targetFormat = config_.target.label();
            publish(failed);
            return;
        }
        {
            std::lock_guard const lock{shared_->mediaMu};
            shared_->decoder = decoderName;
        }

        struct AudioDec
        {
            int stream = -1;
            AVCodecContext* ctx = nullptr;
            SwrContext* swr = nullptr;
            int index = 0;
        };
        std::vector<AudioDec> audio;
        int audioIndex = 0;
        for (int index : audioStreams)
        {
            AVCodec const* codec = avcodec_find_decoder(fmt->streams[index]->codecpar->codec_id);
            AVCodecContext* ctx = nullptr;
            if (codec == nullptr || !openDecoder(&ctx, codec, fmt->streams[index]->codecpar, nullptr))
            {
                continue;
            }
            AudioDec dec;
            dec.stream = index;
            dec.ctx = ctx;
            dec.index = audioIndex++;
            audio.push_back(dec);
            TrackStatus info;
            info.pid = fmt->streams[index]->id;
            info.codec = avcodec_get_name(fmt->streams[index]->codecpar->codec_id);
            info.layout = std::to_string(fmt->streams[index]->codecpar->ch_layout.nb_channels) + "ch";
            if (AVDictionaryEntry* lang = av_dict_get(fmt->streams[index]->metadata, "language", nullptr, 0))
            {
                info.language = lang->value;
            }
            std::lock_guard const lock{shared_->mediaMu};
            if (static_cast<int>(shared_->audio.size()) <= dec.index)
            {
                shared_->audio.resize(static_cast<std::size_t>(dec.index + 1));
            }
            shared_->audio[static_cast<std::size_t>(dec.index)].info = info;
            shared_->audio[static_cast<std::size_t>(dec.index)].channels = fmt->streams[index]->codecpar->ch_layout.nb_channels;
        }

        AVFilterGraph* graph = nullptr;
        AVFilterContext* sourceFilter = nullptr;
        AVFilterContext* sinkFilter = nullptr;
        VideoFormat filteredSource;
        bool haveGraph = false;
        bool graphCuda = false;
        // The graph ends on the GPU and its frames are packed to v210 there.
        bool graphOnGpu = false;
        bool outputInterlaced = false;
        bool gpuPackFailed = false;
        double offset = 0;
        bool haveOffset = false;
        std::int64_t frameIndex = 0;
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        AVFrame* filtered = av_frame_alloc();
        while (!stop_.load())
        {
            int const read = av_read_frame(fmt, packet);
            if (read == AVERROR(EAGAIN))
            {
                continue;
            }
            if (read < 0)
            {
                break;
            }
            if (packet->stream_index == videoStream)
            {
                if (avcodec_send_packet(videoCtx, packet) >= 0)
                {
                    while (avcodec_receive_frame(videoCtx, frame) >= 0)
                    {
                        bool const interlaced = (frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
                        bool const top = (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
                        VideoFormat detected;
                        detected.width = frame->width;
                        detected.height = frame->height;
                        detected.interlaced = interlaced;
                        detected.fieldOrder = interlaced ? (top ? "tff" : "bff") : "progressive";
                        AVRational guessed = av_guess_frame_rate(fmt, fmt->streams[videoStream], frame);
                        if (guessed.num <= 0)
                        {
                            guessed = fmt->streams[videoStream]->avg_frame_rate;
                        }
                        detected.rate.num = guessed.num > 0 ? guessed.num : 25;
                        detected.rate.den = guessed.den > 0 ? guessed.den : 1;
                        for (auto const& known : knownRates())
                        {
                            if (known.sameAs(Rate{detected.rate.num, detected.rate.den, known.name}))
                            {
                                detected.rate = known;
                                break;
                            }
                        }
                        if (detected.rate.name.empty())
                        {
                            detected.rate.name = std::to_string(detected.rate.num) + "/" + std::to_string(detected.rate.den);
                        }
                        detected.sarNum = frame->sample_aspect_ratio.num > 0 ? frame->sample_aspect_ratio.num : 1;
                        detected.sarDen = frame->sample_aspect_ratio.den > 0 ? frame->sample_aspect_ratio.den : 1;
                        detected.color = frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M ? "bt601" : "bt709";
                        if (detected.width != filteredSource.width || detected.height != filteredSource.height || detected.interlaced != filteredSource.interlaced ||
                            !detected.rate.sameAs(filteredSource.rate) || filteredSource.fieldOrder != detected.fieldOrder)
                        {
                            auto const plan = planAdaptation(detected, config_.target, config_.deinterlacer, config_.aspect, config_.scale, config_.sourceScan);
                            auto const timeBase = fmt->streams[videoStream]->time_base;
                            bool built = false;
                            graphCuda = false;
                            graphOnGpu = false;
                            outputInterlaced = plan.output.interlaced;
                            if (frame->format == AV_PIX_FMT_CUDA && frame->hw_frames_ctx != nullptr && cudaFilterChainAvailable(plan))
                            {
                                // When every step runs on CUDA, the frame stays on the GPU and is
                                // packed to v210 there; only the packed picture is downloaded.
                                bool const keep = !gpuPackFailed && cudaconvert::available();
                                std::string const desc = ffmpegFilter(plan, true, keep);
                                bool const onGpu = keep && desc.find("hwdownload") == std::string::npos;
                                int const graphError = configureFilterGraph(&graph, &sourceFilter, &sinkFilter, frame, timeBase, detected.sarNum, detected.sarDen, desc,
                                    true, onGpu ? AV_PIX_FMT_CUDA : AV_PIX_FMT_YUV422P10LE);
                                built = graphError >= 0;
                                if (built)
                                {
                                    graphCuda = true;
                                    graphOnGpu = onGpu;
                                    log::info("adapter_configured",
                                        {{"channel", config_.id}, {"plan", plan.summary}, {"cuda", "true"}, {"v210", onGpu ? "gpu" : "cpu"}});
                                }
                                else
                                {
                                    log::info("cuda_filter_fallback", {{"channel", config_.id}, {"graph", desc}, {"error", ffError(graphError)}});
                                }
                            }
                            AVFrame* probe = nullptr;
                            AVFrame* sample = frame;
                            if (!built && frame->format == AV_PIX_FMT_CUDA)
                            {
                                probe = av_frame_alloc();
                                if (probe != nullptr && av_hwframe_transfer_data(probe, frame, 0) >= 0)
                                {
                                    probe->pts = frame->pts;
                                    probe->sample_aspect_ratio = frame->sample_aspect_ratio;
                                    sample = probe;
                                }
                                else
                                {
                                    av_frame_free(&probe);
                                }
                            }
                            if (!built && sample->format != AV_PIX_FMT_CUDA)
                            {
                                std::string desc = ffmpegFilter(plan, false);
                                if (detected.interlaced)
                                {
                                    desc = std::string("setfield=") + (detected.fieldOrder == "bff" ? "bff" : "tff") + "," + desc;
                                }
                                int const graphError = configureFilterGraph(&graph, &sourceFilter, &sinkFilter, sample, timeBase, detected.sarNum, detected.sarDen, desc, false,
                                    AV_PIX_FMT_YUV422P10LE);
                                built = graphError >= 0;
                                graphCuda = false;
                                if (!built)
                                {
                                    log::error("filter_graph_failed", {{"channel", config_.id}, {"graph", desc}, {"error", ffError(graphError)}});
                                }
                                else
                                {
                                    log::info("adapter_configured", {{"channel", config_.id}, {"plan", plan.summary}, {"cuda", "false"}});
                                }
                            }
                            else if (!built)
                            {
                                log::error("filter_graph_failed", {{"channel", config_.id}, {"reason", "cuda_download"}});
                            }
                            av_frame_free(&probe);
                            haveGraph = built;
                            filteredSource = detected;
                            std::lock_guard const lock{shared_->mediaMu};
                            shared_->source = detected;
                            shared_->sourceKnown = true;
                            shared_->sourceFormat = detected.label();
                        }
                        AVFrame* input = frame;
                        AVFrame* transferred = nullptr;
                        if (haveGraph && !graphCuda && frame->format == AV_PIX_FMT_CUDA)
                        {
                            transferred = av_frame_alloc();
                            if (transferred != nullptr && av_hwframe_transfer_data(transferred, frame, 0) >= 0)
                            {
                                transferred->pts = frame->pts;
                                input = transferred;
                            }
                            else
                            {
                                av_frame_free(&transferred);
                                input = nullptr;
                            }
                        }
                        if (haveGraph && input != nullptr)
                        {
                            input->pts = frame->pts;
                            if (av_buffersrc_add_frame_flags(sourceFilter, input, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0)
                            {
                                while (av_buffersink_get_frame(sinkFilter, filtered) >= 0)
                                {
                                    std::int64_t const ptsNs = av_rescale_q(filtered->pts, av_buffersink_get_time_base(sinkFilter), AVRational{1, 1000000000});
                                    double const now = static_cast<double>(taiNowNs());
                                    double const sample = now - static_cast<double>(ptsNs);
                                    if (!haveOffset)
                                    {
                                        offset = sample;
                                        haveOffset = true;
                                    }
                                    else
                                    {
                                        offset = offset * 0.98 + sample * 0.02;
                                    }
                                    IngestPipeline::Shared::Frame adapted;
                                    adapted.width = filtered->width;
                                    adapted.height = filtered->height;
                                    adapted.ptsNs = ptsNs + static_cast<std::int64_t>(offset);
                                    adapted.index = frameIndex++;
                                    if (graphOnGpu)
                                    {
                                        auto packed = std::make_shared<std::vector<std::uint8_t>>();
                                        if (cudaconvert::toV210(filtered, outputInterlaced, *packed))
                                        {
                                            adapted.bytes = std::move(packed);
                                        }
                                        else
                                        {
                                            // Rebuild on the next frame with the CPU conversion.
                                            log::error("gpu_v210_failed", {{"channel", config_.id}});
                                            gpuPackFailed = true;
                                            filteredSource = VideoFormat{};
                                        }
                                    }
                                    else
                                    {
                                        adapted.bytes = std::make_shared<std::vector<std::uint8_t> const>(packV210(filtered));
                                    }
                                    if (AVFrameSideData* side = av_frame_get_side_data(filtered, AV_FRAME_DATA_S12M_TIMECODE))
                                    {
                                        if (side->size >= 16)
                                        {
                                            adapted.timecode = "sei";
                                        }
                                    }
                                    {
                                        std::lock_guard const lock{shared_->mediaMu};
                                        shared_->frames.push_back(std::move(adapted));
                                        while (shared_->frames.size() > 8)
                                        {
                                            shared_->frames.pop_front();
                                        }
                                        shared_->decoded++;
                                        auto const nowMs = monoNowMs();
                                        if (shared_->decodeWindowMs == 0)
                                        {
                                            shared_->decodeWindowMs = nowMs;
                                        }
                                        shared_->decodeWindowFrames++;
                                        if (nowMs - shared_->decodeWindowMs >= 1000)
                                        {
                                            shared_->decodeWindowMs = nowMs;
                                            shared_->decodeWindowFrames = 0;
                                        }
                                    }
                                    av_frame_unref(filtered);
                                }
                            }
                        }
                        av_frame_free(&transferred);
                        av_frame_unref(frame);
                    }
                }
            }
            else
            {
                for (auto& dec : audio)
                {
                    if (packet->stream_index != dec.stream)
                    {
                        continue;
                    }
                    if (avcodec_send_packet(dec.ctx, packet) < 0)
                    {
                        break;
                    }
                    while (avcodec_receive_frame(dec.ctx, frame) >= 0)
                    {
                        int const channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 2;
                        if (dec.swr == nullptr)
                        {
                            AVChannelLayout inLayout = frame->ch_layout;
                            AVChannelLayout outLayout;
                            av_channel_layout_default(&outLayout, channels);
                            if (swr_alloc_set_opts2(&dec.swr, &outLayout, AV_SAMPLE_FMT_FLT, 48000, &inLayout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0,
                                    nullptr) < 0)
                            {
                                dec.swr = nullptr;
                            }
                            else
                            {
                                av_opt_set(dec.swr, "resampler", "soxr", 0);
                                if (swr_init(dec.swr) < 0)
                                {
                                    swr_free(&dec.swr);
                                    swr_alloc_set_opts2(&dec.swr, &outLayout, AV_SAMPLE_FMT_FLT, 48000, &inLayout, static_cast<AVSampleFormat>(frame->format),
                                        frame->sample_rate, 0, nullptr);
                                    swr_init(dec.swr);
                                }
                            }
                            av_channel_layout_uninit(&outLayout);
                        }
                        if (dec.swr != nullptr)
                        {
                            int const outCount = swr_get_out_samples(dec.swr, frame->nb_samples) + 64;
                            std::vector<float> converted(static_cast<std::size_t>(outCount * channels));
                            std::uint8_t* outPlanes[1] = {reinterpret_cast<std::uint8_t*>(converted.data())};
                            std::uint8_t const* inPlanes[8] = {};
                            for (int plane = 0; plane < 8; ++plane)
                            {
                                inPlanes[plane] = frame->extended_data[plane];
                            }
                            int const got = swr_convert(dec.swr, outPlanes, outCount, inPlanes, frame->nb_samples);
                            if (got > 0)
                            {
                                int const levelTarget = std::max(48, config_.syncLatencyMs * 48);
                                std::lock_guard const lock{shared_->mediaMu};
                                auto& queue = shared_->audio[static_cast<std::size_t>(dec.index)];
                                queue.channels = channels;
                                queue.samples.insert(queue.samples.end(), converted.begin(), converted.begin() + static_cast<std::ptrdiff_t>(got * channels));
                                queue.missing = false;
                                queue.lastMs = monoNowMs();
                                int const level = static_cast<int>(queue.samples.size() / static_cast<std::size_t>(channels));
                                int const error = level - levelTarget;
                                if (std::abs(error) > levelTarget / 5)
                                {
                                    swr_set_compensation(dec.swr, error > 0 ? -40 : 40, 4800);
                                }
                                if (level > levelTarget + 48000)
                                {
                                    int const drop = channels * 480;
                                    if (static_cast<int>(queue.samples.size()) > drop)
                                    {
                                        queue.samples.erase(queue.samples.begin(), queue.samples.begin() + drop);
                                    }
                                }
                            }
                        }
                        av_frame_unref(frame);
                    }
                }
            }
            av_packet_unref(packet);
        }
        for (auto& dec : audio)
        {
            swr_free(&dec.swr);
            avcodec_free_context(&dec.ctx);
        }
        avfilter_graph_free(&graph);
        avcodec_free_context(&videoCtx);
        av_buffer_unref(&hw);
        av_frame_free(&filtered);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avformat_close_input(&fmt);
        if (avio != nullptr)
        {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
    }
}

void IngestPipeline::runClock()
{
    std::unique_ptr<MxlVideoWriter> video;
    std::vector<std::unique_ptr<MxlAudioWriter>> audioWriters;
    try
    {
        if (domain_)
        {
            video = std::make_unique<MxlVideoWriter>(*domain_, videoFlow_, config_.label + " video", config_.label + ":Video", config_.target);
            for (std::size_t i = 0; i < audioFlows_.size() && i < config_.audioOutputs.size(); ++i)
            {
                audioWriters.push_back(std::make_unique<MxlAudioWriter>(*domain_, audioFlows_[i], config_.label + " audio " + std::to_string(i + 1),
                    config_.label + ":Audio " + std::to_string(i + 1), config_.audioOutputs[i].channels));
            }
        }
    }
    catch (std::exception const& ex)
    {
        PipelineStatus failed;
        failed.state = "error";
        failed.error = ex.what();
        failed.targetFormat = config_.target.label();
        publish(failed);
        return;
    }
    FrameSynchroniser sync(static_cast<std::int64_t>(config_.syncLatencyMs) * 1000000LL, static_cast<std::int64_t>(config_.holdMs) * 1000000LL);
    auto const slate = renderSlate(config_.target.width, config_.target.height, config_.label, config_.lossMode != "black");
    auto const black = renderSlate(config_.target.width, config_.target.height, config_.label, false);
    std::uint64_t index = grainIndexNow(config_.target.rate);
    std::uint64_t repeats = 0;
    std::uint64_t drops = 0;
    std::uint64_t frames = 0;
    std::int64_t lastJpeg = 0;
    while (!stop_.load())
    {
        std::uint64_t const nowIndex = grainIndexNow(config_.target.rate);
        if (nowIndex < index)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (nowIndex > index + 8)
        {
            index = nowIndex;
        }
        std::vector<SourceFrame> stamps;
        Shared::Frame chosen;
        bool haveChosen = false;
        std::string sourceFormat;
        std::string decoder;
        std::vector<Shared::AudioQ> audioCopy;
        bool failover = false;
        SrtStats srtStats;
        std::string peer;
        double decodeFps = 0;
        {
            std::lock_guard const lock{shared_->mediaMu};
            for (auto const& frame : shared_->frames)
            {
                stamps.push_back(SourceFrame{frame.index, frame.ptsNs});
            }
            sourceFormat = shared_->sourceFormat;
            decoder = shared_->decoder;
            audioCopy = shared_->audio;
            auto const now = monoNowMs();
            if (shared_->decodeWindowMs != 0 && now > shared_->decodeWindowMs)
            {
                decodeFps = shared_->decodeWindowFrames * 1000.0 / static_cast<double>(now - shared_->decodeWindowMs + 1);
            }
        }
        {
            std::lock_guard const lock{shared_->mainPipe.mu};
            srtStats = shared_->active == 1 ? shared_->backupPipe.stats : shared_->mainPipe.stats;
            peer = shared_->active == 1 ? shared_->backupPipe.peer : shared_->mainPipe.peer;
            if (!shared_->mainPipe.up && shared_->active == 0)
            {
                srtStats.connected = false;
            }
        }
        failover = shared_->active == 1;
        auto const decision = sync.choose(static_cast<std::int64_t>(grainTimeNs(config_.target.rate, index)), stamps);
        sync.commit(decision);
        if (decision.repeat)
        {
            ++repeats;
        }
        drops += static_cast<std::uint64_t>(decision.dropped);
        std::uint8_t const* picture = config_.lossMode == "black" ? black.data() : slate.data();
        std::size_t pictureSize = config_.lossMode == "black" ? black.size() : slate.size();
        std::string timecode;
        if (!decision.slate)
        {
            std::lock_guard const lock{shared_->mediaMu};
            for (auto const& frame : shared_->frames)
            {
                if (frame.index == decision.sourceIndex && frame.bytes)
                {
                    chosen = frame;
                    haveChosen = true;
                    timecode = frame.timecode;
                    picture = chosen.bytes->data();
                    pictureSize = chosen.bytes->size();
                    break;
                }
            }
        }
        if (video)
        {
            video->write(index, picture, pictureSize, false);
        }
        int const samples = samplesPerGrain(static_cast<std::int64_t>(index), config_.target.rate.num, config_.target.rate.den);
        std::vector<double> meters;
        std::vector<std::string> alarms;
        if (!config_.audioOutputs.empty())
        {
            meters.assign(static_cast<std::size_t>(config_.audioOutputs.front().channels), 0.0);
        }
        std::vector<TrackView> views;
        double fifoMs = 0;
        double drift = 0;
        for (auto& queue : audioCopy)
        {
            bool const stale = queue.lastMs != 0 && monoNowMs() - queue.lastMs > 1000 && queue.samples.empty();
            if (stale)
            {
                queue.missing = true;
                alarms.push_back("audio pid " + std::to_string(queue.info.pid) + " missing");
            }
            TrackView view;
            view.channels = queue.channels;
            view.samples = queue.samples.empty() ? nullptr : queue.samples.data();
            view.missing = queue.samples.empty();
            views.push_back(view);
            if (queue.channels > 0)
            {
                fifoMs = std::max(fifoMs, static_cast<double>(queue.samples.size() / static_cast<std::size_t>(queue.channels)) / 48.0);
                double const target = static_cast<double>(config_.syncLatencyMs);
                drift = (fifoMs - target) / std::max(1.0, target) * 1000000.0;
            }
        }
        bool consumed = false;
        std::size_t const outputs = std::max<std::size_t>(config_.audioOutputs.size(), audioWriters.empty() ? 0 : 1);
        for (std::size_t out = 0; out < config_.audioOutputs.size(); ++out)
        {
            std::vector<float> mixed(static_cast<std::size_t>(samples * config_.audioOutputs[out].channels), 0.f);
            if (!decision.slate)
            {
                applyMatrix(views, config_.audioOutputs[out], samples, mixed.data());
            }
            if (out < audioWriters.size())
            {
                std::uint64_t const end = static_cast<std::uint64_t>(sampleIndexAtGrain(static_cast<std::int64_t>(index + 1), config_.target.rate.num, config_.target.rate.den));
                audioWriters[out]->write(end, mixed.data(), samples, config_.audioOutputs[out].channels);
            }
            if (out == 0)
            {
                for (int ch = 0; ch < config_.audioOutputs[out].channels && ch < static_cast<int>(meters.size()); ++ch)
                {
                    float peak = 0;
                    for (int frame = 0; frame < samples; ++frame)
                    {
                        peak = std::max(peak, std::fabs(mixed[static_cast<std::size_t>(frame * config_.audioOutputs[out].channels + ch)]));
                    }
                    meters[static_cast<std::size_t>(ch)] = peak;
                }
            }
            if (!decision.slate && !consumed)
            {
                consumed = true;
                std::lock_guard const lock{shared_->mediaMu};
                for (std::size_t q = 0; q < shared_->audio.size(); ++q)
                {
                    auto& live = shared_->audio[q];
                    if (live.channels <= 0)
                    {
                        continue;
                    }
                    int const consume = samples * live.channels;
                    if (static_cast<int>(live.samples.size()) >= consume)
                    {
                        live.samples.erase(live.samples.begin(), live.samples.begin() + consume);
                    }
                    else
                    {
                        live.samples.clear();
                    }
                }
            }
        }
        (void)outputs;
        ++frames;
        if (monoNowMs() - lastJpeg > 1000 && haveChosen)
        {
            auto jpeg = encodeJpeg(chosen.bytes->data(), chosen.width, chosen.height);
            std::lock_guard const lock{statusMu_};
            status_.jpeg = std::move(jpeg);
            lastJpeg = monoNowMs();
        }
        PipelineStatus status;
        status.state = decision.slate ? (srtStats.connected || shared_->mainPipe.lastMs != 0 ? "no_signal" : "connecting") : "running";
        if (!srtStats.connected && shared_->mainPipe.lastMs == 0 && shared_->backupPipe.lastMs == 0)
        {
            status.state = "connecting";
        }
        status.peer = peer;
        status.srt = srtStats;
        status.sourceFormat = sourceFormat;
        status.targetFormat = config_.target.label();
        status.decoder = decoder;
        status.frames = frames;
        status.repeats = repeats;
        status.drops = drops;
        status.driftPpm = drift;
        status.fifoMs = fifoMs;
        status.decodeFps = decodeFps;
        status.failover = failover;
        status.timecode = timecode;
        status.meters = meters;
        status.alarms = alarms;
        for (auto const& queue : audioCopy)
        {
            status.tracks.push_back(queue.info);
            status.tracks.back().missing = queue.missing;
        }
        status.request = publicRequest(config_, config_.announceAddress.empty() ? primaryIpv4() : config_.announceAddress);
        publish(status);
        ++index;
    }
}

EgressPipeline::EgressPipeline(ChannelConfig config, std::string encoder, DomainFn domains, RouteFn routes)
    : config_(std::move(config))
    , encoderPref_(std::move(encoder))
    , domains_(std::move(domains))
    , routes_(std::move(routes))
{
    status_.targetFormat = config_.target.label();
    status_.state = "idle";
}

EgressPipeline::~EgressPipeline()
{
    stop();
}

void EgressPipeline::start()
{
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
}

void EgressPipeline::stop()
{
    stop_.store(true);
    if (thread_.joinable())
    {
        thread_.join();
    }
}

PipelineStatus EgressPipeline::status() const
{
    std::lock_guard const lock{statusMu_};
    return status_;
}

#if LIBAVFORMAT_VERSION_MAJOR >= 61
int writeEgress(void* opaque, std::uint8_t const* buf, int size)
#else
int writeEgress(void* opaque, std::uint8_t* buf, int size)
#endif
{
    auto* self = static_cast<EgressPipeline*>(opaque);
    if (self->livePrimary_ != nullptr && self->livePrimary_->connected())
    {
        self->livePrimary_->send(buf, size);
    }
    if (self->liveCopy_ != nullptr && self->liveCopy_->connected())
    {
        self->liveCopy_->send(buf, size);
    }
    return size;
}

void EgressPipeline::run()
{
    ffmpegOnce();
    SrtLink primary;
    SrtLink duplicate;
    primary.configure(config_.srt, config_.id);
    if (config_.backup.enabled)
    {
        duplicate.configure(config_.backup.endpoint, config_.id + "/copy");
    }
    livePrimary_ = &primary;
    liveCopy_ = &duplicate;
    AttemptLimiter limiter;
    std::thread connector([&] {
        int backoff = 1000;
        while (!stop_.load())
        {
            if (!primary.connected())
            {
                if (!primary.establish(stop_, limiter))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
                    backoff = std::min(10000, backoff * 2);
                }
                else
                {
                    backoff = 1000;
                }
            }
            else
            {
                primary.rejectPending(limiter);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (config_.backup.enabled && !duplicate.connected() && !stop_.load())
            {
                duplicate.establish(stop_, limiter);
            }
        }
        primary.closeAll();
        duplicate.closeAll();
    });

    auto const slateFrame = renderSlate(config_.target.width, config_.target.height, config_.label, true);
    MxlVideoReader videoReader;
    MxlAudioReader audioReader;
    std::shared_ptr<MxlDomain> openDomain;
    std::string openVideo;
    std::string openAudio;
    std::unique_ptr<MxlSync> sync;
    AVCodecContext* videoEnc = nullptr;
    // NVENC input frames on the GPU (primary CUDA context), when the converter is available.
    AVBufferRef* gpuFrames = nullptr;
    bool gpuConvertFailed = false;
    AVFilterGraph* graph = nullptr;
    AVFilterContext* sourceFilter = nullptr;
    AVFilterContext* sinkFilter = nullptr;
    VideoFormat graphSource;
    std::vector<AVCodecContext*> audioEnc;
    AVFormatContext* mux = nullptr;
    AVIOContext* avio = nullptr;
    std::int64_t videoPts = 0;
    bool needKeyframe = true;
    std::uint64_t frames = 0;
    std::uint64_t repeats = 0;
    std::uint64_t drops = 0;
    std::uint64_t lastSource = 0;
    bool haveSource = false;
    auto const started = std::chrono::steady_clock::now();

    auto closeMux = [&] {
        if (mux != nullptr)
        {
            if (mux->pb != nullptr)
            {
                av_write_trailer(mux);
            }
            avformat_free_context(mux);
            mux = nullptr;
        }
        if (avio != nullptr)
        {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
        for (auto* ctx : audioEnc)
        {
            avcodec_free_context(&ctx);
        }
        audioEnc.clear();
        avcodec_free_context(&videoEnc);
        av_buffer_unref(&gpuFrames);
        avfilter_graph_free(&graph);
    };

    auto ensureEncoder = [&](std::string* error) {
        if (videoEnc != nullptr)
        {
            return true;
        }
        std::string name;
        bool nvenc = encoderPref_ != "cpu";
        if (nvenc)
        {
            name = config_.egress.codec == "hevc" ? "hevc_nvenc" : "h264_nvenc";
        }
        AVCodec const* codec = nvenc ? avcodec_find_encoder_by_name(name.c_str()) : nullptr;
        if (codec == nullptr)
        {
            if (encoderPref_ == "nvenc")
            {
                *error = "NVENC is not available";
                return false;
            }
            name = config_.egress.codec == "hevc" ? "libx265" : "libx264";
            codec = avcodec_find_encoder_by_name(name.c_str());
            nvenc = false;
        }
        if (codec == nullptr)
        {
            *error = "encoder " + name + " is not available in this build";
            return false;
        }
        if (config_.target.interlaced && config_.egress.codec == "hevc")
        {
            *error = "HEVC interlaced encoding is not supported";
            return false;
        }
        videoEnc = avcodec_alloc_context3(codec);
        videoEnc->width = config_.target.width;
        videoEnc->height = config_.target.height;
        videoEnc->pix_fmt = AV_PIX_FMT_YUV420P;
        videoEnc->time_base = AVRational{config_.target.rate.den, config_.target.rate.num};
        videoEnc->framerate = AVRational{config_.target.rate.num, config_.target.rate.den};
        videoEnc->bit_rate = config_.egress.bitrate;
        videoEnc->rc_max_rate = config_.egress.bitrate;
        videoEnc->rc_buffer_size = config_.egress.bitrate;
        videoEnc->gop_size = std::max(1, static_cast<int>(std::llround(config_.target.rate.hz() * config_.egress.gopSeconds)));
        videoEnc->max_b_frames = config_.egress.bframes;
        videoEnc->color_primaries = AVCOL_PRI_BT709;
        videoEnc->color_trc = AVCOL_TRC_BT709;
        videoEnc->colorspace = AVCOL_SPC_BT709;
        if (config_.target.interlaced)
        {
            videoEnc->flags |= AV_CODEC_FLAG_INTERLACED_DCT;
        }
        if (nvenc)
        {
            av_opt_set(videoEnc->priv_data, "preset", config_.egress.nvencPreset.c_str(), 0);
            av_opt_set(videoEnc->priv_data, "tune", config_.egress.nvencTune.c_str(), 0);
            av_opt_set(videoEnc->priv_data, "rc", "cbr", 0);
            av_opt_set(videoEnc->priv_data, "forced-idr", "1", 0);
            // NVENC takes CUDA frames: v210 becomes NV12 on the GPU instead of being
            // unpacked and converted on the CPU.
            if (cudaconvert::available() && gpuFrames == nullptr)
            {
                gpuFrames = cudaFramePool(config_.target.width, config_.target.height);
            }
            if (gpuFrames != nullptr)
            {
                videoEnc->pix_fmt = AV_PIX_FMT_CUDA;
                videoEnc->hw_frames_ctx = av_buffer_ref(gpuFrames);
            }
        }
        else if (name == "libx264")
        {
            av_opt_set(videoEnc->priv_data, "preset", config_.egress.preset.c_str(), 0);
            av_opt_set(videoEnc->priv_data, "tune", config_.egress.tune.c_str(), 0);
            av_opt_set(videoEnc->priv_data, "nal-hrd", config_.egress.mux == "cbr" ? "cbr" : "none", 0);
        }
        else
        {
            av_opt_set(videoEnc->priv_data, "preset", "ultrafast", 0);
            av_opt_set(videoEnc->priv_data, "tune", "zerolatency", 0);
        }
            if (avcodec_open2(videoEnc, codec, nullptr) < 0)
            {
                avcodec_free_context(&videoEnc);
                if (nvenc && encoderPref_ == "auto")
                {
                    encoderPref_ = "cpu";
                    nvenc = false;
                    name = config_.egress.codec == "hevc" ? "libx265" : "libx264";
                    codec = avcodec_find_encoder_by_name(name.c_str());
                    if (codec == nullptr)
                    {
                        *error = "CPU encoder " + name + " is not available";
                        return false;
                    }
                    videoEnc = avcodec_alloc_context3(codec);
                    videoEnc->width = config_.target.width;
                    videoEnc->height = config_.target.height;
                    videoEnc->pix_fmt = AV_PIX_FMT_YUV420P;
                    videoEnc->time_base = AVRational{config_.target.rate.den, config_.target.rate.num};
                    videoEnc->framerate = AVRational{config_.target.rate.num, config_.target.rate.den};
                    videoEnc->bit_rate = config_.egress.bitrate;
                    videoEnc->rc_max_rate = config_.egress.bitrate;
                    videoEnc->rc_buffer_size = config_.egress.bitrate;
                    videoEnc->gop_size = std::max(1, static_cast<int>(std::llround(config_.target.rate.hz() * config_.egress.gopSeconds)));
                    videoEnc->max_b_frames = 0;
                    av_opt_set(videoEnc->priv_data, "preset", name == "libx264" ? config_.egress.preset.c_str() : "ultrafast", 0);
                    av_opt_set(videoEnc->priv_data, "tune", name == "libx264" ? config_.egress.tune.c_str() : "zerolatency", 0);
                    if (avcodec_open2(videoEnc, codec, nullptr) < 0)
                    {
                        avcodec_free_context(&videoEnc);
                        *error = "failed to open " + name;
                        return false;
                    }
                }
                else
                {
                    *error = "failed to open " + name;
                    if (config_.target.interlaced)
                    {
                        *error += " (interlaced encoding was refused by the encoder)";
                    }
                    return false;
                }
            }
        status_.encoder = name;
        for (auto const& track : config_.egress.audioTracks)
        {
            std::string audioName = track.codec == "aac" ? "aac" : track.codec == "mp2" ? "mp2" : track.codec == "ac3" ? "ac3" : track.codec == "opus" ? "libopus" : "s302m";
            AVCodec const* audioCodec = avcodec_find_encoder_by_name(audioName.c_str());
            if (audioCodec == nullptr && track.codec == "opus")
            {
                audioCodec = avcodec_find_encoder_by_name("opus");
                audioName = "opus";
            }
            if (audioCodec == nullptr)
            {
                audioEnc.push_back(nullptr);
                continue;
            }
            AVCodecContext* ctx = avcodec_alloc_context3(audioCodec);
            int const channels = static_cast<int>(track.channels.size());
            av_channel_layout_default(&ctx->ch_layout, channels);
            ctx->sample_rate = 48000;
            ctx->sample_fmt = audioCodec->sample_fmts != nullptr ? audioCodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
            ctx->bit_rate = track.bitrate > 0 ? track.bitrate : defaultAudioBitrate(track.codec, track.layout);
            ctx->time_base = AVRational{1, 48000};
            if (avcodec_open2(ctx, audioCodec, nullptr) < 0)
            {
                avcodec_free_context(&ctx);
                audioEnc.push_back(nullptr);
                continue;
            }
            audioEnc.push_back(ctx);
        }
        avformat_alloc_output_context2(&mux, nullptr, "mpegts", nullptr);
        auto* videoStream = avformat_new_stream(mux, nullptr);
        avcodec_parameters_from_context(videoStream->codecpar, videoEnc);
        videoStream->time_base = videoEnc->time_base;
        videoStream->id = config_.egress.videoPid;
        for (std::size_t i = 0; i < audioEnc.size(); ++i)
        {
            if (audioEnc[i] == nullptr)
            {
                continue;
            }
            auto* stream = avformat_new_stream(mux, nullptr);
            avcodec_parameters_from_context(stream->codecpar, audioEnc[i]);
            stream->time_base = audioEnc[i]->time_base;
            int const pid = config_.egress.audioTracks[i].pid > 0 ? config_.egress.audioTracks[i].pid : config_.egress.videoPid + 1 + static_cast<int>(i);
            stream->id = pid;
            av_dict_set(&stream->metadata, "language", config_.egress.audioTracks[i].language.c_str(), 0);
        }
        int const avioSize = 1316;
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(static_cast<std::size_t>(avioSize)));
        avio = avio_alloc_context(buffer, avioSize, 1, this, nullptr, writeEgress, nullptr);
        mux->pb = avio;
        mux->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FLUSH_PACKETS;
        av_dict_set(&mux->metadata, "service_name", config_.egress.serviceName.c_str(), 0);
        av_dict_set(&mux->metadata, "service_provider", config_.egress.provider.c_str(), 0);
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "mpegts_service_id", std::to_string(config_.egress.programNumber).c_str(), 0);
        av_dict_set(&opts, "pcr_period", std::to_string(config_.egress.pcrMs).c_str(), 0);
        av_dict_set(&opts, "mpegts_pmt_start_pid", std::to_string(config_.egress.pmtPid).c_str(), 0);
        av_dict_set(&opts, "mpegts_start_pid", std::to_string(config_.egress.videoPid).c_str(), 0);
        av_dict_set(&opts, "mpegts_flags", "resend_headers", 0);
        if (config_.egress.mux == "cbr")
        {
            int audioBits = 0;
            for (auto const& track : config_.egress.audioTracks)
            {
                audioBits += track.bitrate > 0 ? track.bitrate : defaultAudioBitrate(track.codec, track.layout);
            }
            int const muxrate = static_cast<int>((config_.egress.bitrate + audioBits) * 1.08);
            av_dict_set(&opts, "muxrate", std::to_string(muxrate).c_str(), 0);
        }
        if (avformat_write_header(mux, &opts) < 0)
        {
            *error = "MPEG-TS muxer failed to start";
            av_dict_free(&opts);
            return false;
        }
        av_dict_free(&opts);
        return true;
    };

    std::string encoderError;
    if (!ensureEncoder(&encoderError))
    {
        PipelineStatus failed;
        failed.state = "error";
        failed.error = encoderError;
        failed.targetFormat = config_.target.label();
        std::lock_guard const lock{statusMu_};
        status_ = failed;
        stop_.store(true);
        connector.join();
        closeMux();
        return;
    }

    // Replace the placeholder write callback by writing directly after each mux packet
    // through a small local hook stored on the pipeline. The AVIO callback above drops
    // bytes; the explicit send below is the real output. Both stay so the muxer clock
    // never blocks on a disconnected caller.
    std::uint64_t index = grainIndexNow(config_.target.rate);
    std::vector<std::vector<float>> audioFifo(config_.egress.audioTracks.size());
    while (!stop_.load())
    {
        std::uint64_t const nowIndex = grainIndexNow(config_.target.rate);
        if (nowIndex < index)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (nowIndex > index + 4)
        {
            index = nowIndex;
        }
        auto const videoRoute = routes_(true);
        auto const audioRoute = routes_(false);
        std::string state = "waiting";
        std::vector<std::uint8_t> picture = slateFrame;
        VideoFormat source = config_.target;
        bool live = false;
        if (videoRoute.active && !videoRoute.flowId.empty())
        {
            if (videoRoute.flowId != openVideo)
            {
                bool mirror = false;
                openDomain = domains_(videoRoute.domainId, &mirror);
                videoReader.close();
                audioReader.close();
                sync.reset();
                openVideo.clear();
                openAudio.clear();
                if (openDomain && videoReader.open(*openDomain, videoRoute.flowId))
                {
                    openVideo = videoRoute.flowId;
                    source = videoReader.format();
                    if (audioRoute.active && audioReader.open(*openDomain, audioRoute.flowId))
                    {
                        openAudio = audioRoute.flowId;
                        sync = std::make_unique<MxlSync>(*openDomain);
                        sync->addVideo(videoReader);
                        sync->addAudio(audioReader);
                    }
                }
            }
            if (videoReader.isOpen())
            {
                source = videoReader.format();
                int const offset = config_.egress.readOffsetGrains + (videoRoute.mirror ? 4 : 0);
                std::uint64_t const head = videoReader.head();
                if (head > static_cast<std::uint64_t>(offset))
                {
                    std::uint64_t const srcIndex = head - static_cast<std::uint64_t>(offset);
                    if (sync)
                    {
                        sync->waitFor(grainTimeNs(source.rate, srcIndex), 40000000);
                    }
                    bool invalid = false;
                    std::vector<std::uint8_t> payload;
                    if (videoReader.read(srcIndex, 40000000, payload, &invalid) && !invalid)
                    {
                        picture = std::move(payload);
                        live = true;
                        state = "running";
                        if (haveSource && srcIndex > lastSource + 1)
                        {
                            drops += srcIndex - lastSource - 1;
                        }
                        else if (haveSource && srcIndex == lastSource)
                        {
                            ++repeats;
                        }
                        lastSource = srcIndex;
                        haveSource = true;
                    }
                    else
                    {
                        state = "no_signal";
                    }
                }
                else
                {
                    state = "waiting";
                }
            }
        }
        if (!live)
        {
            state = videoRoute.active ? "no_signal" : "waiting";
        }
        if (picture.size() != v210Size(source.width, source.height) && picture.size() != v210Size(config_.target.width, config_.target.height))
        {
            picture = slateFrame;
            source = config_.target;
        }
        if (graph == nullptr || source.width != graphSource.width || source.height != graphSource.height || source.interlaced != graphSource.interlaced)
        {
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            auto plan = planAdaptation(source, config_.target, config_.deinterlacer, config_.aspect, config_.scale, "auto");
            std::string desc = ffmpegFilter(plan, false);
            // NVENC with CUDA input takes NV12 (uploaded below); the CPU encoders take yuv420p.
            bool const toNv12 = videoEnc != nullptr && videoEnc->hw_frames_ctx != nullptr;
            auto const marker = desc.rfind("format=yuv422p10le");
            if (marker != std::string::npos)
            {
                desc.replace(marker, std::string("format=yuv422p10le").size(), toNv12 ? "format=nv12" : "format=yuv420p");
            }
            char args[256];
            std::snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1", source.width, source.height, AV_PIX_FMT_YUV422P10LE,
                config_.target.rate.den, config_.target.rate.num);
            avfilter_graph_create_filter(&sourceFilter, avfilter_get_by_name("buffer"), "in", args, nullptr, graph);
            avfilter_graph_create_filter(&sinkFilter, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph);
            enum AVPixelFormat const pixfmts[] = {toNv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
            av_opt_set_int_list(sinkFilter, "pix_fmts", pixfmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
            AVFilterInOut* outputs = avfilter_inout_alloc();
            AVFilterInOut* inputs = avfilter_inout_alloc();
            outputs->name = av_strdup("in");
            outputs->filter_ctx = sourceFilter;
            outputs->pad_idx = 0;
            inputs->name = av_strdup("out");
            inputs->filter_ctx = sinkFilter;
            inputs->pad_idx = 0;
            if (avfilter_graph_parse_ptr(graph, desc.c_str(), &inputs, &outputs, nullptr) < 0 || avfilter_graph_config(graph, nullptr) < 0)
            {
                avfilter_graph_free(&graph);
            }
            graphSource = source;
        }
        bool const gpuInput = videoEnc != nullptr && videoEnc->hw_frames_ctx != nullptr;
        bool const sameRaster = source.width == config_.target.width && source.height == config_.target.height &&
                                source.interlaced == config_.target.interlaced && (!source.interlaced || source.fieldOrder == config_.target.fieldOrder);
        AVFrame* encoded = av_frame_alloc();
        AVFrame* raw = nullptr;
        bool haveFrame = false;
        if (gpuInput && sameRaster && !gpuConvertFailed)
        {
            // Nothing to adapt: v210 straight to an NV12 frame on the GPU.
            haveFrame = av_hwframe_get_buffer(videoEnc->hw_frames_ctx, encoded, 0) >= 0 &&
                        cudaconvert::toNv12(picture.data(), source.width, source.height, source.interlaced, encoded);
            if (!haveFrame)
            {
                // From now on convert on the CPU and upload, as for an adapted raster.
                log::error("gpu_nv12_failed", {{"channel", config_.id}});
                gpuConvertFailed = true;
                av_frame_unref(encoded);
            }
        }
        if (!haveFrame)
        {
            raw = unpackV210(picture.data(), static_cast<int>(picture.size()), source.width, source.height);
            if (raw == nullptr)
            {
                raw = av_frame_alloc();
                raw->format = AV_PIX_FMT_YUV422P10LE;
                raw->width = source.width;
                raw->height = source.height;
                av_frame_get_buffer(raw, 0);
            }
            raw->pts = videoPts;
            AVFrame* adapted = av_frame_alloc();
            if (graph != nullptr && av_buffersrc_add_frame(sourceFilter, raw) >= 0 && av_buffersink_get_frame(sinkFilter, adapted) >= 0)
            {
                if (gpuInput)
                {
                    haveFrame = av_hwframe_get_buffer(videoEnc->hw_frames_ctx, encoded, 0) >= 0 && av_hwframe_transfer_data(encoded, adapted, 0) >= 0;
                }
                else
                {
                    av_frame_move_ref(encoded, adapted);
                    haveFrame = true;
                }
            }
            av_frame_free(&adapted);
        }
        if (haveFrame)
        {
            encoded->pts = videoPts;
            if (needKeyframe && primary.connected())
            {
                encoded->pict_type = AV_PICTURE_TYPE_I;
                encoded->flags |= AV_FRAME_FLAG_KEY;
                needKeyframe = false;
            }
            auto const encodeStart = std::chrono::steady_clock::now();
            if (avcodec_send_frame(videoEnc, encoded) >= 0)
            {
                AVPacket* packet = av_packet_alloc();
                while (avcodec_receive_packet(videoEnc, packet) >= 0)
                {
                    av_packet_rescale_ts(packet, videoEnc->time_base, mux->streams[0]->time_base);
                    packet->stream_index = 0;
                    av_interleaved_write_frame(mux, packet);
                    if (mux->pb != nullptr)
                    {
                        avio_flush(mux->pb);
                    }
                    if (!primary.connected())
                    {
                        needKeyframe = true;
                    }
                    av_packet_unref(packet);
                }
                av_packet_free(&packet);
            }
            auto const encodeEnd = std::chrono::steady_clock::now();
            double const seconds = std::chrono::duration<double>(encodeEnd - encodeStart).count();
            std::lock_guard const lock{statusMu_};
            status_.encodeLatencySum += seconds;
            status_.encodeLatencyCount++;
            status_.latencyBuckets[static_cast<std::size_t>(latencyBucket(seconds))]++;
        }
        av_frame_free(&encoded);
        av_frame_free(&raw);

        if (audioReader.isOpen())
        {
            int const sampleCount = samplesPerGrain(static_cast<std::int64_t>(index), config_.target.rate.num, config_.target.rate.den);
            std::uint64_t const end = static_cast<std::uint64_t>(sampleIndexAtGrain(static_cast<std::int64_t>(index + 1), config_.target.rate.num, config_.target.rate.den));
            std::vector<float> interleaved;
            if (!audioReader.read(end, sampleCount, 20000000, interleaved))
            {
                interleaved.assign(static_cast<std::size_t>(sampleCount * std::max(1, audioReader.channels())), 0.f);
            }
            int const srcChannels = std::max(1, audioReader.channels());
            for (std::size_t t = 0; t < config_.egress.audioTracks.size() && t < audioEnc.size(); ++t)
            {
                if (audioEnc[t] == nullptr)
                {
                    continue;
                }
                auto const& track = config_.egress.audioTracks[t];
                int const channels = static_cast<int>(track.channels.size());
                float const gain = track.mute ? 0.f : static_cast<float>(dbToLinear(track.gainDb));
                for (int frame = 0; frame < sampleCount; ++frame)
                {
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        int const src = ch < static_cast<int>(track.channels.size()) ? track.channels[static_cast<std::size_t>(ch)] : 0;
                        float sample = 0;
                        if (src >= 0 && src < srcChannels)
                        {
                            sample = interleaved[static_cast<std::size_t>(frame * srcChannels + src)] * gain;
                        }
                        audioFifo[t].push_back(sample);
                    }
                }
                int const frameSize = audioEnc[t]->frame_size > 0 ? audioEnc[t]->frame_size : sampleCount;
                while (static_cast<int>(audioFifo[t].size()) >= frameSize * channels)
                {
                    AVFrame* aframe = av_frame_alloc();
                    aframe->nb_samples = frameSize;
                    aframe->format = audioEnc[t]->sample_fmt;
                    aframe->sample_rate = 48000;
                    av_channel_layout_copy(&aframe->ch_layout, &audioEnc[t]->ch_layout);
                    av_frame_get_buffer(aframe, 0);
                    if (audioEnc[t]->sample_fmt == AV_SAMPLE_FMT_FLTP)
                    {
                        for (int ch = 0; ch < channels; ++ch)
                        {
                            auto* dst = reinterpret_cast<float*>(aframe->data[ch]);
                            for (int frame = 0; frame < frameSize; ++frame)
                            {
                                dst[frame] = audioFifo[t][static_cast<std::size_t>(frame * channels + ch)];
                            }
                        }
                    }
                    else
                    {
                        std::memcpy(aframe->data[0], audioFifo[t].data(), static_cast<std::size_t>(frameSize * channels) * sizeof(float));
                    }
                    audioFifo[t].erase(audioFifo[t].begin(), audioFifo[t].begin() + static_cast<std::ptrdiff_t>(frameSize * channels));
                    aframe->pts = videoPts * sampleCount;
                    if (avcodec_send_frame(audioEnc[t], aframe) >= 0)
                    {
                        AVPacket* packet = av_packet_alloc();
                        while (avcodec_receive_packet(audioEnc[t], packet) >= 0)
                        {
                            packet->stream_index = static_cast<int>(t) + 1;
                            av_packet_rescale_ts(packet, audioEnc[t]->time_base, mux->streams[packet->stream_index]->time_base);
                            av_interleaved_write_frame(mux, packet);
                            if (mux->pb != nullptr)
                            {
                                avio_flush(mux->pb);
                            }
                            av_packet_unref(packet);
                        }
                        av_packet_free(&packet);
                    }
                    av_frame_free(&aframe);
                }
            }
        }
        ++frames;
        ++videoPts;
        ++index;
        PipelineStatus status;
        status.state = state;
        status.srt = primary.stats();
        status.peer = status.srt.peer;
        status.sourceFormat = source.label();
        status.targetFormat = config_.target.label();
        status.encoder = status_.encoder;
        status.frames = frames;
        status.repeats = repeats;
        status.drops = drops;
        status.encodeFps = frames / std::max(0.001, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        status.encodeLatencyCount = status_.encodeLatencyCount;
        status.encodeLatencySum = status_.encodeLatencySum;
        status.latencyBuckets = status_.latencyBuckets;
        status.failover = config_.backup.enabled && duplicate.connected();
        status.request = publicRequest(config_, config_.announceAddress.empty() ? primaryIpv4() : config_.announceAddress);
        if (monoNowMs() % 1000 < 40)
        {
            status.jpeg = encodeJpeg(picture.data(), source.width, source.height);
        }
        {
            std::lock_guard const lock{statusMu_};
            auto const previous = status_.jpeg;
            status_ = status;
            if (status_.jpeg.empty())
            {
                status_.jpeg = previous;
            }
        }
    }
    closeMux();
    connector.join();
}
} // namespace srtgw
