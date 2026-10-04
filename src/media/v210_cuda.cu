// v210 <-> NV12 / P010 conversion kernels for the NVDEC and NVENC paths.
//
// Compiled to PTX with Clang (no CUDA headers or libraries: -nocudainc
// -nocudalib), embedded in the binary and loaded at runtime through the CUDA
// driver API, the way FFmpeg builds its CUDA filters (--enable-cuda-llvm).
// One thread converts one 6-pixel v210 group of one row.

#define KERNEL extern "C" __attribute__((global))
#define DEVICE static __attribute__((device)) inline

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

DEVICE int groupIndex()
{
    return __nvvm_read_ptx_sreg_ctaid_x() * __nvvm_read_ptx_sreg_ntid_x() + __nvvm_read_ptx_sreg_tid_x();
}

DEVICE int rowIndex()
{
    return __nvvm_read_ptx_sreg_ctaid_y();
}

DEVICE int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// The 4:2:0 chroma row(s) for luma row y of a 4:2:2 picture. Progressive:
// chroma sits between luma rows 2k and 2k+1, so row y blends its own chroma
// row 3:1 with the next one away (MPEG-2/H.264 siting). Interlaced: the same
// within the field (chroma rows of a field alternate like its luma rows).
DEVICE void chromaRows(int y, int chromaHeight, int interlaced, int* near, int* far)
{
    if (interlaced)
    {
        int const field = y & 1;
        int const fieldRow = y >> 1;
        int const fieldChroma = (chromaHeight + 1 - field) / 2;
        int const c = fieldRow >> 1;
        int const other = (fieldRow & 1) ? c + 1 : c - 1;
        *near = clampi(c, 0, fieldChroma - 1) * 2 + field;
        *far = clampi(other, 0, fieldChroma - 1) * 2 + field;
        if (*near >= chromaHeight)
        {
            *near = chromaHeight - 1;
        }
        if (*far >= chromaHeight)
        {
            *far = chromaHeight - 1;
        }
        return;
    }
    int const c = y >> 1;
    *near = c;
    *far = clampi((y & 1) ? c + 1 : c - 1, 0, chromaHeight - 1);
}

DEVICE u32 blend31(u32 near, u32 far)
{
    return (3 * near + far + 2) >> 2;
}

// 10-bit to 8-bit with rounding; 1021..1023 would round to 256.
DEVICE u8 to8(u32 v)
{
    u32 const r = (v + 2) >> 2;
    return (u8)(r > 255 ? 255 : r);
}

DEVICE void writeGroup(u8* dst, u32 const* y, u32 const* cb, u32 const* cr)
{
    u32* out = (u32*)dst;
    out[0] = cb[0] | (y[0] << 10) | (cr[0] << 20);
    out[1] = y[1] | (cb[1] << 10) | (y[2] << 20);
    out[2] = cr[1] | (y[3] << 10) | (cb[2] << 20);
    out[3] = y[4] | (cr[2] << 10) | (y[5] << 20);
}

// 8-bit NV12 (4:2:0) to 10-bit v210 (4:2:2). Samples are scaled by 4
// (limited range 16..235 maps exactly onto 64..940).
KERNEL void nv12ToV210(u8 const* lumaPlane, int lumaPitch, u8 const* chromaPlane, int chromaPitch, int width, int height, int interlaced, u8* dst,
    int rowBytes)
{
    int const group = groupIndex();
    int const row = rowIndex();
    int const groups = (width + 5) / 6;
    if (row >= height || group >= groups)
    {
        return;
    }
    int near = 0;
    int far = 0;
    chromaRows(row, (height + 1) / 2, interlaced, &near, &far);
    u8 const* luma = lumaPlane + row * lumaPitch;
    u8 const* chromaNear = chromaPlane + near * chromaPitch;
    u8 const* chromaFar = chromaPlane + far * chromaPitch;
    u32 y[6];
    u32 cb[3];
    u32 cr[3];
    for (int i = 0; i < 6; ++i)
    {
        int const x = clampi(group * 6 + i, 0, width - 1);
        y[i] = (u32)luma[x] << 2;
    }
    for (int i = 0; i < 3; ++i)
    {
        int const cx = clampi(group * 3 + i, 0, (width + 1) / 2 - 1);
        cb[i] = blend31(chromaNear[2 * cx], chromaFar[2 * cx]) << 2;
        cr[i] = blend31(chromaNear[2 * cx + 1], chromaFar[2 * cx + 1]) << 2;
    }
    writeGroup(dst + row * rowBytes + group * 16, y, cb, cr);
}

// P010 (10-bit 4:2:0, samples in the high 10 bits of 16) to v210.
KERNEL void p010ToV210(u8 const* lumaPlane, int lumaPitch, u8 const* chromaPlane, int chromaPitch, int width, int height, int interlaced, u8* dst,
    int rowBytes)
{
    int const group = groupIndex();
    int const row = rowIndex();
    int const groups = (width + 5) / 6;
    if (row >= height || group >= groups)
    {
        return;
    }
    int near = 0;
    int far = 0;
    chromaRows(row, (height + 1) / 2, interlaced, &near, &far);
    u16 const* luma = (u16 const*)(lumaPlane + row * lumaPitch);
    u16 const* chromaNear = (u16 const*)(chromaPlane + near * chromaPitch);
    u16 const* chromaFar = (u16 const*)(chromaPlane + far * chromaPitch);
    u32 y[6];
    u32 cb[3];
    u32 cr[3];
    for (int i = 0; i < 6; ++i)
    {
        int const x = clampi(group * 6 + i, 0, width - 1);
        y[i] = (u32)luma[x] >> 6;
    }
    for (int i = 0; i < 3; ++i)
    {
        int const cx = clampi(group * 3 + i, 0, (width + 1) / 2 - 1);
        cb[i] = blend31(chromaNear[2 * cx] >> 6, chromaFar[2 * cx] >> 6);
        cr[i] = blend31(chromaNear[2 * cx + 1] >> 6, chromaFar[2 * cx + 1] >> 6);
    }
    writeGroup(dst + row * rowBytes + group * 16, y, cb, cr);
}

// v210 (10-bit 4:2:2) to 8-bit NV12 (4:2:0) for NVENC. One thread per group of
// a chroma row pair: it writes two luma rows and one chroma row. Chroma is the
// mean of the two source rows (of the same field when interlaced).
KERNEL void v210ToNv12(u8 const* src, int rowBytes, int width, int height, int interlaced, u8* lumaPlane, int lumaPitch, u8* chromaPlane, int chromaPitch)
{
    int const group = groupIndex();
    int const chromaRow = rowIndex();
    int const groups = (width + 5) / 6;
    int const chromaHeight = (height + 1) / 2;
    if (chromaRow >= chromaHeight || group >= groups)
    {
        return;
    }
    // Progressive: rows 2r and 2r+1. Interlaced: chroma row r belongs to field r & 1
    // and averages two rows of that field.
    int rowA = 2 * chromaRow;
    int rowB = rowA + 1;
    if (interlaced)
    {
        int const field = chromaRow & 1;
        int const fieldRow = (chromaRow >> 1) * 2;
        rowA = (fieldRow * 2) + field;
        rowB = rowA + 2;
    }
    rowA = clampi(rowA, 0, height - 1);
    rowB = clampi(rowB, 0, height - 1);
    u32 const* a = (u32 const*)(src + rowA * rowBytes + group * 16);
    u32 const* b = (u32 const*)(src + rowB * rowBytes + group * 16);
    u32 const cbA[3] = {a[0] & 0x3ff, (a[1] >> 10) & 0x3ff, (a[2] >> 20) & 0x3ff};
    u32 const crA[3] = {(a[0] >> 20) & 0x3ff, a[2] & 0x3ff, (a[3] >> 10) & 0x3ff};
    u32 const cbB[3] = {b[0] & 0x3ff, (b[1] >> 10) & 0x3ff, (b[2] >> 20) & 0x3ff};
    u32 const crB[3] = {(b[0] >> 20) & 0x3ff, b[2] & 0x3ff, (b[3] >> 10) & 0x3ff};
    for (int i = 0; i < 3; ++i)
    {
        int const cx = group * 3 + i;
        if (2 * cx < width)
        {
            chromaPlane[chromaRow * chromaPitch + 2 * cx] = to8((cbA[i] + cbB[i] + 1) >> 1);
            chromaPlane[chromaRow * chromaPitch + 2 * cx + 1] = to8((crA[i] + crB[i] + 1) >> 1);
        }
    }
    // Luma of the two progressive rows this chroma row covers (interlaced: every row
    // is written by the thread of its own chroma row pair below).
    int const lumaRows[2] = {2 * chromaRow, 2 * chromaRow + 1};
    for (int r = 0; r < 2; ++r)
    {
        int const row = lumaRows[r];
        if (row >= height)
        {
            continue;
        }
        u32 const* w = (u32 const*)(src + row * rowBytes + group * 16);
        u32 const ys[6] = {(w[0] >> 10) & 0x3ff, w[1] & 0x3ff, (w[1] >> 20) & 0x3ff, (w[2] >> 10) & 0x3ff, w[3] & 0x3ff, (w[3] >> 20) & 0x3ff};
        for (int i = 0; i < 6; ++i)
        {
            int const x = group * 6 + i;
            if (x < width)
            {
                lumaPlane[row * lumaPitch + x] = to8(ys[i]);
            }
        }
    }
}
