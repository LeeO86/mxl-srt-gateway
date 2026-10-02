#include "mxl/domain.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "util/net.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#if SRTGW_WITH_MXL
#include <mxl/time.h>
#endif

namespace srtgw
{
namespace
{
std::string readFile(std::filesystem::path const& path)
{
    std::ifstream in(path);
    if (!in)
    {
        return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeFile(std::filesystem::path const& path, std::string const& text)
{
    std::ofstream out(path, std::ios::trunc);
    if (out)
    {
        out << text;
    }
}
} // namespace

std::vector<DomainRecord> scanDomains(std::string const& root)
{
    std::vector<DomainRecord> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
    {
        return found;
    }
    for (auto const& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (!entry.is_directory())
        {
            continue;
        }
        auto const text = readFile(entry.path() / "domain_def.json");
        if (text.empty())
        {
            continue;
        }
        std::string error;
        auto const json = json::parse(text, &error);
        auto const id = json::fieldString(json, "id", "");
        if (id.empty())
        {
            continue;
        }
        DomainRecord record;
        record.path = entry.path().string();
        record.id = id;
        auto const agent = json::field(json, "x-mxl-fabrics-agent");
        record.mirror = agent && agent->is<picojson::object>();
        found.push_back(std::move(record));
    }
    return found;
}

std::string resolveDomainPath(std::string const& root, std::string const& id)
{
    for (auto const& domain : scanDomains(root))
    {
        if (domain.id == id)
        {
            return domain.path;
        }
    }
    return {};
}

std::string videoFlowJson(std::string const& id, std::string const& label, std::string const& group, VideoFormat const& format)
{
    std::ostringstream out;
    out << "{\"id\":\"" << id << "\",\"format\":\"urn:x-nmos:format:video\",\"label\":\"" << log::jsonEscape(label)
        << "\",\"description\":\"" << log::jsonEscape(label) << "\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" << log::jsonEscape(group)
        << "\"]},\"parents\":[],\"media_type\":\"video/v210\",\"grain_rate\":{\"numerator\":" << format.rate.num << ",\"denominator\":" << format.rate.den
        << "},\"frame_width\":" << format.width << ",\"frame_height\":" << format.height << ",\"interlace_mode\":\""
        << (format.interlaced ? (format.fieldOrder == "bff" ? "interlaced_bff" : "interlaced_tff") : "progressive")
        << "\",\"colorspace\":\"BT709\",\"components\":["
        << "{\"name\":\"Y\",\"width\":" << format.width << ",\"height\":" << format.height << ",\"bit_depth\":10},"
        << "{\"name\":\"Cb\",\"width\":" << format.width / 2 << ",\"height\":" << format.height << ",\"bit_depth\":10},"
        << "{\"name\":\"Cr\",\"width\":" << format.width / 2 << ",\"height\":" << format.height << ",\"bit_depth\":10}]}";
    return out.str();
}

std::string audioFlowJson(std::string const& id, std::string const& label, std::string const& group, int channels)
{
    std::ostringstream out;
    out << "{\"id\":\"" << id << "\",\"format\":\"urn:x-nmos:format:audio\",\"label\":\"" << log::jsonEscape(label)
        << "\",\"description\":\"" << log::jsonEscape(label) << "\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" << log::jsonEscape(group)
        << "\"]},\"parents\":[],\"media_type\":\"audio/float32\",\"sample_rate\":{\"numerator\":48000,\"denominator\":1},\"channel_count\":" << channels
        << ",\"bit_depth\":32}";
    return out.str();
}

std::uint64_t grainIndexNow(Rate const& rate)
{
#if SRTGW_WITH_MXL
    mxlRational edit{static_cast<std::int64_t>(rate.num), static_cast<std::int64_t>(rate.den)};
    return mxlTimestampToIndex(&edit, mxlGetTime());
#else
    if (rate.num <= 0)
    {
        return 0;
    }
    return static_cast<std::uint64_t>((static_cast<__int128>(taiNowNs()) * rate.num) / (static_cast<__int128>(rate.den) * 1000000000));
#endif
}

std::uint64_t grainTimeNs(Rate const& rate, std::uint64_t index)
{
#if SRTGW_WITH_MXL
    mxlRational edit{static_cast<std::int64_t>(rate.num), static_cast<std::int64_t>(rate.den)};
    return mxlIndexToTimestamp(&edit, index);
#else
    if (rate.num <= 0)
    {
        return 0;
    }
    return static_cast<std::uint64_t>((static_cast<__int128>(index) * rate.den * 1000000000) / rate.num);
#endif
}

MxlDomain::MxlDomain(std::string path, std::string id, std::int64_t historyNs, bool create)
    : path_(std::move(path))
    , id_(std::move(id))
{
    std::error_code ec;
    if (create)
    {
        std::filesystem::create_directories(path_, ec);
        if (ec)
        {
            throw std::runtime_error("cannot create MXL domain " + path_);
        }
        auto const def = std::filesystem::path(path_) / "domain_def.json";
        if (!std::filesystem::exists(def))
        {
            writeFile(def, std::string("{\"id\":\"") + id_ + "\",\"label\":\"mxl-srt-gateway\"}\n");
        }
        auto const options = std::filesystem::path(path_) / "options.json";
        if (!std::filesystem::exists(options))
        {
            writeFile(options, std::string("{\"urn:x-mxl:option:history_duration/v1.0\":") + std::to_string(historyNs) + "}\n");
        }
    }
#if SRTGW_WITH_MXL
    bool tmpfs = false;
    if (mxlIsTmpFs(path_.c_str(), &tmpfs) == MXL_STATUS_OK && !tmpfs)
    {
        log::warn("mxl_domain_not_tmpfs", {{"path", path_}});
    }
    instance_ = mxlCreateInstance(path_.c_str(), nullptr);
    if (instance_ == nullptr)
    {
        throw std::runtime_error("mxlCreateInstance failed for " + path_);
    }
#else
    (void)historyNs;
#endif
    log::info("mxl_domain_open", {{"path", path_}, {"id", id_}});
}

MxlDomain::~MxlDomain()
{
#if SRTGW_WITH_MXL
    if (instance_ != nullptr)
    {
        mxlDestroyInstance(instance_);
    }
#endif
}

std::string const& MxlDomain::path() const
{
    return path_;
}

std::string const& MxlDomain::id() const
{
    return id_;
}

void MxlDomain::garbageCollect()
{
#if SRTGW_WITH_MXL
    if (instance_ != nullptr)
    {
        mxlGarbageCollectFlows(instance_);
    }
#endif
}

#if SRTGW_WITH_MXL
mxlInstance MxlDomain::instance() const
{
    return instance_;
}
#endif

MxlVideoWriter::MxlVideoWriter(MxlDomain& domain, std::string flowId, std::string label, std::string group, VideoFormat const& format)
#if SRTGW_WITH_MXL
    : domain_(domain)
#endif
{
#if SRTGW_WITH_MXL
    auto const def = videoFlowJson(flowId, label, group, format);
    bool created = false;
    mxlFlowConfigInfo info{};
    auto const status = mxlCreateFlowWriter(domain.instance(), def.c_str(), "{\"maxCommitBatchSizeHint\":1}", &writer_, &info, &created);
    if (status != MXL_STATUS_OK)
    {
        throw std::runtime_error("video flow writer failed for " + flowId);
    }
    log::info("mxl_video_writer", {{"flow", flowId}, {"created", created ? "true" : "false"}});
#else
    (void)domain;
    (void)flowId;
    (void)label;
    (void)group;
    (void)format;
#endif
}

MxlVideoWriter::~MxlVideoWriter()
{
#if SRTGW_WITH_MXL
    if (writer_ != nullptr)
    {
        mxlReleaseFlowWriter(domain_.instance(), writer_);
    }
#endif
}

bool MxlVideoWriter::write(std::uint64_t index, std::uint8_t const* data, std::size_t size, bool invalid)
{
#if SRTGW_WITH_MXL
    mxlGrainInfo info{};
    std::uint8_t* payload = nullptr;
    if (mxlFlowWriterOpenGrain(writer_, index, &info, &payload) != MXL_STATUS_OK || payload == nullptr)
    {
        return false;
    }
    if (!invalid && data != nullptr)
    {
        auto const n = size < info.grainSize ? size : static_cast<std::size_t>(info.grainSize);
        std::memcpy(payload, data, n);
        info.flags = 0;
        info.validSlices = info.totalSlices;
    }
    else
    {
        info.flags = MXL_GRAIN_FLAG_INVALID;
    }
    return mxlFlowWriterCommitGrain(writer_, &info) == MXL_STATUS_OK;
#else
    (void)index;
    (void)data;
    (void)size;
    (void)invalid;
    return true;
#endif
}

MxlAudioWriter::MxlAudioWriter(MxlDomain& domain, std::string flowId, std::string label, std::string group, int channels)
#if SRTGW_WITH_MXL
    : domain_(domain)
#endif
{
#if SRTGW_WITH_MXL
    auto const def = audioFlowJson(flowId, label, group, channels);
    bool created = false;
    mxlFlowConfigInfo info{};
    auto const status = mxlCreateFlowWriter(domain.instance(), def.c_str(), "{\"maxCommitBatchSizeHint\":2000}", &writer_, &info, &created);
    if (status != MXL_STATUS_OK)
    {
        throw std::runtime_error("audio flow writer failed for " + flowId);
    }
    if (mxlFlowWriterGetMaxWriteLengthSamples(writer_, &maxWrite_) != MXL_STATUS_OK || maxWrite_ == 0)
    {
        maxWrite_ = info.continuous.bufferLength / 2;
    }
#else
    (void)domain;
    (void)flowId;
    (void)label;
    (void)group;
    (void)channels;
#endif
}

MxlAudioWriter::~MxlAudioWriter()
{
#if SRTGW_WITH_MXL
    if (writer_ != nullptr)
    {
        mxlReleaseFlowWriter(domain_.instance(), writer_);
    }
#endif
}

bool MxlAudioWriter::write(std::uint64_t endIndex, float const* interleaved, int frames, int channels)
{
#if SRTGW_WITH_MXL
    if (writer_ == nullptr || frames <= 0 || channels <= 0 || maxWrite_ == 0)
    {
        return false;
    }
    int done = 0;
    while (done < frames)
    {
        int const count = std::min(frames - done, static_cast<int>(maxWrite_));
        std::uint64_t const head = endIndex - static_cast<std::uint64_t>(frames - done) + static_cast<std::uint64_t>(count);
        mxlMutableWrappedMultiBufferSlice slices{};
        if (mxlFlowWriterOpenSamples(writer_, head, static_cast<std::size_t>(count), &slices) != MXL_STATUS_OK)
        {
            return false;
        }
        for (int channel = 0; channel < channels && channel < static_cast<int>(slices.count); ++channel)
        {
            auto* base = static_cast<std::uint8_t*>(slices.base.fragments[0].pointer);
            if (base == nullptr)
            {
                continue;
            }
            auto* dst = reinterpret_cast<float*>(base + static_cast<std::size_t>(channel) * slices.stride);
            int const first = static_cast<int>(slices.base.fragments[0].size / sizeof(float));
            int const headCount = std::min(count, first);
            for (int frame = 0; frame < headCount; ++frame)
            {
                dst[frame] = interleaved[(done + frame) * channels + channel];
            }
            if (headCount < count && slices.base.fragments[1].pointer != nullptr)
            {
                auto* wrapped = reinterpret_cast<float*>(static_cast<std::uint8_t*>(slices.base.fragments[1].pointer) +
                                                         static_cast<std::size_t>(channel) * slices.stride);
                for (int frame = headCount; frame < count; ++frame)
                {
                    wrapped[frame - headCount] = interleaved[(done + frame) * channels + channel];
                }
            }
        }
        if (mxlFlowWriterCommitSamples(writer_) != MXL_STATUS_OK)
        {
            return false;
        }
        done += count;
    }
    return true;
#else
    (void)endIndex;
    (void)interleaved;
    (void)frames;
    (void)channels;
    return true;
#endif
}

bool MxlVideoReader::open(MxlDomain& domain, std::string const& flowId)
{
    close();
#if SRTGW_WITH_MXL
    domain_ = &domain;
    if (mxlCreateFlowReader(domain.instance(), flowId.c_str(), nullptr, &reader_) != MXL_STATUS_OK)
    {
        reader_ = nullptr;
        return false;
    }
    std::string def(4096, '\0');
    std::size_t size = def.size();
    if (mxlGetFlowDef(domain.instance(), flowId.c_str(), def.data(), &size) != MXL_STATUS_OK)
    {
        def.assign(size, '\0');
        mxlGetFlowDef(domain.instance(), flowId.c_str(), def.data(), &size);
    }
    std::string error;
    auto const json = json::parse(def.c_str(), &error);
    format_.width = json::fieldInt(json, "frame_width", 1920);
    format_.height = json::fieldInt(json, "frame_height", 1080);
    auto const mode = json::fieldString(json, "interlace_mode", "progressive");
    format_.interlaced = mode.find("interlaced") != std::string::npos;
    format_.fieldOrder = mode.find("bff") != std::string::npos ? "bff" : "tff";
    auto const rate = json::field(json, "grain_rate");
    if (rate)
    {
        format_.rate.num = json::fieldInt(*rate, "numerator", 50);
        format_.rate.den = json::fieldInt(*rate, "denominator", 1);
        for (auto const& known : knownRates())
        {
            if (known.num == format_.rate.num && known.den == format_.rate.den)
            {
                format_.rate.name = known.name;
            }
        }
    }
    open_ = true;
    return true;
#else
    (void)domain;
    (void)flowId;
    return false;
#endif
}

void MxlVideoReader::close()
{
#if SRTGW_WITH_MXL
    if (reader_ != nullptr && domain_ != nullptr)
    {
        mxlReleaseFlowReader(domain_->instance(), reader_);
    }
    reader_ = nullptr;
#endif
    open_ = false;
}

bool MxlVideoReader::isOpen() const
{
    return open_;
}

std::uint64_t MxlVideoReader::head() const
{
#if SRTGW_WITH_MXL
    mxlFlowRuntimeInfo info{};
    if (reader_ != nullptr && mxlFlowReaderGetRuntimeInfo(reader_, &info) == MXL_STATUS_OK)
    {
        return info.headIndex;
    }
#endif
    return 0;
}

VideoFormat MxlVideoReader::format() const
{
    return format_;
}

bool MxlVideoReader::read(std::uint64_t index, std::uint64_t timeoutNs, std::vector<std::uint8_t>& payload, bool* invalid)
{
#if SRTGW_WITH_MXL
    mxlGrainInfo info{};
    std::uint8_t* data = nullptr;
    if (mxlFlowReaderGetGrain(reader_, index, timeoutNs, &info, &data) != MXL_STATUS_OK || data == nullptr)
    {
        return false;
    }
    payload.assign(data, data + info.grainSize);
    if (invalid != nullptr)
    {
        *invalid = (info.flags & MXL_GRAIN_FLAG_INVALID) != 0 || info.validSlices < info.totalSlices;
    }
    return true;
#else
    (void)index;
    (void)timeoutNs;
    (void)payload;
    if (invalid != nullptr)
    {
        *invalid = true;
    }
    return false;
#endif
}

#if SRTGW_WITH_MXL
mxlFlowReader MxlVideoReader::handle() const
{
    return reader_;
}
#endif

bool MxlAudioReader::open(MxlDomain& domain, std::string const& flowId)
{
    close();
#if SRTGW_WITH_MXL
    domain_ = &domain;
    if (mxlCreateFlowReader(domain.instance(), flowId.c_str(), nullptr, &reader_) != MXL_STATUS_OK)
    {
        reader_ = nullptr;
        return false;
    }
    mxlFlowConfigInfo info{};
    if (mxlFlowReaderGetConfigInfo(reader_, &info) == MXL_STATUS_OK)
    {
        channels_ = static_cast<int>(info.continuous.channelCount);
    }
    if (mxlFlowReaderGetMaxReadLengthSamples(reader_, &maxRead_) != MXL_STATUS_OK)
    {
        maxRead_ = 0;
    }
    open_ = true;
    return true;
#else
    (void)domain;
    (void)flowId;
    return false;
#endif
}

void MxlAudioReader::close()
{
#if SRTGW_WITH_MXL
    if (reader_ != nullptr && domain_ != nullptr)
    {
        mxlReleaseFlowReader(domain_->instance(), reader_);
    }
    reader_ = nullptr;
#endif
    open_ = false;
    channels_ = 0;
}

bool MxlAudioReader::isOpen() const
{
    return open_;
}

int MxlAudioReader::channels() const
{
    return channels_;
}

bool MxlAudioReader::read(std::uint64_t endIndex, int frames, std::uint64_t timeoutNs, std::vector<float>& interleaved)
{
#if SRTGW_WITH_MXL
    if (!open_ || frames <= 0 || channels_ <= 0)
    {
        return false;
    }
    interleaved.assign(static_cast<std::size_t>(frames * channels_), 0.f);
    int done = 0;
    std::size_t const limit = maxRead_ == 0 ? static_cast<std::size_t>(frames) : maxRead_;
    while (done < frames)
    {
        int const count = std::min(frames - done, static_cast<int>(limit));
        std::uint64_t const head = endIndex - static_cast<std::uint64_t>(frames - done) + static_cast<std::uint64_t>(count);
        mxlWrappedMultiBufferSlice slices{};
        if (mxlFlowReaderGetSamples(reader_, head, static_cast<std::size_t>(count), timeoutNs, &slices) != MXL_STATUS_OK)
        {
            return false;
        }
        for (int channel = 0; channel < channels_ && channel < static_cast<int>(slices.count); ++channel)
        {
            auto const* base = static_cast<std::uint8_t const*>(slices.base.fragments[0].pointer);
            if (base == nullptr)
            {
                continue;
            }
            auto const* src = reinterpret_cast<float const*>(base + static_cast<std::size_t>(channel) * slices.stride);
            int const first = static_cast<int>(slices.base.fragments[0].size / sizeof(float));
            for (int frame = 0; frame < count && frame < first; ++frame)
            {
                interleaved[static_cast<std::size_t>((done + frame) * channels_ + channel)] = src[frame];
            }
        }
        done += count;
    }
    return true;
#else
    (void)endIndex;
    (void)frames;
    (void)timeoutNs;
    (void)interleaved;
    return false;
#endif
}

#if SRTGW_WITH_MXL
mxlFlowReader MxlAudioReader::handle() const
{
    return reader_;
}
#endif

MxlSync::MxlSync(MxlDomain& domain)
#if SRTGW_WITH_MXL
    : domain_(domain)
#endif
{
#if SRTGW_WITH_MXL
    if (mxlCreateFlowSynchronizationGroup(domain.instance(), &group_) != MXL_STATUS_OK)
    {
        group_ = nullptr;
    }
#else
    (void)domain;
#endif
}

MxlSync::~MxlSync()
{
#if SRTGW_WITH_MXL
    if (group_ != nullptr)
    {
        mxlReleaseFlowSynchronizationGroup(domain_.instance(), group_);
    }
#endif
}

void MxlSync::addVideo(MxlVideoReader const& reader)
{
#if SRTGW_WITH_MXL
    if (group_ != nullptr && reader.handle() != nullptr)
    {
        mxlFlowSynchronizationGroupAddReader(group_, reader.handle());
    }
#else
    (void)reader;
#endif
}

void MxlSync::addAudio(MxlAudioReader const& reader)
{
#if SRTGW_WITH_MXL
    if (group_ != nullptr && reader.handle() != nullptr)
    {
        mxlFlowSynchronizationGroupAddReader(group_, reader.handle());
    }
#else
    (void)reader;
#endif
}

bool MxlSync::waitFor(std::uint64_t timestampNs, std::uint64_t timeoutNs)
{
#if SRTGW_WITH_MXL
    if (group_ == nullptr)
    {
        return false;
    }
    return mxlFlowSynchronizationGroupWaitForDataAt(group_, timestampNs, timeoutNs) == MXL_STATUS_OK;
#else
    (void)timestampNs;
    (void)timeoutNs;
    return false;
#endif
}
} // namespace srtgw
