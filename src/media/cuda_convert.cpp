#include "media/cuda_convert.hpp"

#if SRTGW_WITH_CUDA_CONVERT

#include "media/v210.hpp"
#include "media/v210_cuda_ptx.hpp"

// The loader defines CUDA_VERSION and the driver types, so hwcontext_cuda.h does
// not need the CUDA toolkit's cuda.h (same as inside FFmpeg).
#include <ffnvcodec/dynlink_loader.h>

extern "C"
{
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <dlfcn.h>

namespace srtgw::cudaconvert
{
namespace
{
CudaFunctions* driver()
{
    static CudaFunctions* const loaded = [] {
        CudaFunctions* functions = nullptr;
        return cuda_load_functions(&functions, nullptr) == 0 ? functions : nullptr;
    }();
    return loaded;
}

// Page-locked host memory: the ffnvcodec loader does not load these two.
struct HostMemory
{
    using Alloc = CUresult (*)(void**, std::size_t);
    using Free = CUresult (*)(void*);
    Alloc alloc = nullptr;
    Free free = nullptr;
};

HostMemory const& hostMemory()
{
    static HostMemory const loaded = [] {
        HostMemory h;
        if (void* lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL))
        {
            h.alloc = reinterpret_cast<HostMemory::Alloc>(dlsym(lib, "cuMemAllocHost_v2"));
            h.free = reinterpret_cast<HostMemory::Free>(dlsym(lib, "cuMemFreeHost"));
            if (h.alloc == nullptr || h.free == nullptr)
            {
                h = HostMemory{};
            }
        }
        return h;
    }();
    return loaded;
}

struct Kernels
{
    CUmodule module = nullptr;
    CUfunction nv12ToV210 = nullptr;
    CUfunction p010ToV210 = nullptr;
    CUfunction v210ToNv12 = nullptr;
};

// The kernels of a device's primary context (the caller has pushed `context`).
// Only primary contexts are used: one retain per device is kept for the life of
// the process, so a cached module can never belong to a destroyed context.
Kernels const* kernels(CUcontext context)
{
    static std::mutex mu;
    static std::map<CUcontext, Kernels> loaded;
    std::lock_guard const lock{mu};
    auto const found = loaded.find(context);
    if (found != loaded.end())
    {
        return found->second.module != nullptr ? &found->second : nullptr;
    }
    Kernels k;
    auto* cu = driver();
    CUdevice device = 0;
    CUcontext primary = nullptr;
    if (cu->cuCtxGetDevice(&device) != CUDA_SUCCESS || cu->cuDevicePrimaryCtxRetain(&primary, device) != CUDA_SUCCESS)
    {
        return nullptr;
    }
    if (primary != context)
    {
        // Not the primary context: no module, and no lasting retain.
        cu->cuDevicePrimaryCtxRelease(device);
        return nullptr;
    }
    // The whole array: the generated accessor leaves out the last byte.
    std::string const ptx(reinterpret_cast<char const*>(kv210CudaPtx), sizeof(kv210CudaPtx));
    if (cu->cuModuleLoadData(&k.module, ptx.c_str()) != CUDA_SUCCESS ||
        cu->cuModuleGetFunction(&k.nv12ToV210, k.module, "nv12ToV210") != CUDA_SUCCESS ||
        cu->cuModuleGetFunction(&k.p010ToV210, k.module, "p010ToV210") != CUDA_SUCCESS ||
        cu->cuModuleGetFunction(&k.v210ToNv12, k.module, "v210ToNv12") != CUDA_SUCCESS)
    {
        k = Kernels{};
    }
    auto const& slot = loaded[context] = k;
    return slot.module != nullptr ? &slot : nullptr;
}

class ContextScope
{
public:
    ContextScope(CudaFunctions* cu, CUcontext context)
        : cu_(cu)
        , ok_(cu != nullptr && cu->cuCtxPushCurrent(context) == CUDA_SUCCESS)
    {
    }
    ~ContextScope()
    {
        if (ok_)
        {
            CUcontext dummy = nullptr;
            cu_->cuCtxPopCurrent(&dummy);
        }
    }
    ContextScope(ContextScope const&) = delete;
    ContextScope& operator=(ContextScope const&) = delete;
    explicit operator bool() const
    {
        return ok_;
    }

private:
    CudaFunctions* cu_;
    bool ok_;
};

// The packed v210 picture on the device, per thread (one per pipeline) and context.
struct Scratch
{
    CUcontext context = nullptr;
    CUdeviceptr device = 0;
    std::size_t bytes = 0;
    // Page-locked staging for the copies: from or into pageable memory the driver
    // copies through its own staging at about half the link rate, and 16 ingest
    // channels of v210 (4.4 GB/s) no longer fitted a x4 link.
    void* host = nullptr;
    std::size_t hostBytes = 0;

    void* ensureHost(std::size_t need)
    {
        auto const& memory = hostMemory();
        if (memory.alloc == nullptr)
        {
            return nullptr;
        }
        if (host != nullptr && hostBytes >= need)
        {
            return host;
        }
        if (host != nullptr)
        {
            memory.free(host);
        }
        host = nullptr;
        hostBytes = 0;
        if (memory.alloc(&host, need) != CUDA_SUCCESS)
        {
            host = nullptr;
            return nullptr;
        }
        hostBytes = need;
        return host;
    }

    CUdeviceptr ensure(CudaFunctions* cu, CUcontext ctx, std::size_t need)
    {
        if (device != 0 && context == ctx && bytes >= need)
        {
            return device;
        }
        if (device != 0 && context == ctx)
        {
            cu->cuMemFree(device);
        }
        device = 0;
        bytes = 0;
        context = ctx;
        if (cu->cuMemAlloc(&device, need) != CUDA_SUCCESS)
        {
            device = 0;
            return 0;
        }
        bytes = need;
        return device;
    }
};

Scratch& scratch()
{
    thread_local Scratch value;
    return value;
}

AVCUDADeviceContext const* deviceOf(AVFrame const* frame, AVHWFramesContext const** frames)
{
    if (frame == nullptr || frame->format != AV_PIX_FMT_CUDA || frame->hw_frames_ctx == nullptr)
    {
        return nullptr;
    }
    *frames = reinterpret_cast<AVHWFramesContext const*>(frame->hw_frames_ctx->data);
    return static_cast<AVCUDADeviceContext const*>((*frames)->device_ctx->hwctx);
}

constexpr unsigned kBlock = 128;

unsigned groupBlocks(int width)
{
    unsigned const groups = static_cast<unsigned>((width + 5) / 6);
    return (groups + kBlock - 1) / kBlock;
}
} // namespace

bool available()
{
    return driver() != nullptr && sizeof(kv210CudaPtx) > 1;
}

bool toV210(AVFrame const* frame, bool interlaced, std::vector<std::uint8_t>& out)
{
    AVHWFramesContext const* frames = nullptr;
    auto const* device = deviceOf(frame, &frames);
    auto* cu = driver();
    if (device == nullptr || cu == nullptr || (frames->sw_format != AV_PIX_FMT_NV12 && frames->sw_format != AV_PIX_FMT_P010))
    {
        return false;
    }
    ContextScope const scope(cu, device->cuda_ctx);
    auto const* k = scope ? kernels(device->cuda_ctx) : nullptr;
    if (k == nullptr)
    {
        return false;
    }
    int width = frame->width;
    int height = frame->height;
    int rowBytes = static_cast<int>(v210Stride(width));
    std::size_t const bytes = static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(height);
    CUdeviceptr dst = scratch().ensure(cu, device->cuda_ctx, bytes);
    if (dst == 0)
    {
        return false;
    }
    CUdeviceptr luma = reinterpret_cast<CUdeviceptr>(frame->data[0]);
    CUdeviceptr chroma = reinterpret_cast<CUdeviceptr>(frame->data[1]);
    int lumaPitch = frame->linesize[0];
    int chromaPitch = frame->linesize[1];
    int fields = interlaced ? 1 : 0;
    void* args[] = {&luma, &lumaPitch, &chroma, &chromaPitch, &width, &height, &fields, &dst, &rowBytes};
    CUfunction const kernel = frames->sw_format == AV_PIX_FMT_P010 ? k->p010ToV210 : k->nv12ToV210;
    out.resize(bytes);
    // The decoder writes its frames on this stream.
    void* const host = scratch().ensureHost(bytes);
    if (cu->cuLaunchKernel(kernel, groupBlocks(width), static_cast<unsigned>(height), 1, kBlock, 1, 1, 0, device->stream, args, nullptr) != CUDA_SUCCESS ||
        cu->cuMemcpyDtoHAsync(host != nullptr ? host : out.data(), dst, bytes, device->stream) != CUDA_SUCCESS ||
        cu->cuStreamSynchronize(device->stream) != CUDA_SUCCESS)
    {
        return false;
    }
    if (host != nullptr)
    {
        std::memcpy(out.data(), host, bytes);
    }
    return true;
}

bool toNv12(std::uint8_t const* v210, int width, int height, bool interlaced, AVFrame* frame)
{
    AVHWFramesContext const* frames = nullptr;
    auto const* device = deviceOf(frame, &frames);
    auto* cu = driver();
    if (v210 == nullptr || device == nullptr || cu == nullptr || frames->sw_format != AV_PIX_FMT_NV12 || frame->width != width || frame->height != height)
    {
        return false;
    }
    ContextScope const scope(cu, device->cuda_ctx);
    auto const* k = scope ? kernels(device->cuda_ctx) : nullptr;
    if (k == nullptr)
    {
        return false;
    }
    int rowBytes = static_cast<int>(v210Stride(width));
    std::size_t const bytes = static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(height);
    CUdeviceptr src = scratch().ensure(cu, device->cuda_ctx, bytes);
    if (src == 0)
    {
        return false;
    }
    CUdeviceptr luma = reinterpret_cast<CUdeviceptr>(frame->data[0]);
    CUdeviceptr chroma = reinterpret_cast<CUdeviceptr>(frame->data[1]);
    int lumaPitch = frame->linesize[0];
    int chromaPitch = frame->linesize[1];
    int fields = interlaced ? 1 : 0;
    void* args[] = {&src, &rowBytes, &width, &height, &fields, &luma, &lumaPitch, &chroma, &chromaPitch};
    void* const host = scratch().ensureHost(bytes);
    if (host != nullptr)
    {
        std::memcpy(host, v210, bytes);
    }
    // The encoder reads the frame after this returns, so wait for the kernel here.
    return cu->cuMemcpyHtoDAsync(src, host != nullptr ? host : v210, bytes, device->stream) == CUDA_SUCCESS &&
           cu->cuLaunchKernel(k->v210ToNv12, groupBlocks(width), static_cast<unsigned>((height + 1) / 2), 1, kBlock, 1, 1, 0, device->stream, args, nullptr) ==
               CUDA_SUCCESS &&
           cu->cuStreamSynchronize(device->stream) == CUDA_SUCCESS;
}
} // namespace srtgw::cudaconvert

#else

namespace srtgw::cudaconvert
{
bool available()
{
    return false;
}

bool toV210(AVFrame const*, bool, std::vector<std::uint8_t>&)
{
    return false;
}

bool toNv12(std::uint8_t const*, int, int, bool, AVFrame*)
{
    return false;
}
} // namespace srtgw::cudaconvert
#endif
