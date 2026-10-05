#pragma once

#include "media/format.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifndef SRTGW_WITH_MXL
#define SRTGW_WITH_MXL 0
#endif

#if SRTGW_WITH_MXL
#include <mxl/flow.h>
#include <mxl/mxl.h>
#endif

namespace srtgw
{
struct DomainRecord
{
    std::string path;
    std::string id;
    bool mirror = false;
};

std::vector<DomainRecord> scanDomains(std::string const& root);
std::string resolveDomainPath(std::string const& root, std::string const& id);
// Creates the directory and writes domain_def.json and options.json only when they are absent.
// An existing domain_def.json with a different id is left untouched and reported in error.
bool ensureOutputDomain(std::string const& path, std::string const& id, std::int64_t historyNs, std::string* error);
// Deletes path when its domain_def.json id matches. Refuses an empty path, "/", and a mismatched id.
bool removeOwnDomain(std::string const& path, std::string const& id);

class MxlDomain
{
public:
    MxlDomain(std::string path, std::string id, std::int64_t historyNs, bool create);
    ~MxlDomain();

    MxlDomain(MxlDomain const&) = delete;
    MxlDomain& operator=(MxlDomain const&) = delete;

    [[nodiscard]] std::string const& path() const;
    [[nodiscard]] std::string const& id() const;
    void garbageCollect();
#if SRTGW_WITH_MXL
    [[nodiscard]] mxlInstance instance() const;
#endif

private:
    std::string path_;
    std::string id_;
#if SRTGW_WITH_MXL
    mxlInstance instance_ = nullptr;
#endif
};

class MxlVideoWriter
{
public:
    MxlVideoWriter(MxlDomain& domain, std::string flowId, std::string label, std::string group, VideoFormat const& format);
    ~MxlVideoWriter();
    bool write(std::uint64_t index, std::uint8_t const* data, std::size_t size, bool invalid);
    // Fills the grain in place: `fill` gets the payload and its size.
    bool writeWith(std::uint64_t index, std::function<void(std::uint8_t*, std::size_t)> const& fill);

private:
#if SRTGW_WITH_MXL
    MxlDomain& domain_;
    mxlFlowWriter writer_ = nullptr;
#endif
};

class MxlAudioWriter
{
public:
    MxlAudioWriter(MxlDomain& domain, std::string flowId, std::string label, std::string group, int channels);
    ~MxlAudioWriter();
    bool write(std::uint64_t endIndex, float const* interleaved, int frames, int channels);

private:
#if SRTGW_WITH_MXL
    MxlDomain& domain_;
    mxlFlowWriter writer_ = nullptr;
    std::size_t maxWrite_ = 0;
#endif
};

class MxlVideoReader
{
public:
    bool open(MxlDomain& domain, std::string const& flowId);
    void close();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] std::uint64_t head() const;
    [[nodiscard]] VideoFormat format() const;
    bool read(std::uint64_t index, std::uint64_t timeoutNs, std::vector<std::uint8_t>& payload, bool* invalid);
    // The grain in place, without a copy: valid until the writer reuses its slot (the flow's
    // history later).
    bool view(std::uint64_t index, std::uint64_t timeoutNs, std::uint8_t const** data, std::size_t* size, bool* invalid);
#if SRTGW_WITH_MXL
    [[nodiscard]] mxlFlowReader handle() const;
#endif

private:
#if SRTGW_WITH_MXL
    MxlDomain* domain_ = nullptr;
    mxlFlowReader reader_ = nullptr;
#endif
    VideoFormat format_;
    bool open_ = false;
};

class MxlAudioReader
{
public:
    bool open(MxlDomain& domain, std::string const& flowId);
    void close();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] int channels() const;
    bool read(std::uint64_t endIndex, int frames, std::uint64_t timeoutNs, std::vector<float>& interleaved);
#if SRTGW_WITH_MXL
    [[nodiscard]] mxlFlowReader handle() const;
#endif

private:
#if SRTGW_WITH_MXL
    MxlDomain* domain_ = nullptr;
    mxlFlowReader reader_ = nullptr;
    std::size_t maxRead_ = 0;
#endif
    int channels_ = 0;
    bool open_ = false;
};

class MxlSync
{
public:
    explicit MxlSync(MxlDomain& domain);
    ~MxlSync();
    void addVideo(MxlVideoReader const& reader);
    void addAudio(MxlAudioReader const& reader);
    bool waitFor(std::uint64_t timestampNs, std::uint64_t timeoutNs);

private:
#if SRTGW_WITH_MXL
    MxlDomain& domain_;
    mxlFlowSynchronizationGroup group_ = nullptr;
#endif
};

std::uint64_t grainIndexNow(Rate const& rate);
std::uint64_t grainIndexAt(Rate const& rate, std::uint64_t taiNs);
std::uint64_t grainTimeNs(Rate const& rate, std::uint64_t index);
std::string videoFlowJson(std::string const& id, std::string const& label, std::string const& group, VideoFormat const& format);
std::string audioFlowJson(std::string const& id, std::string const& label, std::string const& group, int channels);
} // namespace srtgw
