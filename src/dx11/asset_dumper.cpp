#include "dx11/asset_dumper.h"
#include "common/logger.h"
#include "dx11/dxgi_format_names.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace lotro_uie {

namespace {

#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};

struct DdsHeader {
    uint32_t dwMagic;
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DdsPixelFormat ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};

struct DdsHeaderDxt10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
#pragma pack(pop)

constexpr uint32_t MakeFourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(a) |
           (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) |
           (static_cast<uint32_t>(d) << 24);
}

size_t FormatBitsPerPixel(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return 128;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_FLOAT:
            return 64;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R32_FLOAT:
            return 32;
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
            return 16;
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_A8_UNORM:
            return 8;
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 4;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 8;
        default:
            return 0;
    }
}

void ComputeSurfacePitch(DXGI_FORMAT fmt, UINT width, UINT height, size_t& outRowBytes, size_t& outNumRows) {
    if (IsBlockCompressedFormat(fmt)) {
        const size_t blockBytes = (FormatBitsPerPixel(fmt) == 4) ? 8 : 16;
        const size_t blocksWide = std::max<size_t>(1, (width + 3) / 4);
        const size_t blocksHigh = std::max<size_t>(1, (height + 3) / 4);
        outRowBytes = blocksWide * blockBytes;
        outNumRows = blocksHigh;
    } else {
        const size_t bpp = FormatBitsPerPixel(fmt);
        outRowBytes = (static_cast<size_t>(width) * bpp + 7) / 8;
        outNumRows = std::max<size_t>(1, height);
    }
}

bool WriteBmp24Rgb(const std::filesystem::path& path, UINT width, UINT height, const std::vector<uint8_t>& rgbaOrBgra, bool isBgra, bool compositeAlpha) {
    if (width == 0 || height == 0 || rgbaOrBgra.size() < static_cast<size_t>(width) * height * 4) {
        return false;
    }

    // Check if alpha channel carries meaningful variation (vs all 0 or all 255).
    uint8_t minA = 255, maxA = 0;
    if (compositeAlpha) {
        for (size_t i = 3; i < static_cast<size_t>(width) * height * 4; i += 4) {
            minA = std::min(minA, rgbaOrBgra[i]);
            maxA = std::max(maxA, rgbaOrBgra[i]);
        }
    }
    const bool useAlphaComposite = compositeAlpha && (maxA > 0) && (minA < 255);

    const size_t rowStride = ((static_cast<size_t>(width) * 3 + 3) / 4) * 4;
    std::vector<uint8_t> bmpPixels(rowStride * height, 0);

    for (UINT y = 0; y < height; ++y) {
        const uint8_t* srcRow = rgbaOrBgra.data() + static_cast<size_t>(y) * width * 4;
        uint8_t* dstRow = bmpPixels.data() + static_cast<size_t>(y) * rowStride;
        for (UINT x = 0; x < width; ++x) {
            const uint8_t c0 = srcRow[x * 4 + 0];
            const uint8_t c1 = srcRow[x * 4 + 1];
            const uint8_t c2 = srcRow[x * 4 + 2];
            const uint8_t a  = srcRow[x * 4 + 3];
            uint8_t b = isBgra ? c0 : c2;
            uint8_t g = c1;
            uint8_t r = isBgra ? c2 : c0;
            if (useAlphaComposite) {
                // Dark slate background (24, 24, 32) makes white/light font glyphs and UI borders clearly visible.
                const uint32_t invA = 255 - a;
                b = static_cast<uint8_t>((b * a + 32 * invA) / 255);
                g = static_cast<uint8_t>((g * a + 24 * invA) / 255);
                r = static_cast<uint8_t>((r * a + 24 * invA) / 255);
            }
            dstRow[x * 3 + 0] = b;
            dstRow[x * 3 + 1] = g;
            dstRow[x * 3 + 2] = r;
        }
    }

    BITMAPFILEHEADER bfh{};
    BITMAPINFOHEADER bih{};
    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + static_cast<DWORD>(bmpPixels.size());

    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = static_cast<LONG>(width);
    bih.biHeight = -static_cast<LONG>(height); // Top-down
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = static_cast<DWORD>(bmpPixels.size());

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    out.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    out.write(reinterpret_cast<const char*>(bmpPixels.data()), static_cast<std::streamsize>(bmpPixels.size()));
    return out.good();
}

void UnpackRgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint8_t r5 = static_cast<uint8_t>((c >> 11) & 0x1F);
    const uint8_t g6 = static_cast<uint8_t>((c >> 5) & 0x3F);
    const uint8_t b5 = static_cast<uint8_t>(c & 0x1F);
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

void DecodeBc1Block(const uint8_t* blk, uint8_t outRgba[16][4], bool allowPunchthrough) {
    const uint16_t c0 = static_cast<uint16_t>(blk[0] | (blk[1] << 8));
    const uint16_t c1 = static_cast<uint16_t>(blk[2] | (blk[3] << 8));
    uint8_t palette[4][4]{};
    UnpackRgb565(c0, palette[0][0], palette[0][1], palette[0][2]);
    palette[0][3] = 255;
    UnpackRgb565(c1, palette[1][0], palette[1][1], palette[1][2]);
    palette[1][3] = 255;

    if (c0 > c1 || !allowPunchthrough) {
        for (int ch = 0; ch < 3; ++ch) {
            palette[2][ch] = static_cast<uint8_t>((2 * palette[0][ch] + palette[1][ch] + 1) / 3);
            palette[3][ch] = static_cast<uint8_t>((palette[0][ch] + 2 * palette[1][ch] + 1) / 3);
        }
        palette[2][3] = 255;
        palette[3][3] = 255;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            palette[2][ch] = static_cast<uint8_t>((palette[0][ch] + palette[1][ch]) / 2);
            palette[3][ch] = 0;
        }
        palette[2][3] = 255;
        palette[3][3] = 0;
    }

    const uint32_t codes = static_cast<uint32_t>(blk[4]) |
                           (static_cast<uint32_t>(blk[5]) << 8) |
                           (static_cast<uint32_t>(blk[6]) << 16) |
                           (static_cast<uint32_t>(blk[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const uint32_t idx = (codes >> (i * 2)) & 0x3;
        std::memcpy(outRgba[i], palette[idx], 4);
    }
}

void DecodeBcImage(DXGI_FORMAT fmt, UINT width, UINT height, const uint8_t* src, size_t srcPitch, std::vector<uint8_t>& outRgba) {
    outRgba.assign(static_cast<size_t>(width) * height * 4, 0);
    const UINT bw = std::max<UINT>(1, (width + 3) / 4);
    const UINT bh = std::max<UINT>(1, (height + 3) / 4);
    const bool isBc1 = (fmt == DXGI_FORMAT_BC1_UNORM || fmt == DXGI_FORMAT_BC1_UNORM_SRGB || fmt == DXGI_FORMAT_BC1_TYPELESS);
    const bool isBc2 = (fmt == DXGI_FORMAT_BC2_UNORM || fmt == DXGI_FORMAT_BC2_UNORM_SRGB || fmt == DXGI_FORMAT_BC2_TYPELESS);
    const size_t blockSize = isBc1 ? 8 : 16;

    for (UINT by = 0; by < bh; ++by) {
        const uint8_t* rowPtr = src + static_cast<size_t>(by) * srcPitch;
        for (UINT bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = rowPtr + static_cast<size_t>(bx) * blockSize;
            uint8_t blockRgba[16][4]{};
            if (isBc1) {
                DecodeBc1Block(blk, blockRgba, true);
            } else if (isBc2) {
                DecodeBc1Block(blk + 8, blockRgba, false);
                for (int i = 0; i < 16; ++i) {
                    const uint8_t byteVal = blk[i / 2];
                    const uint8_t nibble = (i % 2 == 0) ? (byteVal & 0x0F) : ((byteVal >> 4) & 0x0F);
                    blockRgba[i][3] = static_cast<uint8_t>((nibble << 4) | nibble);
                }
            } else {
                // BC3 (DXT5)
                DecodeBc1Block(blk + 8, blockRgba, false);
                uint8_t alphas[8]{};
                alphas[0] = blk[0];
                alphas[1] = blk[1];
                if (alphas[0] > alphas[1]) {
                    for (int k = 1; k <= 6; ++k) {
                        alphas[1 + k] = static_cast<uint8_t>(((7 - k) * alphas[0] + k * alphas[1] + 3) / 7);
                    }
                } else {
                    for (int k = 1; k <= 4; ++k) {
                        alphas[1 + k] = static_cast<uint8_t>(((5 - k) * alphas[0] + k * alphas[1] + 2) / 5);
                    }
                    alphas[6] = 0;
                    alphas[7] = 255;
                }
                uint64_t aBits = 0;
                for (int k = 0; k < 6; ++k) {
                    aBits |= static_cast<uint64_t>(blk[2 + k]) << (8 * k);
                }
                for (int i = 0; i < 16; ++i) {
                    const uint32_t aIdx = static_cast<uint32_t>((aBits >> (i * 3)) & 0x7);
                    blockRgba[i][3] = alphas[aIdx];
                }
            }

            for (UINT py = 0; py < 4; ++py) {
                const UINT y = by * 4 + py;
                if (y >= height) continue;
                for (UINT px = 0; px < 4; ++px) {
                    const UINT x = bx * 4 + px;
                    if (x >= width) continue;
                    std::memcpy(&outRgba[(static_cast<size_t>(y) * width + x) * 4], blockRgba[py * 4 + px], 4);
                }
            }
        }
    }
}

using PfnD3DDisassemble = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

PfnD3DDisassemble GetD3DDisassembleFn() {
    static PfnD3DDisassemble fn = []() -> PfnD3DDisassemble {
        HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<PfnD3DDisassemble>(GetProcAddress(mod, "D3DDisassemble"));
    }();
    return fn;
}

uint32_t ComputeCrc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t ComputeAdler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ) {
        const size_t chunk = (std::min<size_t>)(len - i, 5552);
        for (size_t j = 0; j < chunk; ++j) {
            a += data[i + j];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        i += chunk;
    }
    return (b << 16) | a;
}

void AppendBe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8)  & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendPngChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* payload, size_t len) {
    AppendBe32(out, static_cast<uint32_t>(len));
    const size_t crcStart = out.size();
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(type), reinterpret_cast<const uint8_t*>(type) + 4);
    if (payload && len > 0) {
        out.insert(out.end(), payload, payload + len);
    }
    const uint32_t crc = ComputeCrc32(out.data() + crcStart, 4 + len);
    AppendBe32(out, crc);
}

// Fast RFC 1951 Fixed-Huffman + LZ77 Deflate RGBA8 PNG writer (zero external dependencies)
bool WritePngRgba8(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t>& srcData,
    bool isBgra
) {
    if (width == 0 || height == 0 || srcData.size() < static_cast<size_t>(width) * height * 4) return false;

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> rawScanlines((rowBytes + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* srcRow = srcData.data() + static_cast<size_t>(y) * rowBytes;
        uint8_t* dstRow = rawScanlines.data() + static_cast<size_t>(y) * (rowBytes + 1);
        dstRow[0] = 1; // Sub filter
        uint8_t prev[4] = {0, 0, 0, 0};
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t r = isBgra ? srcRow[x * 4 + 2] : srcRow[x * 4 + 0];
            const uint8_t g = srcRow[x * 4 + 1];
            const uint8_t b = isBgra ? srcRow[x * 4 + 0] : srcRow[x * 4 + 2];
            const uint8_t a = srcRow[x * 4 + 3];
            dstRow[1 + x * 4 + 0] = static_cast<uint8_t>(r - prev[0]);
            dstRow[1 + x * 4 + 1] = static_cast<uint8_t>(g - prev[1]);
            dstRow[1 + x * 4 + 2] = static_cast<uint8_t>(b - prev[2]);
            dstRow[1 + x * 4 + 3] = static_cast<uint8_t>(a - prev[3]);
            prev[0] = r; prev[1] = g; prev[2] = b; prev[3] = a;
        }
    }

    std::vector<uint8_t> zstream;
    zstream.reserve(rawScanlines.size() / 2 + 64);
    zstream.push_back(0x78);
    zstream.push_back(0x01);

    uint64_t bitBuf = 0;
    int bitCount = 0;
    auto putBits = [&](uint32_t val, int nbits) {
        bitBuf |= (static_cast<uint64_t>(val) << bitCount);
        bitCount += nbits;
        while (bitCount >= 8) {
            zstream.push_back(static_cast<uint8_t>(bitBuf & 0xFFu));
            bitBuf >>= 8;
            bitCount -= 8;
        }
    };
    auto putRevBits = [&](uint32_t code, int nbits) {
        uint32_t rev = 0;
        for (int i = 0; i < nbits; ++i) {
            rev = (rev << 1) | ((code >> i) & 1u);
        }
        putBits(rev, nbits);
    };
    auto emitLitLen = [&](uint32_t sym) {
        if (sym <= 143) putRevBits(0x30u + sym, 8);
        else if (sym <= 255) putRevBits(0x190u + (sym - 144u), 9);
        else if (sym <= 279) putRevBits(sym - 256u, 7);
        else putRevBits(0xC0u + (sym - 280u), 8);
    };

    static const uint16_t kLenBase[29] = {
        3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31,
        35,43,51,59, 67,83,99,115, 131,163,195,227, 258
    };
    static const uint8_t kLenExtra[29] = {
        0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2,
        3,3,3,3, 4,4,4,4, 5,5,5,5, 0
    };
    static const uint16_t kDistBase[30] = {
        1,2,3,4, 5,7,9,13, 17,25,33,49, 65,97,129,193,
        257,385,513,769, 1025,1537,2049,3073, 4097,6145,8193,12289, 16385,24577
    };
    static const uint8_t kDistExtra[30] = {
        0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6,
        7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13
    };

    // BFINAL=1, BTYPE=01 (Fixed Huffman)
    putBits(1u, 1);
    putBits(1u, 2);

    const size_t n = rawScanlines.size();
    const uint8_t* s = rawScanlines.data();
    std::vector<int32_t> head(8192, -1);

    for (size_t pos = 0; pos < n; ) {
        size_t bestLen = 0;
        size_t bestDist = 0;
        if (pos + 3 < n) {
            const uint32_t h = ((static_cast<uint32_t>(s[pos]) * 2654435761u) ^
                                (static_cast<uint32_t>(s[pos + 1]) << 8) ^
                                (static_cast<uint32_t>(s[pos + 2]) << 16) ^
                                static_cast<uint32_t>(s[pos + 3])) & 8191u;
            const int32_t prevPos = head[h];
            head[h] = static_cast<int32_t>(pos);

            // Check distance 1, distance 4 (previous pixel), and hash match
            const size_t candidates[3] = {
                (pos >= 1) ? (pos - 1) : pos,
                (pos >= 4) ? (pos - 4) : pos,
                (prevPos >= 0 && pos - static_cast<size_t>(prevPos) <= 32768) ? static_cast<size_t>(prevPos) : pos
            };
            const size_t maxL = (std::min<size_t>)(258, n - pos);
            for (size_t cand : candidates) {
                if (cand >= pos) continue;
                const size_t dist = pos - cand;
                if (dist > 32768 || s[cand] != s[pos] || s[cand + 1] != s[pos + 1] || s[cand + 2] != s[pos + 2]) continue;
                size_t l = 3;
                while (l < maxL && s[cand + l] == s[pos + l]) ++l;
                if (l > bestLen) {
                    bestLen = l;
                    bestDist = dist;
                    if (bestLen == maxL) break;
                }
            }
        }

        if (bestLen >= 3) {
            int lenIdx = 28;
            for (int i = 0; i < 28; ++i) {
                if (bestLen >= kLenBase[i] && bestLen < kLenBase[i + 1]) {
                    lenIdx = i;
                    break;
                }
            }
            emitLitLen(257u + static_cast<uint32_t>(lenIdx));
            if (kLenExtra[lenIdx] > 0) {
                putBits(static_cast<uint32_t>(bestLen - kLenBase[lenIdx]), kLenExtra[lenIdx]);
            }

            int distIdx = 29;
            for (int i = 0; i < 29; ++i) {
                if (bestDist >= kDistBase[i] && bestDist < kDistBase[i + 1]) {
                    distIdx = i;
                    break;
                }
            }
            putRevBits(static_cast<uint32_t>(distIdx), 5);
            if (kDistExtra[distIdx] > 0) {
                putBits(static_cast<uint32_t>(bestDist - kDistBase[distIdx]), kDistExtra[distIdx]);
            }
            pos += bestLen;
        } else {
            emitLitLen(s[pos]);
            ++pos;
        }
    }

    emitLitLen(256u); // End of block
    if (bitCount > 0) {
        zstream.push_back(static_cast<uint8_t>(bitBuf & 0xFFu));
    }
    AppendBe32(zstream, ComputeAdler32(rawScanlines.data(), rawScanlines.size()));

    std::vector<uint8_t> png;
    png.reserve(zstream.size() + 64);
    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), kPngSig, kPngSig + 8);

    std::vector<uint8_t> ihdr;
    ihdr.reserve(13);
    AppendBe32(ihdr, width);
    AppendBe32(ihdr, height);
    ihdr.push_back(8); // bit depth 8
    ihdr.push_back(6); // color type 6 = RGBA
    ihdr.push_back(0); // compression
    ihdr.push_back(0); // filter
    ihdr.push_back(0); // interlace
    AppendPngChunk(png, "IHDR", ihdr.data(), ihdr.size());
    AppendPngChunk(png, "IDAT", zstream.data(), zstream.size());
    AppendPngChunk(png, "IEND", nullptr, 0);

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return out.good();
}

} // namespace

bool DumpTextureToDisk(
    ID3D11Device* device,
    ID3D11DeviceContext* immediateCtx,
    ID3D11Texture2D* tex,
    const TextureMeta& meta,
    const std::filesystem::path& outDir,
    const std::string& baseName,
    const std::string& dumpFormat
) {
    if (!device || !immediateCtx || !tex) {
        return false;
    }
    if (meta.width == 0 || meta.height == 0 || meta.sampleCount > 1) {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    tex->GetDesc(&srcDesc);

    D3D11_TEXTURE2D_DESC stagingDesc = srcDesc;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        UIE_LOG_WARN("DumpTexture '%s': CreateTexture2D(STAGING) failed (hr=0x%08X, fmt=%s)",
                     baseName.c_str(), static_cast<unsigned>(hr), DxgiFormatName(srcDesc.Format));
        return false;
    }

    immediateCtx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = immediateCtx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        staging->Release();
        UIE_LOG_WARN("DumpTexture '%s': Map(READ) failed (hr=0x%08X)", baseName.c_str(), static_cast<unsigned>(hr));
        return false;
    }

    size_t tightRowBytes = 0;
    size_t numRows = 0;
    ComputeSurfacePitch(srcDesc.Format, srcDesc.Width, srcDesc.Height, tightRowBytes, numRows);

    bool wroteAny = false;
    if (tightRowBytes > 0 && numRows > 0) {
        std::vector<uint8_t> tightBytes(tightRowBytes * numRows);
        const auto* srcBytes = static_cast<const uint8_t*>(mapped.pData);
        const size_t copyBytes = std::min<size_t>(tightRowBytes, mapped.RowPitch);
        for (size_t r = 0; r < numRows; ++r) {
            std::memcpy(tightBytes.data() + r * tightRowBytes, srcBytes + r * mapped.RowPitch, copyBytes);
        }

        const bool wantPng = (dumpFormat.empty() || dumpFormat == "png" || dumpFormat == "png+dds" || dumpFormat == "all");
        const bool wantDds = (dumpFormat == "dds" || dumpFormat == "png+dds" || dumpFormat == "all");
        const bool wantBmp = (dumpFormat == "bmp" || dumpFormat == "all");

        const auto* packed = tightBytes.data();
        bool wrotePng = false;
        if (wantPng || wantBmp) {
            if (srcDesc.Format == DXGI_FORMAT_R8_UNORM || srcDesc.Format == DXGI_FORMAT_A8_UNORM || srcDesc.Format == DXGI_FORMAT_R8_TYPELESS) {
                std::vector<uint8_t> rgba(static_cast<size_t>(srcDesc.Width) * srcDesc.Height * 4);
                for (size_t i = 0; i < static_cast<size_t>(srcDesc.Width) * srcDesc.Height; ++i) {
                    const uint8_t v = packed[i];
                    rgba[i * 4 + 0] = v;
                    rgba[i * 4 + 1] = v;
                    rgba[i * 4 + 2] = v;
                    rgba[i * 4 + 3] = 255;
                }
                if (wantPng && WritePngRgba8(outDir / (baseName + ".png"), srcDesc.Width, srcDesc.Height, rgba, false)) wrotePng = true;
                if (wantBmp) WriteBmp24Rgb(outDir / (baseName + ".bmp"), srcDesc.Width, srcDesc.Height, rgba, false, false);
            } else if (srcDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || srcDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                       srcDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || srcDesc.Format == DXGI_FORMAT_B8G8R8X8_UNORM ||
                       srcDesc.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) {
                if (wantPng && WritePngRgba8(outDir / (baseName + ".png"), srcDesc.Width, srcDesc.Height, tightBytes, true)) wrotePng = true;
                if (wantBmp) WriteBmp24Rgb(outDir / (baseName + ".bmp"), srcDesc.Width, srcDesc.Height, tightBytes, true, true);
            } else if (srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                       srcDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) {
                if (wantPng && WritePngRgba8(outDir / (baseName + ".png"), srcDesc.Width, srcDesc.Height, tightBytes, false)) wrotePng = true;
                if (wantBmp) WriteBmp24Rgb(outDir / (baseName + ".bmp"), srcDesc.Width, srcDesc.Height, tightBytes, false, true);
            } else if (srcDesc.Format == DXGI_FORMAT_BC1_UNORM || srcDesc.Format == DXGI_FORMAT_BC1_UNORM_SRGB ||
                       srcDesc.Format == DXGI_FORMAT_BC1_TYPELESS || srcDesc.Format == DXGI_FORMAT_BC2_UNORM ||
                       srcDesc.Format == DXGI_FORMAT_BC2_UNORM_SRGB || srcDesc.Format == DXGI_FORMAT_BC2_TYPELESS ||
                       srcDesc.Format == DXGI_FORMAT_BC3_UNORM || srcDesc.Format == DXGI_FORMAT_BC3_UNORM_SRGB ||
                       srcDesc.Format == DXGI_FORMAT_BC3_TYPELESS) {
                std::vector<uint8_t> rgba;
                DecodeBcImage(srcDesc.Format, srcDesc.Width, srcDesc.Height, packed, tightRowBytes, rgba);
                if (wantPng && WritePngRgba8(outDir / (baseName + ".png"), srcDesc.Width, srcDesc.Height, rgba, false)) wrotePng = true;
                if (wantBmp) WriteBmp24Rgb(outDir / (baseName + ".bmp"), srcDesc.Width, srcDesc.Height, rgba, false, true);
            }
            wroteAny = wroteAny || wrotePng;
        }

        if (wantDds || !wrotePng) {
            DdsHeader hdr{};
            hdr.dwMagic = MakeFourCC('D', 'D', 'S', ' ');
            hdr.dwSize = 124;
            hdr.dwFlags = 0x1 /*CAPS*/ | 0x2 /*HEIGHT*/ | 0x4 /*WIDTH*/ | 0x1000 /*PIXELFORMAT*/ |
                          (IsBlockCompressedFormat(srcDesc.Format) ? 0x80000u /*LINEARSIZE*/ : 0x8u /*PITCH*/);
            hdr.dwHeight = srcDesc.Height;
            hdr.dwWidth = srcDesc.Width;
            hdr.dwPitchOrLinearSize = static_cast<uint32_t>(
                IsBlockCompressedFormat(srcDesc.Format) ? tightBytes.size() : tightRowBytes
            );
            hdr.dwDepth = 1;
            hdr.dwMipMapCount = 1;
            hdr.ddspf.dwSize = 32;
            hdr.ddspf.dwFlags = 0x4; // DDPF_FOURCC
            hdr.ddspf.dwFourCC = MakeFourCC('D', 'X', '1', '0');
            hdr.dwCaps = 0x1000; // DDSCAPS_TEXTURE

            DdsHeaderDxt10 dxt10{};
            dxt10.dxgiFormat = static_cast<uint32_t>(srcDesc.Format);
            dxt10.resourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
            dxt10.arraySize = 1;

            std::ofstream ddsOut(outDir / (baseName + ".dds"), std::ios::binary);
            if (ddsOut.is_open()) {
                ddsOut.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
                ddsOut.write(reinterpret_cast<const char*>(&dxt10), sizeof(dxt10));
                ddsOut.write(reinterpret_cast<const char*>(tightBytes.data()), static_cast<std::streamsize>(tightBytes.size()));
                wroteAny = wroteAny || ddsOut.good();
            }
        }
    }

    immediateCtx->Unmap(staging, 0);
    staging->Release();
    return wroteAny;
}

bool DumpShaderToDisk(
    const ShaderMeta& shader,
    const std::filesystem::path& outDir,
    const std::string& baseName
) {
    if (shader.bytecode.empty()) return false;

    bool ok = false;
    {
        std::ofstream dxbcOut(outDir / (baseName + ".dxbc"), std::ios::binary);
        if (dxbcOut.is_open()) {
            dxbcOut.write(reinterpret_cast<const char*>(shader.bytecode.data()),
                          static_cast<std::streamsize>(shader.bytecode.size()));
            ok = dxbcOut.good();
        }
    }

    if (auto disasm = GetD3DDisassembleFn()) {
        ID3DBlob* blob = nullptr;
        const HRESULT hr = disasm(
            shader.bytecode.data(),
            shader.bytecode.size(),
            D3D_DISASM_ENABLE_INSTRUCTION_OFFSET,
            nullptr,
            &blob
        );
        if (SUCCEEDED(hr) && blob) {
            std::ofstream asmOut(outDir / (baseName + ".asm"), std::ios::binary);
            if (asmOut.is_open()) {
                const char* text = static_cast<const char*>(blob->GetBufferPointer());
                size_t len = static_cast<size_t>(blob->GetBufferSize());
                while (len > 0 && text[len - 1] == '\0') {
                    --len;
                }
                asmOut.write(text, static_cast<std::streamsize>(len));
            }
            blob->Release();
        }
    }

    return ok;
}

bool LoadImageRgba8(
    const std::filesystem::path& filePath,
    uint32_t& outWidth,
    uint32_t& outHeight,
    std::vector<uint8_t>& outRgba
) {
    outWidth = 0;
    outHeight = 0;
    outRgba.clear();

    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec)) {
        return false;
    }

    // 1. Native .dds loader (supports DX10 R8G8B8A8, B8G8R8A8, R8, BC1, BC2, BC3)
    auto ext = filePath.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext == L".dds") {
        std::ifstream f(filePath, std::ios::binary);
        if (!f.is_open()) return false;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.size() < sizeof(DdsHeader)) return false;
        const auto* hdr = reinterpret_cast<const DdsHeader*>(bytes.data());
        if (hdr->dwMagic != MakeFourCC('D', 'D', 'S', ' ') || hdr->dwWidth == 0 || hdr->dwHeight == 0) {
            return false;
        }
        size_t offset = sizeof(DdsHeader);
        DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        if ((hdr->ddspf.dwFlags & 0x4u) != 0 && hdr->ddspf.dwFourCC == MakeFourCC('D', 'X', '1', '0')) {
            if (bytes.size() < offset + sizeof(DdsHeaderDxt10)) return false;
            const auto* dxt10 = reinterpret_cast<const DdsHeaderDxt10*>(bytes.data() + offset);
            fmt = static_cast<DXGI_FORMAT>(dxt10->dxgiFormat);
            offset += sizeof(DdsHeaderDxt10);
        }
        const uint32_t w = hdr->dwWidth;
        const uint32_t h = hdr->dwHeight;
        const uint8_t* payload = bytes.data() + offset;
        const size_t payloadBytes = bytes.size() - offset;
        if (IsBlockCompressedFormat(fmt)) {
            const size_t bx = (w + 3) / 4;
            const size_t by = (h + 3) / 4;
            const size_t blockBytes = (FormatBitsPerPixel(fmt) == 4) ? 8 : 16;
            const size_t rowBytes = bx * blockBytes;
            if (payloadBytes < rowBytes * by) return false;
            DecodeBcImage(fmt, w, h, payload, rowBytes, outRgba);
            outWidth = w;
            outHeight = h;
            return !outRgba.empty();
        } else if (FormatBitsPerPixel(fmt) == 32) {
            const size_t need = static_cast<size_t>(w) * h * 4;
            if (payloadBytes < need) return false;
            outRgba.assign(payload, payload + need);
            if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
                for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
                    std::swap(outRgba[i * 4 + 0], outRgba[i * 4 + 2]);
                }
            }
            outWidth = w;
            outHeight = h;
            return true;
        }
        return false;
    }

    // 2. Windows Imaging Component (WIC) loader for .png, .bmp, .jpg, .tiff
    using PfnWICCreateImagingFactory = HRESULT(WINAPI*)(UINT, IWICImagingFactory**);
    static PfnWICCreateImagingFactory s_createFactory = []() -> PfnWICCreateImagingFactory {
        HMODULE mod = LoadLibraryW(L"windowscodecs.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<PfnWICCreateImagingFactory>(
            GetProcAddress(mod, "WICCreateImagingFactory_Proxy")
        );
    }();
    if (!s_createFactory) return false;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(s_createFactory(WINCODEC_SDK_VERSION, &factory)) || !factory) {
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = factory->CreateDecoderFromFilename(
        filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder
    );
    if (FAILED(hr) || !decoder) {
        factory->Release();
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        decoder->Release();
        factory->Release();
        return false;
    }

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0 || w > 8192 || h > 8192) {
        frame->Release();
        decoder->Release();
        factory->Release();
        return false;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        frame->Release();
        decoder->Release();
        factory->Release();
        return false;
    }

    hr = converter->Initialize(
        frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom
    );
    bool success = false;
    if (SUCCEEDED(hr)) {
        const UINT stride = w * 4;
        const UINT bufSize = stride * h;
        outRgba.resize(bufSize);
        if (SUCCEEDED(converter->CopyPixels(nullptr, stride, bufSize, outRgba.data()))) {
            outWidth = w;
            outHeight = h;
            success = true;
        } else {
            outRgba.clear();
        }
    }

    converter->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    return success;
}

} // namespace lotro_uie
