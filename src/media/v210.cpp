#include "media/v210.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

#if defined(__x86_64__)
#include <immintrin.h>
#define SRTGW_X86 1
#else
#define SRTGW_X86 0
#endif

namespace srtgw
{
std::size_t v210Stride(int width)
{
    int const blocks = (width + 47) / 48;
    return static_cast<std::size_t>(blocks) * 128U;
}

std::size_t v210Size(int width, int height)
{
    return v210Stride(width) * static_cast<std::size_t>(height < 0 ? 0 : height);
}

namespace
{
void pack6(std::uint32_t* dst, std::uint16_t u0, std::uint16_t y0, std::uint16_t v0, std::uint16_t y1, std::uint16_t u2, std::uint16_t y2,
    std::uint16_t v2, std::uint16_t y3, std::uint16_t u4, std::uint16_t y4, std::uint16_t v4, std::uint16_t y5)
{
    auto const m = [](std::uint16_t v) { return static_cast<std::uint32_t>(v & 0x3ff); };
    dst[0] = m(u0) | (m(y0) << 10) | (m(v0) << 20);
    dst[1] = m(y1) | (m(u2) << 10) | (m(y2) << 20);
    dst[2] = m(v2) | (m(y3) << 10) | (m(u4) << 20);
    dst[3] = m(y4) | (m(v4) << 10) | (m(y5) << 20);
}
} // namespace

void v210PackLine(std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr, int width, std::uint8_t* dst)
{
    int const groups = (width + 5) / 6;
    for (int g = 0; g < groups; ++g)
    {
        auto at = [&](std::uint16_t const* plane, int index, std::uint16_t neutral) {
            if (index < 0 || index >= width)
            {
                return neutral;
            }
            return plane[index];
        };
        int const x = g * 6;
        std::uint32_t words[4];
        pack6(words, at(cb, x, 512), at(y, x, 64), at(cr, x, 512), at(y, x + 1, 64), at(cb, x + 2, 512), at(y, x + 2, 64), at(cr, x + 2, 512),
            at(y, x + 3, 64), at(cb, x + 4, 512), at(y, x + 4, 64), at(cr, x + 4, 512), at(y, x + 5, 64));
        std::memcpy(dst + static_cast<std::size_t>(g) * 16U, words, sizeof(words));
    }
}

void fillV210(std::uint8_t* dst, int width, int height, std::uint16_t y, std::uint16_t cb, std::uint16_t cr)
{
    std::vector<std::uint16_t> yLine(static_cast<std::size_t>(width), y);
    std::vector<std::uint16_t> cLine(static_cast<std::size_t>(width), cb);
    std::vector<std::uint16_t> rLine(static_cast<std::size_t>(width), cr);
    auto const stride = v210Stride(width);
    for (int row = 0; row < height; ++row)
    {
        v210PackLine(yLine.data(), cLine.data(), rLine.data(), width, dst + stride * static_cast<std::size_t>(row));
    }
}

std::vector<std::uint8_t> renderBars(int width, int height)
{
    std::vector<std::uint8_t> frame(v210Size(width, height));
    struct Bar
    {
        std::uint16_t y;
        std::uint16_t cb;
        std::uint16_t cr;
    };
    Bar const bars[8] = {
        {940, 512, 512},
        {877, 128, 555},
        {754, 384, 64},
        {690, 64, 167},
        {313, 960, 856},
        {250, 640, 960},
        {127, 896, 469},
        {64, 512, 512},
    };
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width));
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(width));
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(width));
    int const barWidth = std::max(1, width / 8);
    for (int x = 0; x < width; ++x)
    {
        auto const& bar = bars[std::min(7, x / barWidth)];
        y[static_cast<std::size_t>(x)] = bar.y;
        cb[static_cast<std::size_t>(x)] = bar.cb;
        cr[static_cast<std::size_t>(x)] = bar.cr;
    }
    auto const stride = v210Stride(width);
    for (int row = 0; row < height; ++row)
    {
        v210PackLine(y.data(), cb.data(), cr.data(), width, frame.data() + stride * static_cast<std::size_t>(row));
    }
    return frame;
}


namespace
{
std::atomic<bool> gSimd{true};

bool useSsse3()
{
#if SRTGW_X86
    static bool const supported = __builtin_cpu_supports("ssse3") != 0;
    return supported && gSimd.load(std::memory_order_relaxed);
#else
    return false;
#endif
}

int clampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// The 4:2:0 chroma rows for luma row `row` (chromaRows in v210_cuda.cu).
void chromaRows(int row, int chromaHeight, bool interlaced, int& near, int& far)
{
    if (interlaced)
    {
        int const field = row & 1;
        int const fieldRow = row >> 1;
        int const fieldChroma = (chromaHeight + 1 - field) / 2;
        int const c = fieldRow >> 1;
        int const other = (fieldRow & 1) != 0 ? c + 1 : c - 1;
        near = std::min(clampInt(c, 0, fieldChroma - 1) * 2 + field, chromaHeight - 1);
        far = std::min(clampInt(other, 0, fieldChroma - 1) * 2 + field, chromaHeight - 1);
        return;
    }
    int const c = row >> 1;
    near = c;
    far = clampInt((row & 1) != 0 ? c + 1 : c - 1, 0, chromaHeight - 1);
}

// 10-bit to 8-bit with rounding; 1022 and 1023 would round to 256.
std::uint8_t to8(std::uint32_t v)
{
    std::uint32_t const r = (v + 2) >> 2;
    return static_cast<std::uint8_t>(r > 255 ? 255 : r);
}

// One v210 group from 6 luma and 3 + 3 chroma 8-bit samples (each × 4).
void packGroup(std::uint8_t const* ys, std::uint8_t const* b, std::uint8_t const* r, std::uint8_t* out)
{
    auto const s = [](std::uint8_t v) { return static_cast<std::uint32_t>(v) << 2; };
    std::uint32_t const words[4] = {s(b[0]) | (s(ys[0]) << 10) | (s(r[0]) << 20), s(ys[1]) | (s(b[1]) << 10) | (s(ys[2]) << 20),
        s(r[1]) | (s(ys[3]) << 10) | (s(b[2]) << 20), s(ys[4]) | (s(r[2]) << 10) | (s(ys[5]) << 20)};
    std::memcpy(out, words, sizeof(words));
}

std::array<std::uint32_t, 4> groupWords(std::uint8_t const* group)
{
    std::array<std::uint32_t, 4> w{};
    std::memcpy(w.data(), group, sizeof(std::uint32_t) * 4);
    return w;
}

// Chroma row `chromaRow` (from v210 rows a and b) and luma rows 2·chromaRow, +1 of groups
// [first, end), one group at a time.
void toYuv420Groups(std::uint8_t const* src, std::size_t rowBytes, int width, int height, int chromaRow, int rowA, int rowB, int first, int end,
    std::uint8_t* y, int yPitch, std::uint8_t* cb, std::uint8_t* cr, int cPitch)
{
    int const chromaWidth = (width + 1) / 2;
    std::uint8_t* b = cb + static_cast<std::ptrdiff_t>(chromaRow) * cPitch;
    std::uint8_t* r = cr + static_cast<std::ptrdiff_t>(chromaRow) * cPitch;
    for (int g = first; g < end; ++g)
    {
        auto const a = groupWords(src + rowBytes * static_cast<std::size_t>(rowA) + static_cast<std::size_t>(g) * 16U);
        auto const o = groupWords(src + rowBytes * static_cast<std::size_t>(rowB) + static_cast<std::size_t>(g) * 16U);
        std::uint32_t const cbs[3] = {((a[0] & 0x3ffU) + (o[0] & 0x3ffU) + 1U) >> 1, (((a[1] >> 10) & 0x3ffU) + ((o[1] >> 10) & 0x3ffU) + 1U) >> 1,
            (((a[2] >> 20) & 0x3ffU) + ((o[2] >> 20) & 0x3ffU) + 1U) >> 1};
        std::uint32_t const crs[3] = {(((a[0] >> 20) & 0x3ffU) + ((o[0] >> 20) & 0x3ffU) + 1U) >> 1, ((a[2] & 0x3ffU) + (o[2] & 0x3ffU) + 1U) >> 1,
            (((a[3] >> 10) & 0x3ffU) + ((o[3] >> 10) & 0x3ffU) + 1U) >> 1};
        for (int i = 0; i < 3 && g * 3 + i < chromaWidth; ++i)
        {
            b[g * 3 + i] = to8(cbs[i]);
            r[g * 3 + i] = to8(crs[i]);
        }
        for (int row = 2 * chromaRow; row < std::min(2 * chromaRow + 2, height); ++row)
        {
            auto const w = groupWords(src + rowBytes * static_cast<std::size_t>(row) + static_cast<std::size_t>(g) * 16U);
            std::uint32_t const ys[6] = {(w[0] >> 10) & 0x3ffU, w[1] & 0x3ffU, (w[1] >> 20) & 0x3ffU, (w[2] >> 10) & 0x3ffU, w[3] & 0x3ffU, (w[3] >> 20) & 0x3ffU};
            std::uint8_t* l = y + static_cast<std::ptrdiff_t>(row) * yPitch;
            for (int i = 0; i < 6 && g * 6 + i < width; ++i)
            {
                l[g * 6 + i] = to8(ys[i]);
            }
        }
    }
}

#if SRTGW_X86
// SSSE3, one group per register. Groups [0, end) must be whole and leave room for the 8-byte
// luma and 4-byte chroma stores (the bytes past a group's samples are rewritten by the next).

// Bits 0–9, 10–19 and 20–29 of each word, rounded to 8 bits (to8), as bytes 0, 1, 2 of the word.
__attribute__((target("ssse3"))) __m128i fieldBytes(__m128i a, __m128i b, __m128i c)
{
    __m128i const two = _mm_set1_epi32(2);
    __m128i const max = _mm_set1_epi32(255);
    a = _mm_min_epi16(_mm_srli_epi32(_mm_add_epi32(a, two), 2), max);
    b = _mm_min_epi16(_mm_srli_epi32(_mm_add_epi32(b, two), 2), max);
    c = _mm_min_epi16(_mm_srli_epi32(_mm_add_epi32(c, two), 2), max);
    return _mm_or_si128(a, _mm_or_si128(_mm_slli_epi32(b, 8), _mm_slli_epi32(c, 16)));
}

__attribute__((target("ssse3"))) void fields(__m128i w, __m128i& f0, __m128i& f1, __m128i& f2)
{
    __m128i const mask = _mm_set1_epi32(0x3ff);
    f0 = _mm_and_si128(w, mask);
    f1 = _mm_and_si128(_mm_srli_epi32(w, 10), mask);
    f2 = _mm_and_si128(_mm_srli_epi32(w, 20), mask);
}

// The six luma samples of group g of a row, as 8 bytes (the last two belong to the next group).
__attribute__((target("ssse3"))) void lumaSsse3(std::uint8_t const* row, int g, std::uint8_t* out)
{
    // Word i's fields at bytes 4i (bits 0–9), 4i+1 (10–19), 4i+2 (20–29).
    __m128i const lumaOrder = _mm_setr_epi8(1, 4, 6, 9, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    __m128i f0;
    __m128i f1;
    __m128i f2;
    fields(_mm_loadu_si128(reinterpret_cast<__m128i const*>(row + static_cast<std::size_t>(g) * 16U)), f0, f1, f2);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(out + g * 6), _mm_shuffle_epi8(fieldBytes(f0, f1, f2), lumaOrder));
}

__attribute__((target("ssse3"))) void toYuv420Ssse3(std::uint8_t const* a, std::uint8_t const* o, std::uint8_t const* l0, std::uint8_t const* l1, int end,
    std::uint8_t* b, std::uint8_t* r, std::uint8_t* y0, std::uint8_t* y1)
{
    __m128i const one = _mm_set1_epi32(1);
    __m128i const chromaOrder = _mm_setr_epi8(0, 5, 10, -1, 2, 8, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    for (int g = 0; g < end; ++g)
    {
        __m128i a0;
        __m128i a1;
        __m128i a2;
        __m128i o0;
        __m128i o1;
        __m128i o2;
        fields(_mm_loadu_si128(reinterpret_cast<__m128i const*>(a + static_cast<std::size_t>(g) * 16U)), a0, a1, a2);
        fields(_mm_loadu_si128(reinterpret_cast<__m128i const*>(o + static_cast<std::size_t>(g) * 16U)), o0, o1, o2);
        __m128i const mean0 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(a0, o0), one), 1);
        __m128i const mean1 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(a1, o1), one), 1);
        __m128i const mean2 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(a2, o2), one), 1);
        __m128i const chroma = _mm_shuffle_epi8(fieldBytes(mean0, mean1, mean2), chromaOrder);
        std::uint32_t const cbBytes = static_cast<std::uint32_t>(_mm_cvtsi128_si32(chroma));
        std::uint32_t const crBytes = static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_srli_si128(chroma, 4)));
        std::memcpy(b + g * 3, &cbBytes, 4);
        std::memcpy(r + g * 3, &crBytes, 4);
        lumaSsse3(l0, g, y0);
        if (l1 != nullptr)
        {
            lumaSsse3(l1, g, y1);
        }
    }
}

__attribute__((target("ssse3"))) void toV210Ssse3(std::uint8_t const* luma, std::uint8_t const* b, std::uint8_t const* r, int end, std::uint8_t* out)
{
    // Source bytes: Y0–Y5 at 0–5, Cb0–Cb2 at 8–10, Cr0–Cr2 at 12–14. Each word's three fields.
    __m128i const field0 = _mm_setr_epi8(8, -1, -1, -1, 1, -1, -1, -1, 13, -1, -1, -1, 4, -1, -1, -1);
    __m128i const field1 = _mm_setr_epi8(0, -1, -1, -1, 9, -1, -1, -1, 3, -1, -1, -1, 14, -1, -1, -1);
    __m128i const field2 = _mm_setr_epi8(12, -1, -1, -1, 2, -1, -1, -1, 10, -1, -1, -1, 5, -1, -1, -1);
    for (int g = 0; g < end; ++g)
    {
        std::uint32_t cbBytes = 0;
        std::uint32_t crBytes = 0;
        std::memcpy(&cbBytes, b + g * 3, 4);
        std::memcpy(&crBytes, r + g * 3, 4);
        __m128i const chroma = _mm_unpacklo_epi32(_mm_cvtsi32_si128(static_cast<int>(cbBytes)), _mm_cvtsi32_si128(static_cast<int>(crBytes)));
        __m128i const samples = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<__m128i const*>(luma + g * 6)), chroma);
        __m128i const words = _mm_or_si128(_mm_slli_epi32(_mm_shuffle_epi8(samples, field0), 2),
            _mm_or_si128(_mm_slli_epi32(_mm_shuffle_epi8(samples, field1), 12), _mm_slli_epi32(_mm_shuffle_epi8(samples, field2), 22)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + static_cast<std::size_t>(g) * 16U), words);
    }
}
#endif
} // namespace

void setV210Simd(bool enabled)
{
    gSimd.store(enabled);
}

void yuv420ToV210(std::uint8_t const* y, int yPitch, std::uint8_t const* cb, std::uint8_t const* cr, int cPitch, int width, int height, bool interlaced,
    std::uint8_t* dst, std::size_t rowBytes, int parity)
{
    int const groups = (width + 5) / 6;
    int const full = width / 6;
    int const chromaWidth = (width + 1) / 2;
    int const chromaHeight = (height + 1) / 2;
    // Whole groups whose 8-byte luma load stays inside the row.
    int const simdEnd = useSsse3() && width >= 8 ? std::min(full, (width - 8) / 6 + 1) : 0;
    // One row of blended 8-bit chroma per luma row, padded to whole groups with the last
    // sample (and one byte for the 4-byte loads).
    std::vector<std::uint8_t> cbRow(static_cast<std::size_t>(groups) * 3U + 1U);
    std::vector<std::uint8_t> crRow(cbRow.size());
    for (int row = parity < 0 ? 0 : parity; row < height; row += parity < 0 ? 1 : 2)
    {
        int near = 0;
        int far = 0;
        chromaRows(row, chromaHeight, interlaced, near, far);
        std::uint8_t const* bn = cb + static_cast<std::ptrdiff_t>(near) * cPitch;
        std::uint8_t const* bf = cb + static_cast<std::ptrdiff_t>(far) * cPitch;
        std::uint8_t const* rn = cr + static_cast<std::ptrdiff_t>(near) * cPitch;
        std::uint8_t const* rf = cr + static_cast<std::ptrdiff_t>(far) * cPitch;
        for (int c = 0; c < chromaWidth; ++c)
        {
            cbRow[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>((3U * bn[c] + bf[c] + 2U) >> 2);
            crRow[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>((3U * rn[c] + rf[c] + 2U) >> 2);
        }
        std::fill(cbRow.begin() + chromaWidth, cbRow.end(), cbRow[static_cast<std::size_t>(chromaWidth - 1)]);
        std::fill(crRow.begin() + chromaWidth, crRow.end(), crRow[static_cast<std::size_t>(chromaWidth - 1)]);
        std::uint8_t const* luma = y + static_cast<std::ptrdiff_t>(row) * yPitch;
        std::uint8_t* out = dst + rowBytes * static_cast<std::size_t>(parity < 0 ? row : row / 2);
#if SRTGW_X86
        toV210Ssse3(luma, cbRow.data(), crRow.data(), simdEnd, out);
#endif
        for (int g = simdEnd; g < groups; ++g)
        {
            std::uint8_t ys[6];
            for (int i = 0; i < 6; ++i)
            {
                ys[i] = luma[std::min(g * 6 + i, width - 1)];
            }
            packGroup(ys, cbRow.data() + g * 3, crRow.data() + g * 3, out + static_cast<std::size_t>(g) * 16U);
        }
    }
}

void copyV210Field(std::uint8_t const* frame, std::size_t rowBytes, int height, int parity, std::uint8_t* field)
{
    for (int row = parity; row < height; row += 2)
    {
        std::memcpy(field + rowBytes * static_cast<std::size_t>(row / 2), frame + rowBytes * static_cast<std::size_t>(row), rowBytes);
    }
}

void interleaveV210Fields(std::uint8_t const* evenRows, std::uint8_t const* oddRows, std::size_t rowBytes, int height, std::uint8_t* frame)
{
    for (int row = 0; row < height; ++row)
    {
        std::uint8_t const* field = (row & 1) == 0 ? evenRows : oddRows;
        std::memcpy(frame + rowBytes * static_cast<std::size_t>(row), field + rowBytes * static_cast<std::size_t>(row / 2), rowBytes);
    }
}

void v210ToYuv420(std::uint8_t const* src, std::size_t rowBytes, int width, int height, bool interlaced, std::uint8_t* y, int yPitch, std::uint8_t* cb,
    std::uint8_t* cr, int cPitch)
{
    int const groups = (width + 5) / 6;
    int const chromaWidth = (width + 1) / 2;
    int const chromaHeight = (height + 1) / 2;
    // Whole groups whose 8-byte luma and 4-byte chroma stores stay inside the row.
    int const simdEnd = useSsse3() && width >= 8 ? std::min({width / 6, (width - 8) / 6 + 1, (chromaWidth - 4) / 3 + 1}) : 0;
    for (int chromaRow = 0; chromaRow < chromaHeight; ++chromaRow)
    {
        // Progressive: rows 2r and 2r+1. Interlaced: chroma row r belongs to field r & 1 and
        // averages two rows of that field.
        int rowA = 2 * chromaRow;
        int rowB = rowA + 1;
        if (interlaced)
        {
            rowA = (chromaRow >> 1) * 4 + (chromaRow & 1);
            rowB = rowA + 2;
        }
        rowA = clampInt(rowA, 0, height - 1);
        rowB = clampInt(rowB, 0, height - 1);
#if SRTGW_X86
        if (simdEnd > 0)
        {
            int const l1 = 2 * chromaRow + 1;
            toYuv420Ssse3(src + rowBytes * static_cast<std::size_t>(rowA), src + rowBytes * static_cast<std::size_t>(rowB),
                src + rowBytes * static_cast<std::size_t>(2 * chromaRow), l1 < height ? src + rowBytes * static_cast<std::size_t>(l1) : nullptr, simdEnd,
                cb + static_cast<std::ptrdiff_t>(chromaRow) * cPitch, cr + static_cast<std::ptrdiff_t>(chromaRow) * cPitch,
                y + static_cast<std::ptrdiff_t>(2 * chromaRow) * yPitch, l1 < height ? y + static_cast<std::ptrdiff_t>(l1) * yPitch : nullptr);
        }
#endif
        toYuv420Groups(src, rowBytes, width, height, chromaRow, rowA, rowB, simdEnd, groups, y, yPitch, cb, cr, cPitch);
    }
}
} // namespace srtgw
