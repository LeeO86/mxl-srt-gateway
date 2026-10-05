#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace srtgw
{
// Rows are padded to 128 bytes (48 pixels), as MXL and FFmpeg lay v210 out.
std::size_t v210Stride(int width);
std::size_t v210Size(int width, int height);
void v210PackLine(std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr, int width, std::uint8_t* dst);
void fillV210(std::uint8_t* dst, int width, int height, std::uint16_t y, std::uint16_t cb, std::uint16_t cr);
std::vector<std::uint8_t> renderBars(int width, int height);

// 8-bit 4:2:0 planes to v210 rows `rowBytes` apart (the padding is not written). The
// arithmetic of the CUDA kernel nv12ToV210: samples × 4, chroma blended 3:1 from the two
// nearest 4:2:0 rows (of the same field when interlaced), so CPU and GPU give the same picture.
// `parity` 0 or 1 converts only the frame rows of that parity (0: rows 0, 2, …, the top
// field), one after another: one MXL field grain.
void yuv420ToV210(std::uint8_t const* y, int yPitch, std::uint8_t const* cb, std::uint8_t const* cr, int cPitch, int width, int height, bool interlaced,
    std::uint8_t* dst, std::size_t rowBytes, int parity = -1);

// The rows of one parity of a v210 frame, one after another (an MXL field grain).
void copyV210Field(std::uint8_t const* frame, std::size_t rowBytes, int height, int parity, std::uint8_t* field);
// A v210 frame from its two fields: `evenRows` holds frame rows 0, 2, …
void interleaveV210Fields(std::uint8_t const* evenRows, std::uint8_t const* oddRows, std::size_t rowBytes, int height, std::uint8_t* frame);

// v210 to 8-bit 4:2:0 planes. The arithmetic of the CUDA kernel v210ToNv12: samples rounded
// to 8 bits, chroma the mean of two rows (of the same field when interlaced).
void v210ToYuv420(std::uint8_t const* src, std::size_t rowBytes, int width, int height, bool interlaced, std::uint8_t* y, int yPitch, std::uint8_t* cb,
    std::uint8_t* cr, int cPitch);

// Both conversions use SSSE3 where the CPU has it; false forces the portable code (tests).
void setV210Simd(bool enabled);
} // namespace srtgw
