#pragma once

extern "C"
{
#include <libavutil/frame.h>
}

#include <cstdint>
#include <vector>

// v210 <-> NV12/P010 on the GPU for the NVDEC and NVENC paths. The kernels are
// PTX built into the binary (src/media/v210_cuda.cu) and run in the decoder's
// or encoder's own CUDA context through the driver API, which the NVIDIA
// container toolkit provides. Without driver or kernels, available() is false
// and the callers keep the CPU path.
namespace srtgw::cudaconvert
{
bool available();

// A decoded CUDA frame (NV12 or P010) to packed v210 in host memory.
bool toV210(AVFrame const* frame, bool interlaced, std::vector<std::uint8_t>& out);

// Packed v210 in host memory into `frame`, a CUDA NV12 frame from an
// AVHWFramesContext (for NVENC). Returns when the frame is written.
bool toNv12(std::uint8_t const* v210, int width, int height, bool interlaced, AVFrame* frame);
} // namespace srtgw::cudaconvert
