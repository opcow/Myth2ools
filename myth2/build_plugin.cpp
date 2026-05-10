// build_plugin.cpp
// Rebuild a Myth II dng2 plugin from an extracted map folder.
//
// First pass:
//   - writes a valid dng2 plugin header, one entry point, and tag directory
//   - includes raw/mesh_tag.bin
//   - includes terrain/terrain_tag.bin when present
//   - includes strings/name_tag.bin, optionally rebuilt from strings/name.txt
//   - includes screens/*_tag.bin when present
//
// Usage:
//   build_plugin <folder> [output] [--edit] [--obj <input.obj>] [--water-obj <input.obj>] [--heightscale <n>] [--water] [--water-flags] [--animation]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>

#include "mesh_flags.h"

static uint32_t swap32(uint32_t n) {
    return ((n & 0xFF000000u) >> 24) | ((n & 0x00FF0000u) >> 8)
         | ((n & 0x0000FF00u) << 8)  | ((n & 0x000000FFu) << 24);
}
static uint16_t swap16(uint16_t n) { return (uint16_t)((n >> 8) | (n << 8)); }

static void writeBE16(FILE* f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) };
    fwrite(b, 1, 2, f);
}
static void writeBE32(FILE* f, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v >> 24), (uint8_t)((v >> 16) & 0xFF),
        (uint8_t)((v >> 8) & 0xFF), (uint8_t)(v & 0xFF)
    };
    fwrite(b, 1, 4, f);
}
static void writeBE16To(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}
static void writeBE32To(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}
static int16_t readBE16s(const uint8_t* b, size_t o) {
    uint16_t v;
    memcpy(&v, b + o, 2);
    return (int16_t)swap16(v);
}
static int32_t readBE32s(const uint8_t* b, size_t o) {
    uint32_t v;
    memcpy(&v, b + o, 4);
    return (int32_t)swap32(v);
}

static std::vector<uint8_t> readFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) {
        fclose(f);
        return {};
    }
    std::vector<uint8_t> buf((size_t)sz);
    if (sz > 0 && fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        return {};
    }
    fclose(f);
    return buf;
}

static bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "Cannot write: %s\n", path.c_str());
        return false;
    }
    bool ok = (data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size());
    fclose(f);
    return ok;
}

static bool fileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static std::string readTextFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) {
        fclose(f);
        return {};
    }
    std::string s((size_t)sz, '\0');
    if (sz > 0 && fread(&s[0], 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        return {};
    }
    fclose(f);
    return s;
}

static uint32_t get32le(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static int32_t get32les(const uint8_t* b) { return (int32_t)get32le(b); }
static uint16_t get16le(const uint8_t* b) { return (uint16_t)(b[0] | ((uint16_t)b[1] << 8)); }

static std::vector<uint8_t> readBMP8(const std::string& path, int& outW, int& outH) {
    auto raw = readFile(path);
    if (raw.size() < 54 || raw[0] != 'B' || raw[1] != 'M') return {};
    uint32_t dataOff = get32le(raw.data() + 10);
    int32_t w = get32les(raw.data() + 18), h = get32les(raw.data() + 22);
    uint16_t bits = get16le(raw.data() + 28);
    if (bits != 8 || w <= 0 || h <= 0) return {};
    outW = w;
    outH = h;
    int stride = (w + 3) & ~3;
    if (dataOff + (size_t)stride * h > raw.size()) return {};
    std::vector<uint8_t> px((size_t)w * h);
    for (int row = 0; row < h; row++) {
        const uint8_t* src = raw.data() + dataOff + (size_t)(h - 1 - row) * stride;
        memcpy(px.data() + (size_t)row * w, src, w);
    }
    return px;
}

static std::vector<uint8_t> readBMPIndexed4Or8(const std::string& path, int& outW, int& outH) {
    auto raw = readFile(path);
    if (raw.size() < 54 || raw[0] != 'B' || raw[1] != 'M') return {};
    uint32_t dataOff = get32le(raw.data() + 10);
    int32_t w = get32les(raw.data() + 18), h = get32les(raw.data() + 22);
    uint16_t bits = get16le(raw.data() + 28);
    if ((bits != 4 && bits != 8) || w <= 0 || h <= 0) return {};
    outW = w;
    outH = h;
    int rawStride = (bits == 4) ? ((w + 1) / 2) : w;
    int stride = (rawStride + 3) & ~3;
    if (dataOff + (size_t)stride * h > raw.size()) return {};
    std::vector<uint8_t> px((size_t)w * h);
    for (int row = 0; row < h; row++) {
        const uint8_t* src = raw.data() + dataOff + (size_t)(h - 1 - row) * stride;
        uint8_t* dst = px.data() + (size_t)row * w;
        for (int x = 0; x < w; x++) {
            dst[x] = (bits == 4)
                ? (uint8_t)((x & 1) ? (src[x / 2] & 0x0F) : (src[x / 2] >> 4))
                : src[x];
        }
    }
    return px;
}

static bool copyBMPPaletteToMyth256(uint8_t* colorBase, const std::vector<uint8_t>& bmpRaw) {
    if (bmpRaw.size() < 54 || bmpRaw[0] != 'B' || bmpRaw[1] != 'M') return false;
    uint32_t dibSz = get32le(bmpRaw.data() + 14);
    uint32_t palStart = 14 + dibSz;
    uint32_t pixOff = get32le(bmpRaw.data() + 10);
    if (palStart > bmpRaw.size() || pixOff > bmpRaw.size() || pixOff < palStart) return false;
    uint32_t nEntries = (pixOff - palStart) / 4;
    if (nEntries > 256) nEntries = 256;
    for (int i = 0; i < 256; i++) {
        uint8_t* ce = colorBase + (size_t)i * 8;
        ce[0] = ce[1] = ce[2] = ce[3] = ce[4] = ce[5] = ce[6] = ce[7] = 0;
    }
    for (uint32_t i = 0; i < nEntries; i++) {
        uint8_t b = bmpRaw[palStart + i * 4 + 0];
        uint8_t g = bmpRaw[palStart + i * 4 + 1];
        uint8_t r = bmpRaw[palStart + i * 4 + 2];
        uint8_t* ce = colorBase + (size_t)i * 8;
        ce[0] = r; ce[1] = 0; ce[2] = g; ce[3] = 0; ce[4] = b; ce[5] = 0; ce[6] = 0; ce[7] = 0;
    }
    return true;
}

static std::vector<uint8_t> readBMP24(const std::string& path, int& outW, int& outH) {
    auto raw = readFile(path);
    if (raw.size() < 54 || raw[0] != 'B' || raw[1] != 'M') return {};
    uint32_t dataOff = get32le(raw.data() + 10);
    int32_t w = get32les(raw.data() + 18), h = get32les(raw.data() + 22);
    uint16_t bits = get16le(raw.data() + 28);
    if (bits != 24 || w <= 0 || h <= 0) return {};
    outW = w;
    outH = h;
    int stride = (w * 3 + 3) & ~3;
    if (dataOff + (size_t)stride * h > raw.size()) return {};
    std::vector<uint8_t> px((size_t)w * h * 3);
    for (int row = 0; row < h; row++) {
        const uint8_t* src = raw.data() + dataOff + (size_t)(h - 1 - row) * stride;
        uint8_t* dst = px.data() + (size_t)row * w * 3;
        for (int x = 0; x < w; x++) {
            dst[(size_t)x * 3 + 0] = src[(size_t)x * 3 + 2];
            dst[(size_t)x * 3 + 1] = src[(size_t)x * 3 + 1];
            dst[(size_t)x * 3 + 2] = src[(size_t)x * 3 + 0];
        }
    }
    return px;
}

struct MythColor { uint8_t red, fr, green, fg, blue, fb, flag, ff; };
struct Myth256Palette { int32_t colors; int32_t unk[7]; MythColor color[256]; };
static_assert(sizeof(Myth256Palette) == 2080, "palette size");
struct SecEntry { long absOffset; int32_t length; };

static bool readDot256FromData(const std::vector<uint8_t>& data,
                               std::vector<SecEntry>& secs, Myth256Palette& pal) {
    const size_t HDR = 320, SECSZ = 128, TEXHDR = 52;
    if (data.size() < HDR + sizeof(Myth256Palette)) return false;
    const uint8_t* hdr = data.data();
    int32_t palOff = readBE32s(hdr, 68);
    int32_t sections = readBE32s(hdr, 96);
    int32_t secOff = readBE32s(hdr, 100);
    if (sections < 1 || sections > 4000) return false;
    if ((size_t)(HDR + palOff + sizeof(Myth256Palette)) > data.size()) return false;
    memcpy(&pal, data.data() + HDR + palOff, sizeof(Myth256Palette));
    secs.resize((size_t)sections);
    size_t stPos = HDR + (size_t)secOff;
    for (int i = 0; i < sections; i++) {
        size_t ePos = stPos + (size_t)i * SECSZ;
        if (ePos + SECSZ > data.size()) return false;
        secs[(size_t)i].absOffset = (long)(HDR + (long)readBE32s(data.data(), ePos + 64) + (long)TEXHDR);
        secs[(size_t)i].length = readBE32s(data.data(), ePos + 68);
    }
    return true;
}

static bool injectTerrainTiles(std::vector<uint8_t>& terrain,
                               const std::vector<uint8_t>& bmp, int bW, int bH,
                               const std::vector<uint8_t>& bmpRaw,
                               int meshW, int meshH, int sectionParity,
                               bool updatePalette) {
    const size_t HDR = 320, SECSZ = 128, TEXHDR = 52;
    if (terrain.size() < HDR + sizeof(Myth256Palette)) return false;
    int32_t palOff = readBE32s(terrain.data(), 68);
    int32_t sections = readBE32s(terrain.data(), 96);
    int32_t secOff = readBE32s(terrain.data(), 100);
    if (sectionParity < 0 || sectionParity > 1) return false;
    if (sections != meshW * meshH * 2) return false;

    size_t palBase = HDR + (size_t)palOff;
    size_t colorBase = palBase + 32;
    if (colorBase + 256 * 8 > terrain.size()) return false;

    if (updatePalette && bmpRaw.size() >= 54) {
        copyBMPPaletteToMyth256(terrain.data() + colorBase, bmpRaw);
    }

    std::vector<uint32_t> relOfs((size_t)sections);
    for (int i = 0; i < sections; i++) {
        size_t eStart = HDR + (size_t)secOff + (size_t)i * SECSZ;
        if (eStart + 68 + 4 > terrain.size()) return false;
        relOfs[(size_t)i] = (uint32_t)readBE32s(terrain.data(), eStart + 64);
    }

    for (int r = 0; r < meshH; r++) {
        for (int c = 0; c < meshW; c++) {
            int idx = 2 * (r * meshW + (meshW - 1 - c)) + sectionParity;
            size_t pixStart = HDR + relOfs[(size_t)idx] + TEXHDR;
            if (pixStart + 256 * 256 > terrain.size()) return false;
            for (int line = 0; line < 256; line++) {
                const uint8_t* src = bmp.data() + (size_t)(r * 256 + line) * bW + c * 256;
                uint8_t* dst = terrain.data() + pixStart + (size_t)line * 256;
                for (int x = 0; x < 256; x++) dst[x] = src[255 - x];
            }
        }
    }
    return true;
}

static bool injectTerrainColor(std::vector<uint8_t>& terrain,
                               const std::vector<uint8_t>& bmp, int bW, int bH,
                               const std::vector<uint8_t>& bmpRaw,
                               int meshW, int meshH) {
    return injectTerrainTiles(terrain, bmp, bW, bH, bmpRaw, meshW, meshH, 0, true);
}

static bool injectTerrainShadow(std::vector<uint8_t>& terrain,
                                const std::vector<uint8_t>& bmp, int bW, int bH,
                                const std::vector<uint8_t>& bmpRaw,
                                int meshW, int meshH) {
    return injectTerrainTiles(terrain, bmp, bW, bH, bmpRaw, meshW, meshH, 1, false);
}

static bool injectSingleImage256(std::vector<uint8_t>& data, const std::vector<uint8_t>& bmpRaw,
                                 const std::vector<uint8_t>& bmp, int bW, int bH) {
    const size_t HDR = 320, BITMAP_REF_SIZE = 128, BITMAP_HEADER = 48;
    if (data.size() < HDR + sizeof(Myth256Palette)) return false;
    int32_t bulkOff = readBE32s(data.data(), 248);
    int32_t palOff = readBE32s(data.data(), 68);
    int32_t bitmapCount = readBE32s(data.data(), 96);
    int32_t bitmapRefsOff = readBE32s(data.data(), 100);
    if (bulkOff < 0 || palOff < 0 || bitmapRefsOff < 0 || bitmapCount < 1 || bitmapCount > 4096) return false;

    int w = 0, h = 0;
    long pixDataOff = -1;
    long imgAbs = -1;
    long imgLen = -1;
    size_t refBase = (size_t)bulkOff + (size_t)bitmapRefsOff;
    for (int bi = 0; bi < bitmapCount; bi++) {
        size_t ref = refBase + (size_t)bi * BITMAP_REF_SIZE;
        if (ref + BITMAP_REF_SIZE > data.size()) return false;
        int32_t imgDataOff = readBE32s(data.data(), ref + 64);
        int32_t imgDataLen = readBE32s(data.data(), ref + 68);
        int candidateW = (int)readBE16s(data.data(), ref + 76);
        int candidateH = (int)readBE16s(data.data(), ref + 78);
        long candidateImg = (long)bulkOff + (long)imgDataOff;
        long candidatePix = candidateImg + (long)BITMAP_HEADER + (long)candidateH * 4L;
        if (candidateW <= 0 || candidateW > 4096 || candidateH <= 0 || candidateH > 4096) continue;
        if (imgDataLen < (int32_t)(BITMAP_HEADER + candidateH * 4 + candidateW * candidateH)) continue;
        if (candidateImg < 0 || candidateImg + imgDataLen > (long)data.size()) continue;
        w = candidateW;
        h = candidateH;
        imgAbs = candidateImg;
        imgLen = imgDataLen;
        pixDataOff = candidatePix;
        break;
    }
    if (pixDataOff < 0) return false;
    if (bW != w || bH != h) return false;

    size_t colorBase = (size_t)bulkOff + (size_t)palOff + 32;
    if (colorBase + 256 * 8 > data.size()) return false;
    copyBMPPaletteToMyth256(data.data() + colorBase, bmpRaw);

    if (pixDataOff < 0 || pixDataOff + (long)w * h > (long)data.size()) return false;
    if (imgAbs < 0 || imgLen < (long)(BITMAP_HEADER + h * 4 + w * h)) return false;
    uint32_t firstRowOffset = (uint32_t)(BITMAP_HEADER + h * 4);
    for (int y = 0; y < h; y++) {
        writeBE32To(data.data() + (size_t)imgAbs + BITMAP_HEADER + (size_t)y * 4,
                    firstRowOffset + (uint32_t)y * (uint32_t)w);
    }
    memcpy(data.data() + pixDataOff, bmp.data(), (size_t)w * h);
    return true;
}

static int ceilLog2Int(int v) {
    if (v <= 1) return 0;
    int p = 0;
    int n = 1;
    while (n < v && p < 31) {
        n <<= 1;
        p++;
    }
    return p;
}

static std::vector<uint8_t> buildSingleImage256FromBMP(const std::string& bmpPath, const std::string& imageName) {
    int w = 0, h = 0;
    std::vector<uint8_t> bmpRaw = readFile(bmpPath);
    std::vector<uint8_t> bmp = readBMP8(bmpPath, w, h);
    if (bmp.empty() || w <= 0 || h <= 0 || w > 4096 || h > 4096) return {};

    const uint32_t HDR = 320;
    const uint32_t PAL_OFF = 0;
    const uint32_t PAL_LEN = (uint32_t)sizeof(Myth256Palette);
    const uint32_t INST_OFF = PAL_OFF + PAL_LEN;
    const uint32_t INST_LEN = 64;
    const uint32_t SEQ_OFF = INST_OFF + INST_LEN;
    const uint32_t SEQ_LEN = 128;
    const uint32_t AUX_OFF = SEQ_OFF + SEQ_LEN + 112;
    const uint32_t AUX_LEN = 32;
    const uint32_t REF_OFF = AUX_OFF + AUX_LEN;
    const uint32_t REF_LEN = 128;
    const uint32_t IMG_OFF = REF_OFF + REF_LEN;
    const uint32_t BITMAP_HEADER = 48;
    uint32_t imgLen = BITMAP_HEADER + (uint32_t)h * 4u + (uint32_t)w * (uint32_t)h;
    uint32_t bulkLen = IMG_OFF + imgLen;

    std::vector<uint8_t> out((size_t)HDR + bulkLen, 0);
    writeBE32To(out.data() + 4, 32);
    writeBE32To(out.data() + 64, 1);
    writeBE32To(out.data() + 68, PAL_OFF);
    writeBE32To(out.data() + 72, PAL_LEN);
    writeBE32To(out.data() + 84, PAL_LEN);
    writeBE32To(out.data() + 96, 1);
    writeBE32To(out.data() + 100, REF_OFF);
    writeBE32To(out.data() + 104, REF_LEN);
    writeBE32To(out.data() + 112, 1);
    writeBE32To(out.data() + 116, INST_OFF);
    writeBE32To(out.data() + 120, INST_LEN);
    writeBE32To(out.data() + 128, 1);
    writeBE32To(out.data() + 132, SEQ_OFF);
    writeBE32To(out.data() + 136, SEQ_LEN);
    writeBE32To(out.data() + 144, 1);
    writeBE32To(out.data() + 148, AUX_OFF);
    writeBE32To(out.data() + 152, AUX_LEN);
    writeBE32To(out.data() + 248, HDR);
    writeBE32To(out.data() + 252, bulkLen);

    uint8_t* bulk = out.data() + HDR;
    writeBE32To(bulk + PAL_OFF, 256);
    if (!copyBMPPaletteToMyth256(bulk + PAL_OFF + 32, bmpRaw)) return {};

    uint8_t* inst = bulk + INST_OFF;
    writeBE32To(inst + 0, 16);
    writeBE16To(inst + 6, 0xFFFFu);
    writeBE16To(inst + 12, (uint16_t)(w / 2));
    writeBE16To(inst + 14, (uint16_t)(h - 1));

    uint8_t* seq = bulk + SEQ_OFF;
    std::string seqName = imageName.empty() ? "screen" : imageName;
    memcpy(seq, seqName.c_str(), std::min<size_t>(seqName.size(), 63));
    writeBE16To(seq + 68, 1);

    uint8_t* ref = bulk + REF_OFF;
    char name[64];
    std::snprintf(name, sizeof(name), "bitmap #0 (#%d,#%d)", w, h);
    memcpy(ref, name, std::min<size_t>(strlen(name), 63));
    writeBE32To(ref + 64, IMG_OFF);
    writeBE32To(ref + 68, imgLen);
    writeBE32To(ref + 72, BITMAP_HEADER + (uint32_t)h * 4u);
    writeBE16To(ref + 76, (uint16_t)w);
    writeBE16To(ref + 78, (uint16_t)h);

    uint8_t* img = bulk + IMG_OFF;
    writeBE16To(img + 0, (uint16_t)w);
    writeBE16To(img + 2, (uint16_t)h);
    writeBE16To(img + 4, (uint16_t)w);
    writeBE16To(img + 8, 8);
    writeBE16To(img + 28, (uint16_t)ceilLog2Int(w));
    writeBE16To(img + 30, (uint16_t)ceilLog2Int(h));
    writeBE16To(img + 32, (uint16_t)std::max(1, (w + 255) / 256));
    writeBE16To(img + 34, (uint16_t)std::max(1, (h + 127) / 128));
    uint32_t firstRowOffset = BITMAP_HEADER + (uint32_t)h * 4u;
    for (int y = 0; y < h; y++) {
        writeBE32To(img + BITMAP_HEADER + (size_t)y * 4,
                    firstRowOffset + (uint32_t)y * (uint32_t)w);
    }
    memcpy(img + firstRowOffset, bmp.data(), bmp.size());
    return out;
}

static bool parseOBJHeights(const std::string& path, float hs, int vertexW, int vertexH,
                            std::vector<int16_t>& heightsOut) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    float halfW = (float)(vertexW - 1) * 0.5f;
    float halfH = (float)(vertexH - 1) * 0.5f;
    int total = vertexW * vertexH;
    heightsOut.assign((size_t)total, 0);
    std::vector<uint8_t> filled((size_t)total, 0);
    char line[256];
    int mapped = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != 'v' || line[1] != ' ') continue;
        float x = 0, y = 0, z = 0;
        if (sscanf(line + 2, "%f %f %f", &x, &y, &z) != 3) continue;
        int col = (int)roundf(halfW - x);
        int row = (int)roundf(z + halfH);
        if (col < 0 || col >= vertexW || row < 0 || row >= vertexH) continue;
        float hf = y / hs;
        int16_t h = (hf < -32768.0f) ? -32768 : (hf > 32767.0f) ? 32767 : (int16_t)roundf(hf);
        int idx = row * vertexW + col;
        heightsOut[(size_t)idx] = h;
        filled[(size_t)idx] = 1;
        mapped++;
    }
    fclose(f);
    return mapped > 0;
}

static constexpr int MYTH2_NORMAL_FULL = 1024;
static constexpr int MYTH2_NORMAL_Q14_ONE = 0x4000;
static constexpr int MYTH2_NORMAL_TABLE_SIZE = 256;
static constexpr double MYTH2_WORLD_HEIGHT_SCALE = 1.0 / 512.0;

struct NormalVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct RuntimeNormalEntry {
    NormalVec3 n;
};

static NormalVec3 normalSub(const NormalVec3& a, const NormalVec3& b) {
    return NormalVec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

static NormalVec3 normalCross(const NormalVec3& a, const NormalVec3& b) {
    return NormalVec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static double normalDot(const NormalVec3& a, const NormalVec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double normalLength(const NormalVec3& v) {
    return std::sqrt(normalDot(v, v));
}

static NormalVec3 normalNormalized(const NormalVec3& v) {
    double len = normalLength(v);
    if (len < 1e-12) return NormalVec3{0.0, 1.0, 0.0};
    return NormalVec3{v.x / len, v.y / len, v.z / len};
}

static double normalClamp1(double v) {
    if (v < -1.0) return -1.0;
    if (v > 1.0) return 1.0;
    return v;
}

static double normalAngleDeg(const NormalVec3& a, const NormalVec3& b) {
    return std::acos(normalClamp1(normalDot(a, b))) * (180.0 / 3.14159265358979323846);
}

static int16_t normalQ14FromFloat(double v) {
    long n = lround(v * (double)MYTH2_NORMAL_Q14_ONE);
    if (n < -32768) n = -32768;
    if (n > 32767) n = 32767;
    return (int16_t)n;
}

static void buildRuntimeTrigTables(std::vector<int16_t>& sinQ14, std::vector<int16_t>& cosQ14) {
    sinQ14.resize(MYTH2_NORMAL_FULL);
    cosQ14.resize(MYTH2_NORMAL_FULL);
    const double twoPi = 6.28318530717958647692;
    for (int i = 0; i < MYTH2_NORMAL_FULL; i++) {
        double a = ((double)i * twoPi) / (double)MYTH2_NORMAL_FULL;
        sinQ14[(size_t)i] = normalQ14FromFloat(std::sin(a));
        cosQ14[(size_t)i] = normalQ14FromFloat(std::cos(a));
    }
    sinQ14[0]   = 0;                     cosQ14[0]   =  MYTH2_NORMAL_Q14_ONE;
    sinQ14[256] = MYTH2_NORMAL_Q14_ONE;  cosQ14[256] =  0;
    sinQ14[512] = 0;                     cosQ14[512] = -MYTH2_NORMAL_Q14_ONE;
    sinQ14[768] = -MYTH2_NORMAL_Q14_ONE; cosQ14[768] =  0;
}

static std::vector<RuntimeNormalEntry> buildRuntimeNormalTable() {
    std::vector<int16_t> sinQ14, cosQ14;
    buildRuntimeTrigTables(sinQ14, cosQ14);

    std::vector<RuntimeNormalEntry> out(MYTH2_NORMAL_TABLE_SIZE);
    int entry = 0;
    int local8 = 0x4020;
    while (entry < MYTH2_NORMAL_TABLE_SIZE) {
        unsigned pitchIdx = (unsigned)((local8 >> 6) & 0x3FF);
        int ringCount = (((int)cosQ14[pitchIdx] << 2) / 0x444) + 1;
        if (ringCount > 0) {
            int localC = 0x20;
            while (ringCount > 0 && entry < MYTH2_NORMAL_TABLE_SIZE) {
                unsigned azIdx = (unsigned)((localC >> 6) & 0x3FF);
                int16_t x = (int16_t)(((int)cosQ14[azIdx] * (int)cosQ14[pitchIdx]) >> 14);
                int16_t y = (int16_t)(((int)sinQ14[azIdx] * (int)cosQ14[pitchIdx]) >> 14);
                int16_t z = sinQ14[pitchIdx];
                out[(size_t)entry].n = normalNormalized(NormalVec3{
                    (double)x / (double)MYTH2_NORMAL_Q14_ONE,
                    (double)y / (double)MYTH2_NORMAL_Q14_ONE,
                    (double)z / (double)MYTH2_NORMAL_Q14_ONE
                });
                entry++;
                localC += (int)(0x10000 / (long long)((((int)cosQ14[pitchIdx] << 2) / 0x444) + 1));
                ringCount--;
            }
        }
        local8 -= 0x444;
    }
    return out;
}

static NormalVec3 empiricalToRuntimeNormal(const NormalVec3& v) {
    return normalNormalized(NormalVec3{-v.x, v.z, v.y});
}

static uint8_t nearestRuntimeNormalIndex(const NormalVec3& empiricalNormal,
                                         const std::vector<RuntimeNormalEntry>& table) {
    NormalVec3 runtimeNormal = empiricalToRuntimeNormal(empiricalNormal);
    double bestErr = std::numeric_limits<double>::infinity();
    int bestIdx = 0;
    for (int i = 0; i < MYTH2_NORMAL_TABLE_SIZE; i++) {
        double err = normalAngleDeg(runtimeNormal, table[(size_t)i].n);
        if (err < bestErr) {
            bestErr = err;
            bestIdx = i;
        }
    }
    return (uint8_t)bestIdx;
}

static bool preserveLegacyNormalWord(uint16_t flags, int16_t mediaHeight, uint16_t oldWord) {
    const int tri0Type = (flags >> 4) & 0x0F;
    const int tri1Type = flags & 0x0F;
    const bool tri0Wet = (flags & MYTH2_MESH_CELL_TRIANGLE0_IS_MEDIA_FLAG) != 0;
    const bool tri1Wet = (flags & MYTH2_MESH_CELL_TRIANGLE1_IS_MEDIA_FLAG) != 0;
    if (oldWord == 0) return true;
    if ((flags & MYTH2_MESH_CELL_IS_NOT_RENDERED_FLAG) != 0) return true;
    if (tri0Type == 13 || tri1Type == 13) return true;
    if (mediaHeight == (int16_t)-30981) return true;
    if ((tri0Type <= 3 && !tri0Wet) || (tri1Type <= 3 && !tri1Wet)) return true;
    return false;
}

static bool parseWaterOBJHeights(const std::string& path, float hs, int vertexW, int vertexH,
                                 std::vector<int16_t>& heightsOut) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    float halfW = (float)(vertexW - 1) * 0.5f;
    float halfH = (float)(vertexH - 1) * 0.5f;
    int total = vertexW * vertexH;
    heightsOut.assign((size_t)total, 0);
    std::vector<uint8_t> filled((size_t)total, 0);
    char line[256];
    int mapped = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != 'v' || line[1] != ' ') continue;
        float x = 0, y = 0, z = 0;
        if (sscanf(line + 2, "%f %f %f", &x, &y, &z) != 3) continue;
        int col = (int)roundf(halfW - x);
        int row = (int)roundf(z + halfH);
        if (col < 0 || col >= vertexW || row < 0 || row >= vertexH) continue;
        float hf = y / hs;
        int16_t h = (hf < -32768.0f) ? -32768 : (hf > 32767.0f) ? 32767 : (int16_t)roundf(hf);
        int idx = row * vertexW + col;
        heightsOut[(size_t)idx] = h;
        filled[(size_t)idx] = 1;
        mapped++;
    }
    fclose(f);
    return mapped > 0;
}

static bool applyObjToMyth2Mesh(std::vector<uint8_t>& meshData, int submeshW, int submeshH,
                                const std::string& objPath, float heightScale) {
    int cellW = submeshW * 32;
    int cellH = submeshH * 32;
    int vertexW = cellW + 1;
    int vertexH = cellH + 1;
    std::vector<int16_t> heights;
    if (!parseOBJHeights(objPath, heightScale, vertexW, vertexH, heights)) return false;
    int changed = 0;
    int normalChanged = 0;
    std::vector<RuntimeNormalEntry> runtimeTable = buildRuntimeNormalTable();
    std::vector<int16_t> originalVertexHeights((size_t)vertexW * vertexH, 0);

    for (int vy = 0; vy < vertexH; vy++) {
        for (int vx = 0; vx < vertexW; vx++) {
            int cx = vx;
            int cy = vy;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx >= cellW) cx = cellW - 1;
            if (cy >= cellH) cy = cellH - 1;
            size_t off = 1024 + (size_t)(cy * cellW + cx) * 12;
            originalVertexHeights[(size_t)vy * vertexW + vx] = readBE16s(meshData.data(), off);
        }
    }

    double halfW = (double)(vertexW - 1) * 0.5;
    double halfH = (double)cellH * 0.5;
    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            size_t off = 1024 + (size_t)(y * cellW + x) * 12;
            if (off + 1 >= meshData.size()) continue;
            int16_t oldH = readBE16s(meshData.data(), off);
            int16_t newH = heights[(size_t)y * vertexW + x];
            if (oldH != newH) {
                writeBE16To(meshData.data() + off, (uint16_t)newH);
                changed++;
            }

            int A = y * vertexW + x;
            int B = y * vertexW + (x + 1);
            int C = (y + 1) * vertexW + (x + 1);
            int D = (y + 1) * vertexW + x;
            bool cornersChanged =
                originalVertexHeights[(size_t)A] != heights[(size_t)A] ||
                originalVertexHeights[(size_t)B] != heights[(size_t)B] ||
                originalVertexHeights[(size_t)C] != heights[(size_t)C] ||
                originalVertexHeights[(size_t)D] != heights[(size_t)D];
            if (!cornersChanged) {
                continue;
            }
            NormalVec3 pA{halfW - (double)x, (double)heights[(size_t)A] * MYTH2_WORLD_HEIGHT_SCALE, (double)y - halfH};
            NormalVec3 pB{halfW - (double)(x + 1), (double)heights[(size_t)B] * MYTH2_WORLD_HEIGHT_SCALE, (double)y - halfH};
            NormalVec3 pC{halfW - (double)(x + 1), (double)heights[(size_t)C] * MYTH2_WORLD_HEIGHT_SCALE, (double)(y + 1) - halfH};
            NormalVec3 pD{halfW - (double)x, (double)heights[(size_t)D] * MYTH2_WORLD_HEIGHT_SCALE, (double)(y + 1) - halfH};

            NormalVec3 n0;
            NormalVec3 n1;
            if (((x ^ y) & 1) == 0) {
                n0 = normalNormalized(normalCross(normalSub(pB, pA), normalSub(pC, pA)));
                n1 = normalNormalized(normalCross(normalSub(pC, pA), normalSub(pD, pA)));
            } else {
                n0 = normalNormalized(normalCross(normalSub(pB, pA), normalSub(pD, pA)));
                n1 = normalNormalized(normalCross(normalSub(pC, pB), normalSub(pD, pB)));
            }

            // Myth II stores the even-parity cell triangles in the opposite
            // slot order from the natural A-B-C / A-C-D construction above.
            if (((x ^ y) & 1) == 0) {
                std::swap(n0, n1);
            }

            uint16_t oldWord = (uint16_t)readBE16s(meshData.data(), off + 2);
            uint16_t flags = (uint16_t)readBE16s(meshData.data(), off + 4);
            int16_t mediaHeight = readBE16s(meshData.data(), off + 8);
            uint16_t newWord = oldWord;
            if (!preserveLegacyNormalWord(flags, mediaHeight, oldWord)) {
                newWord = (uint16_t)((uint16_t)nearestRuntimeNormalIndex(n0, runtimeTable) << 8)
                        | (uint16_t)nearestRuntimeNormalIndex(n1, runtimeTable);
            }
            if (oldWord != newWord) {
                writeBE16To(meshData.data() + off + 2, newWord);
                normalChanged++;
            }
        }
    }
    printf("Applied OBJ displacement changes: %d\n", changed);
    printf("Regenerated OBJ normal words: %d\n", normalChanged);
    return true;
}

static bool applyWaterObjToMyth2Mesh(std::vector<uint8_t>& meshData, int submeshW, int submeshH,
                                     const std::string& objPath, float heightScale) {
    const uint16_t tri0MediaBit = (1u << 11);
    const uint16_t tri1MediaBit = (1u << 12);
    int cellW = submeshW * 32;
    int cellH = submeshH * 32;
    int vertexW = cellW + 1;
    int vertexH = cellH + 1;
    std::vector<int16_t> heights;
    if (!parseOBJHeights(objPath, heightScale, vertexW, vertexH, heights)) return false;

    int changed = 0;
    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            size_t cellOff = 1024 + (size_t)(y * cellW + x) * 12;
            uint16_t flags = (uint16_t)readBE16s(meshData.data(), cellOff + 4);
            if ((flags & (tri0MediaBit | tri1MediaBit)) == 0) continue;

            size_t off = cellOff + 8;
            if (off + 1 >= meshData.size()) continue;
            int16_t oldH = readBE16s(meshData.data(), off);
            int16_t newH = heights[(size_t)y * vertexW + x];
            if (oldH != newH) {
                writeBE16To(meshData.data() + off, (uint16_t)newH);
                changed++;
            }
        }
    }

    printf("Applied water OBJ media-height changes: %d\n", changed);
    return true;
}

static const uint8_t TERRAIN_TYPE_COLORS[16][3] = {
    {0x00,0x00,0xFF}, // media dwarf
    {0x00,0x00,0xB0}, // media human
    {0x00,0x00,0x80}, // media giant
    {0x00,0x00,0x40}, // media deep
    {0x80,0x00,0x00}, // sloped
    {0xFF,0x00,0x00}, // steep
    {0x00,0x00,0x00}, // grass
    {0xFF,0xFF,0x00}, // desert
    {0x20,0x20,0x20}, // rocky
    {0x80,0x40,0x00}, // marsh
    {0xA0,0xA0,0xA0}, // snow
    {0x20,0x70,0x20}, // forest
    {0xFF,0x00,0xFF}, // loathing special
    {0x00,0x00,0x00}, // unused
    {0x00,0x80,0x00}, // walking impassable
    {0x00,0xFF,0x00}  // flying impassable
};

static size_t myth2CellOffset(int cellW, int x, int y) {
    return 1024u + (size_t)(y * cellW + x) * 12u;
}

static uint16_t readCellFlags(const std::vector<uint8_t>& meshData, int cellW, int x, int y) {
    return (uint16_t)readBE16s(meshData.data(), myth2CellOffset(cellW, x, y) + 4);
}

static void writeCellFlags(std::vector<uint8_t>& meshData, int cellW, int x, int y, uint16_t flags) {
    writeBE16To(meshData.data() + myth2CellOffset(cellW, x, y) + 4, flags);
}

static int16_t readCellPhysicalHeight(const std::vector<uint8_t>& meshData, int cellW, int x, int y) {
    return readBE16s(meshData.data(), myth2CellOffset(cellW, x, y));
}

static int16_t readCellMediaHeight(const std::vector<uint8_t>& meshData, int cellW, int x, int y) {
    return readBE16s(meshData.data(), myth2CellOffset(cellW, x, y) + 8);
}

static void writeCellMediaHeight(std::vector<uint8_t>& meshData, int cellW, int x, int y, int16_t mediaHeight) {
    writeBE16To(meshData.data() + myth2CellOffset(cellW, x, y) + 8, (uint16_t)mediaHeight);
}

static int getCellTriType(uint16_t flags, int which) {
    return which ? (flags & 0x0F) : ((flags >> 4) & 0x0F);
}

static uint16_t setCellTriType(uint16_t flags, int which, int type) {
    type &= 0x0F;
    if (which) return (uint16_t)((flags & 0xFFF0u) | (uint16_t)type);
    return (uint16_t)((flags & 0xFF0Fu) | (uint16_t)(type << 4));
}

static bool isCellTriMedia(uint16_t flags, int which) {
    return (flags & (which ? MYTH2_MESH_CELL_TRIANGLE1_IS_MEDIA_FLAG : MYTH2_MESH_CELL_TRIANGLE0_IS_MEDIA_FLAG)) != 0;
}

static uint16_t setCellTriMedia(uint16_t flags, int which, bool wet) {
    uint16_t bit = which ? MYTH2_MESH_CELL_TRIANGLE1_IS_MEDIA_FLAG : MYTH2_MESH_CELL_TRIANGLE0_IS_MEDIA_FLAG;
    return wet ? (uint16_t)(flags | bit) : (uint16_t)(flags & ~bit);
}

static int nearestTerrainType(const uint8_t* rgb, int count) {
    int best = 0;
    int bestDist = INT32_MAX;
    for (int i = 0; i < count; i++) {
        int dr = (int)rgb[0] - (int)TERRAIN_TYPE_COLORS[i][0];
        int dg = (int)rgb[1] - (int)TERRAIN_TYPE_COLORS[i][1];
        int db = (int)rgb[2] - (int)TERRAIN_TYPE_COLORS[i][2];
        int dist = dr * dr + dg * dg + db * db;
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

static void sampleTriangleCenters(int x, int y, int& tri0x, int& tri0y, int& tri1x, int& tri1y) {
    if (((x ^ y) & 1) != 0) {
        tri0x = 2; tri0y = 2;
        tri1x = 5; tri1y = 5;
    } else {
        tri0x = 2; tri0y = 5;
        tri1x = 5; tri1y = 2;
    }
}

static size_t meshMapSampleOffset(int bmpW, int cellW, int x, int y, int localX, int localY) {
    int mirroredX = (cellW - 1 - x) * 8 + (7 - localX);
    int pixelY = y * 8 + localY;
    return ((size_t)pixelY * bmpW + mirroredX) * 3;
}

static size_t meshMapSampleIndexOffset(int bmpW, int cellW, int x, int y, int localX, int localY) {
    int mirroredX = (cellW - 1 - x) * 8 + (7 - localX);
    int pixelY = y * 8 + localY;
    return (size_t)pixelY * bmpW + mirroredX;
}

static int16_t estimateMediaHeight(const std::vector<uint8_t>& meshData, int cellW, int cellH, int x, int y) {
    int16_t maxHeight = readCellPhysicalHeight(meshData, cellW, x, y);
    const int dx[4] = { 0, 1, 0, 1 };
    const int dy[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) {
        int sx = std::min(std::max(x + dx[i], 0), cellW - 1);
        int sy = std::min(std::max(y + dy[i], 0), cellH - 1);
        int16_t h = readCellPhysicalHeight(meshData, cellW, sx, sy);
        if (h > maxHeight) maxHeight = h;
    }
    return maxHeight;
}

static void recomputeVertexMediaFlags(std::vector<uint8_t>& meshData, int cellW, int cellH) {
    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            uint16_t flags = readCellFlags(meshData, cellW, x, y);
            flags = (uint16_t)(flags & ~MYTH2_MESH_VERTEX_IS_MEDIA_FLAG);

            bool wet = false;
            for (int oy = -1; oy <= 0 && !wet; oy++) {
                for (int ox = -1; ox <= 0 && !wet; ox++) {
                    int cx = x + ox;
                    int cy = y + oy;
                    if (cx < 0 || cy < 0 || cx >= cellW || cy >= cellH) continue;
                    uint16_t neighborFlags = readCellFlags(meshData, cellW, cx, cy);
                    wet = wet || isCellTriMedia(neighborFlags, 0) || isCellTriMedia(neighborFlags, 1);
                }
            }

            if (wet) flags = (uint16_t)(flags | MYTH2_MESH_VERTEX_IS_MEDIA_FLAG);
            writeCellFlags(meshData, cellW, x, y, flags);
        }
    }
}

static void flattenMediaRegions(std::vector<uint8_t>& meshData, int cellW, int cellH) {
    std::vector<uint8_t> seen((size_t)cellW * cellH, 0);
    std::vector<int> queue;
    queue.reserve((size_t)cellW * cellH);

    for (int sy = 0; sy < cellH; sy++) {
        for (int sx = 0; sx < cellW; sx++) {
            size_t startIdx = (size_t)sy * cellW + sx;
            if (seen[startIdx]) continue;
            uint16_t startFlags = readCellFlags(meshData, cellW, sx, sy);
            if (!isCellTriMedia(startFlags, 0) && !isCellTriMedia(startFlags, 1)) continue;

            seen[startIdx] = 1;
            queue.clear();
            queue.push_back(sx);
            queue.push_back(sy);

            int16_t targetHeight = estimateMediaHeight(meshData, cellW, cellH, sx, sy);
            std::vector<int> cells;

            for (size_t q = 0; q < queue.size(); q += 2) {
                int x = queue[q + 0];
                int y = queue[q + 1];
                cells.push_back(x);
                cells.push_back(y);

                int16_t existingMedia = readCellMediaHeight(meshData, cellW, x, y);
                int16_t estimated = estimateMediaHeight(meshData, cellW, cellH, x, y);
                if (existingMedia > targetHeight) targetHeight = existingMedia;
                if (estimated > targetHeight) targetHeight = estimated;

                const int nx[4] = { x + 1, x - 1, x, x };
                const int ny[4] = { y, y, y + 1, y - 1 };
                for (int i = 0; i < 4; i++) {
                    int xx = nx[i];
                    int yy = ny[i];
                    if (xx < 0 || yy < 0 || xx >= cellW || yy >= cellH) continue;
                    size_t ni = (size_t)yy * cellW + xx;
                    if (seen[ni]) continue;
                    uint16_t nf = readCellFlags(meshData, cellW, xx, yy);
                    if (!isCellTriMedia(nf, 0) && !isCellTriMedia(nf, 1)) continue;
                    seen[ni] = 1;
                    queue.push_back(xx);
                    queue.push_back(yy);
                }
            }

            for (size_t i = 0; i < cells.size(); i += 2) {
                int x = cells[i + 0];
                int y = cells[i + 1];
                for (int dy = 0; dy <= 1; dy++) {
                    for (int dx = 0; dx <= 1; dx++) {
                        int vx = x + dx;
                        int vy = y + dy;
                        if (vx < 0 || vy < 0 || vx >= cellW || vy >= cellH) continue;
                        writeCellMediaHeight(meshData, cellW, vx, vy, targetHeight);
                    }
                }
            }
        }
    }

    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            uint16_t flags = readCellFlags(meshData, cellW, x, y);
            if (!isCellTriMedia(flags, 0) && !isCellTriMedia(flags, 1)) {
                flags = (uint16_t)(flags & ~MYTH2_MESH_VERTEX_IS_ANIMATED_MEDIA_FLAG);
                flags = (uint16_t)(flags & ~MYTH2_MESH_CELL_HAS_REFLECTION_FLAG);
                writeCellFlags(meshData, cellW, x, y, flags);
            }
        }
    }
}

static bool applyAnimationToMyth2Mesh(std::vector<uint8_t>& meshData, int submeshW, int submeshH,
                                      const std::string& bmpPath) {
    int bW = 0, bH = 0;
    auto indexed = readBMPIndexed4Or8(bmpPath, bW, bH);
    bool exactIndexed = !indexed.empty();
    std::vector<uint8_t> bmp;
    if (!exactIndexed) bmp = readBMP24(bmpPath, bW, bH);
    int cellW = submeshW * 32;
    int cellH = submeshH * 32;
    if (!exactIndexed && bmp.empty()) return false;
    if (bW != cellW * 8 || bH != cellH * 8) {
        fprintf(stderr, "Animation map has wrong size: %s\n", bmpPath.c_str());
        return false;
    }

    int changed = 0;
    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            bool animated = false;
            if (exactIndexed) {
                size_t pc = meshMapSampleIndexOffset(bW, cellW, x, y, 4, 4);
                animated = indexed[pc] != 0;
            } else {
                size_t pc = meshMapSampleOffset(bW, cellW, x, y, 4, 4);
                animated = ((int)bmp[pc + 0] + (int)bmp[pc + 1] + (int)bmp[pc + 2]) > 384;
            }
            uint16_t flags = readCellFlags(meshData, cellW, x, y);
            bool wet = isCellTriMedia(flags, 0) || isCellTriMedia(flags, 1);
            uint16_t newFlags = animated && wet
                ? (uint16_t)(flags | MYTH2_MESH_VERTEX_IS_ANIMATED_MEDIA_FLAG)
                : (uint16_t)(flags & ~MYTH2_MESH_VERTEX_IS_ANIMATED_MEDIA_FLAG);
            if (newFlags != flags) {
                writeCellFlags(meshData, cellW, x, y, newFlags);
                changed++;
            }
        }
    }

    printf("Applied terrain/animation.bmp: %d cells changed\n", changed);
    return true;
}

static bool applyReflectionToMyth2Mesh(std::vector<uint8_t>& meshData, int submeshW, int submeshH,
                                       const std::string& bmpPath) {
    int bW = 0, bH = 0;
    auto indexed = readBMPIndexed4Or8(bmpPath, bW, bH);
    bool exactIndexed = !indexed.empty();
    std::vector<uint8_t> bmp;
    if (!exactIndexed) bmp = readBMP24(bmpPath, bW, bH);
    int cellW = submeshW * 32;
    int cellH = submeshH * 32;
    if (!exactIndexed && bmp.empty()) return false;
    if (bW != cellW * 8 || bH != cellH * 8) {
        fprintf(stderr, "Reflection map has wrong size: %s\n", bmpPath.c_str());
        return false;
    }

    int changed = 0;
    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            bool reflective = false;
            if (exactIndexed) {
                size_t pc = meshMapSampleIndexOffset(bW, cellW, x, y, 4, 4);
                reflective = indexed[pc] != 0;
            } else {
                size_t pc = meshMapSampleOffset(bW, cellW, x, y, 4, 4);
                reflective = ((int)bmp[pc + 0] + (int)bmp[pc + 1] + (int)bmp[pc + 2]) > 24;
            }
            uint16_t flags = readCellFlags(meshData, cellW, x, y);
            uint16_t newFlags = reflective
                ? (uint16_t)(flags | MYTH2_MESH_CELL_HAS_REFLECTION_FLAG)
                : (uint16_t)(flags & ~MYTH2_MESH_CELL_HAS_REFLECTION_FLAG);
            if (newFlags != flags) {
                writeCellFlags(meshData, cellW, x, y, newFlags);
                changed++;
            }
        }
    }

    printf("Applied terrain/reflection.bmp: %d cells changed\n", changed);
    return true;
}

static bool applyPassabilityToMyth2Mesh(std::vector<uint8_t>& meshData, int submeshW, int submeshH,
                                        const std::string& bmpPath) {
    int bW = 0, bH = 0;
    auto indexed = readBMPIndexed4Or8(bmpPath, bW, bH);
    bool exactIndexed = !indexed.empty();
    std::vector<uint8_t> bmp;
    if (!exactIndexed) bmp = readBMP24(bmpPath, bW, bH);
    int cellW = submeshW * 32;
    int cellH = submeshH * 32;
    if (!exactIndexed && bmp.empty()) return false;
    if (bW != cellW * 8 || bH != cellH * 8) {
        fprintf(stderr, "Passability map has wrong size: %s\n", bmpPath.c_str());
        return false;
    }

    int changed = 0;
    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            int tri0x, tri0y, tri1x, tri1y;
            sampleTriangleCenters(x, y, tri0x, tri0y, tri1x, tri1y);
            int t0 = 0, t1 = 0;
            if (exactIndexed) {
                size_t p0 = meshMapSampleIndexOffset(bW, cellW, x, y, tri0x, tri0y);
                size_t p1 = meshMapSampleIndexOffset(bW, cellW, x, y, tri1x, tri1y);
                t0 = indexed[p0] & 0x0F;
                t1 = indexed[p1] & 0x0F;
            } else {
                size_t p0 = meshMapSampleOffset(bW, cellW, x, y, tri0x, tri0y);
                size_t p1 = meshMapSampleOffset(bW, cellW, x, y, tri1x, tri1y);
                t0 = nearestTerrainType(bmp.data() + p0, 16);
                t1 = nearestTerrainType(bmp.data() + p1, 16);
            }
            uint16_t flags = readCellFlags(meshData, cellW, x, y);
            uint16_t newFlags = setCellTriType(setCellTriType(flags, 0, t0), 1, t1);
            if (newFlags != flags) {
                writeCellFlags(meshData, cellW, x, y, newFlags);
                changed++;
            }
        }
    }

    printf("Applied terrain/passability.bmp: %d cells changed\n", changed);
    return true;
}

static bool applyWaterToMyth2MeshImpl(std::vector<uint8_t>& meshData, int submeshW, int submeshH,
                                      const std::string& bmpPath, bool updateHeights) {
    int bW = 0, bH = 0;
    auto indexed = readBMPIndexed4Or8(bmpPath, bW, bH);
    bool exactIndexed = !indexed.empty();
    std::vector<uint8_t> bmp;
    if (!exactIndexed) bmp = readBMP24(bmpPath, bW, bH);
    int cellW = submeshW * 32;
    int cellH = submeshH * 32;
    if (!exactIndexed && bmp.empty()) return false;
    if (bW != cellW * 8 || bH != cellH * 8) {
        fprintf(stderr, "Water map has wrong size: %s\n", bmpPath.c_str());
        return false;
    }

    int changed = 0;
    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            int tri0x, tri0y, tri1x, tri1y;
            sampleTriangleCenters(x, y, tri0x, tri0y, tri1x, tri1y);
            bool wet0 = false, wet1 = false;
            int type0 = 0, type1 = 0;
            if (exactIndexed) {
                size_t p0 = meshMapSampleIndexOffset(bW, cellW, x, y, tri0x, tri0y);
                size_t p1 = meshMapSampleIndexOffset(bW, cellW, x, y, tri1x, tri1y);
                type0 = indexed[p0] & 0x0F;
                type1 = indexed[p1] & 0x0F;
                wet0 = type0 < 4;
                wet1 = type1 < 4;
            } else {
                size_t p0 = meshMapSampleOffset(bW, cellW, x, y, tri0x, tri0y);
                size_t p1 = meshMapSampleOffset(bW, cellW, x, y, tri1x, tri1y);
                wet0 = ((int)bmp[p0 + 0] + (int)bmp[p0 + 1] + (int)bmp[p0 + 2]) > 24;
                wet1 = ((int)bmp[p1 + 0] + (int)bmp[p1 + 1] + (int)bmp[p1 + 2]) > 24;
                type0 = nearestTerrainType(bmp.data() + p0, 4);
                type1 = nearestTerrainType(bmp.data() + p1, 4);
            }

            uint16_t flags = readCellFlags(meshData, cellW, x, y);
            uint16_t oldFlags = flags;
            bool oldWet0 = isCellTriMedia(flags, 0);
            bool oldWet1 = isCellTriMedia(flags, 1);

            flags = setCellTriMedia(flags, 0, wet0);
            flags = setCellTriMedia(flags, 1, wet1);

            if (wet0) flags = setCellTriType(flags, 0, type0);
            else if (oldWet0 && getCellTriType(flags, 0) < 4) flags = setCellTriType(flags, 0, 6);

            if (wet1) flags = setCellTriType(flags, 1, type1);
            else if (oldWet1 && getCellTriType(flags, 1) < 4) flags = setCellTriType(flags, 1, 6);

            bool nowWet = wet0 || wet1;
            bool wasWet = oldWet0 || oldWet1;
            if (updateHeights && !nowWet) {
                flags = (uint16_t)(flags & ~MYTH2_MESH_VERTEX_IS_ANIMATED_MEDIA_FLAG);
                flags = (uint16_t)(flags & ~MYTH2_MESH_CELL_HAS_REFLECTION_FLAG);
            }
            if (updateHeights && nowWet && !wasWet) {
                writeCellMediaHeight(meshData, cellW, x, y, estimateMediaHeight(meshData, cellW, cellH, x, y));
            }

            if (flags != oldFlags) {
                writeCellFlags(meshData, cellW, x, y, flags);
                changed++;
            }
        }
    }

    if (updateHeights) {
        flattenMediaRegions(meshData, cellW, cellH);
        recomputeVertexMediaFlags(meshData, cellW, cellH);
    }
    printf("Applied terrain/water.bmp%s: %d cells changed\n",
           updateHeights ? "" : " (flags only)", changed);
    return true;
}

static bool applyWaterToMyth2Mesh(std::vector<uint8_t>& meshData, int submeshW, int submeshH,
                                  const std::string& bmpPath) {
    return applyWaterToMyth2MeshImpl(meshData, submeshW, submeshH, bmpPath, true);
}

static bool applyWaterFlagsToMyth2Mesh(std::vector<uint8_t>& meshData, int submeshW, int submeshH,
                                       const std::string& bmpPath) {
    return applyWaterToMyth2MeshImpl(meshData, submeshW, submeshH, bmpPath, false);
}

static int jsonInt(const std::string& j, const std::string& key, int def = 0) {
    for (const char* sep : {"\": ", "\":"}) {
        std::string needle = "\"" + key + sep;
        size_t pos = j.find(needle);
        if (pos == std::string::npos) continue;
        pos += needle.size();
        while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) pos++;
        if (pos >= j.size()) continue;
        bool neg = (j[pos] == '-');
        if (neg) pos++;
        int val = 0;
        bool got = false;
        while (pos < j.size() && j[pos] >= '0' && j[pos] <= '9') {
            val = val * 10 + (j[pos] - '0');
            pos++;
            got = true;
        }
        if (got) return neg ? -val : val;
    }
    return def;
}

static std::string jsonString(const std::string& j, const std::string& key, const std::string& def = "") {
    std::string needle = "\"" + key + "\":";
    size_t pos = j.find(needle);
    if (pos == std::string::npos) return def;
    pos += needle.size();
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) pos++;
    if (pos >= j.size() || j[pos] != '"') return def;
    pos++;
    std::string out;
    while (pos < j.size()) {
        char c = j[pos++];
        if (c == '"') return out;
        if (c == '\\' && pos < j.size()) {
            char e = j[pos++];
            switch (e) {
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(e); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return def;
}

static std::string dirNameOf(const std::string& path) {
    std::string p = path;
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

static uint32_t tagFromString(const std::string& s) {
    if (s.size() != 4) return 0;
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16)
         | ((uint32_t)(uint8_t)s[2] << 8)  | (uint32_t)(uint8_t)s[3];
}

static std::string tagToString(uint32_t tag) {
    char s[5];
    s[0] = (char)((tag >> 24) & 0xFF);
    s[1] = (char)((tag >> 16) & 0xFF);
    s[2] = (char)((tag >> 8) & 0xFF);
    s[3] = (char)(tag & 0xFF);
    s[4] = 0;
    return std::string(s, 4);
}

static bool isNullTagString(const std::string& s) {
    return s.empty() || s == "null";
}

static std::string textToStli(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '\r') {
            out.push_back('\r');
            if (i + 1 < text.size() && text[i + 1] == '\n') i++;
        } else if (c == '\n') {
            out.push_back('\r');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

struct Manifest {
    std::string meshTag;
    std::string mapNameTag;
    std::string landscapeTag;
    std::string pregameTag;
    std::string overheadTag;
    std::string postgameTag;
    int submeshWidth = 0;
    int submeshHeight = 0;
};

struct UnitPlacementEdit {
    int markerIdx = -1;
    int sourceMarkerIdx = -1;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double facingDeg = 0.0;
    bool hasFacing = false;
    bool add = false;
};

struct MarkerPlacementEdit {
    int markerIdx = -1;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double facingDeg = 0.0;
    bool hasFacing = false;
};

static bool jsonNumberNear(const std::string& obj, const std::string& key, double& out) {
    std::string needle = "\"" + key + "\"";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return false;
    p = obj.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < obj.size() && std::isspace((unsigned char)obj[p])) p++;
    char* end = nullptr;
    out = std::strtod(obj.c_str() + p, &end);
    return end && end != obj.c_str() + p;
}

static bool jsonIntNear(const std::string& obj, const std::string& key, int& out) {
    double v = 0.0;
    if (!jsonNumberNear(obj, key, v)) return false;
    out = (int)std::lround(v);
    return true;
}

static bool jsonBoolNear(const std::string& obj, const std::string& key, bool& out) {
    std::string needle = "\"" + key + "\"";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return false;
    p = obj.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < obj.size() && std::isspace((unsigned char)obj[p])) p++;
    if (obj.compare(p, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (obj.compare(p, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

static std::vector<std::string> jsonObjectsInArray(const std::string& j, const std::string& key) {
    std::vector<std::string> objects;
    std::string needle = "\"" + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return objects;
    p = j.find('[', p + needle.size());
    if (p == std::string::npos) return objects;

    bool inString = false;
    bool escape = false;
    int arrayDepth = 0;
    int objectDepth = 0;
    size_t objectStart = std::string::npos;
    for (; p < j.size(); p++) {
        char c = j[p];
        if (escape) {
            escape = false;
            continue;
        }
        if (inString) {
            if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '[') {
            arrayDepth++;
        } else if (c == ']') {
            arrayDepth--;
            if (arrayDepth == 0) break;
        } else if (c == '{') {
            if (arrayDepth == 1 && objectDepth == 0) objectStart = p;
            objectDepth++;
        } else if (c == '}') {
            if (objectDepth > 0) objectDepth--;
            if (arrayDepth == 1 && objectDepth == 0 && objectStart != std::string::npos) {
                objects.push_back(j.substr(objectStart, p - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }
    return objects;
}

static std::vector<std::string> jsonObjectsWithMarkerIdx(const std::string& j) {
    std::vector<std::string> objects;
    size_t p = 0;
    while ((p = j.find("\"marker_idx\"", p)) != std::string::npos) {
        size_t objStart = j.rfind('{', p);
        size_t objEnd = j.find('}', p);
        if (objStart != std::string::npos && objEnd != std::string::npos && objEnd > objStart) {
            objects.push_back(j.substr(objStart, objEnd - objStart + 1));
            p = objEnd + 1;
        } else {
            p += 12;
        }
    }
    return objects;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parseHexBytes(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    if (s.size() % 2 != 0) return false;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = hexValue(s[i]);
        int lo = hexValue(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

static size_t alignToSize(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

static size_t actionParameterValueBytes(uint16_t type, uint16_t count) {
    switch (type) {
    case 1:  // string
    case 0:  // flag
        return alignToSize(count, 4);
    case 7:  // field name
    case 9:  // projectile
    case 10: // string list
    case 11: // sound
    case 12: // projectile or world point 2d
        return (size_t)count * 4;
    case 13: // world point 2d
        return (size_t)count * 8;
    case 18: // world point 3d
        return (size_t)count * 12;
    case 5:  // integer
    case 6:  // world distance
    case 8:  // fixed
        return (size_t)count * 4;
    default:
        return alignToSize(count, 2) * 2;
    }
}

static bool countActionParameters(const std::vector<uint8_t>& paramData, uint16_t& countOut) {
    size_t p = 0;
    uint32_t count = 0;
    while (p < paramData.size()) {
        if (p + 8 > paramData.size()) return false;
        uint16_t type = (uint16_t)readBE16s(paramData.data(), p + 0);
        uint16_t countItems = (uint16_t)readBE16s(paramData.data(), p + 2);
        p += 8;
        size_t bytes = actionParameterValueBytes(type, countItems);
        if (p + bytes > paramData.size()) return false;
        p += bytes;
        count++;
        if (count > 0xFFFFu) return false;
    }
    countOut = (uint16_t)count;
    return true;
}

struct ActionJsonEntry {
    uint16_t id = 0;
    uint16_t expirationMode = 0;
    std::string type;
    uint32_t flags = 0;
    uint32_t triggerLower = 0;
    uint32_t triggerDelta = 0;
    uint32_t parameterOffset = 0;
    uint16_t indent = 0;
    uint16_t numParams = 0;
    std::vector<uint8_t> parameterData;
};

static bool readActionJsonEntries(const std::string& path, std::vector<ActionJsonEntry>& actions) {
    actions.clear();
    std::string j = readTextFile(path);
    if (j.empty()) return false;

    for (const std::string& obj : jsonObjectsInArray(j, "actions")) {
        ActionJsonEntry e;
        int id = 0, expiration = 0, flags = 0, indent = 0, parameterOffset = 0;
        if (!jsonIntNear(obj, "id", id) ||
            !jsonIntNear(obj, "expiration_mode_id", expiration) ||
            !jsonIntNear(obj, "flags_raw", flags) ||
            !jsonIntNear(obj, "indent", indent) ||
            !jsonIntNear(obj, "parameter_data_offset", parameterOffset)) {
            return false;
        }
        double lower = 0.0, delta = 0.0;
        jsonNumberNear(obj, "trigger_time_lower_bound_seconds", lower);
        jsonNumberNear(obj, "trigger_time_delta_seconds", delta);
        e.id = (uint16_t)id;
        e.expirationMode = (uint16_t)expiration;
        e.type = jsonString(obj, "type", "");
        e.flags = (uint32_t)flags;
        e.triggerLower = (uint32_t)std::lround(lower * 30.0);
        e.triggerDelta = (uint32_t)std::lround(delta * 30.0);
        e.parameterOffset = (uint32_t)parameterOffset;
        e.indent = (uint16_t)indent;
        std::string hex = jsonString(obj, "parameter_data_hex", "");
        if (!parseHexBytes(hex, e.parameterData)) return false;
        if (e.parameterData.size() > 0xFFFFu) return false;
        if (!countActionParameters(e.parameterData, e.numParams)) return false;
        actions.push_back(std::move(e));
    }
    return true;
}

static bool applyMapActionsFromJson(std::vector<uint8_t>& meshData, const std::string& path) {
    constexpr size_t MESH_HEADER_SIZE = 1024;
    constexpr size_t ACTION_HEAD_SIZE = 64;
    std::vector<ActionJsonEntry> actions;
    if (!readActionJsonEntries(path, actions)) return false;

    std::string j = readTextFile(path);
    size_t actionHeadEnd = actions.size() * ACTION_HEAD_SIZE;
    size_t bufferSize = (size_t)std::max(jsonInt(j, "action_buffer_size", 0), (int)actionHeadEnd);
    for (const ActionJsonEntry& a : actions) {
        bufferSize = std::max(bufferSize, actionHeadEnd + (size_t)a.parameterOffset + a.parameterData.size());
    }
    std::vector<uint8_t> buffer(bufferSize, 0);
    for (size_t i = 0; i < actions.size(); i++) {
        const ActionJsonEntry& a = actions[i];
        size_t h = i * ACTION_HEAD_SIZE;
        writeBE16To(buffer.data() + h + 0, a.id);
        writeBE16To(buffer.data() + h + 2, a.expirationMode);
        writeBE32To(buffer.data() + h + 4, a.type.empty() ? 0xFFFFFFFFu : tagFromString(a.type));
        writeBE32To(buffer.data() + h + 8, a.flags);
        writeBE32To(buffer.data() + h + 12, a.triggerLower);
        writeBE32To(buffer.data() + h + 16, a.triggerDelta);
        writeBE16To(buffer.data() + h + 20, a.numParams);
        writeBE16To(buffer.data() + h + 22, (uint16_t)a.parameterData.size());
        writeBE32To(buffer.data() + h + 24, a.parameterOffset);
        writeBE16To(buffer.data() + h + 28, a.indent);
        std::copy(a.parameterData.begin(), a.parameterData.end(),
                  buffer.begin() + (ptrdiff_t)(actionHeadEnd + (size_t)a.parameterOffset));
    }

    uint32_t oldOffset = (uint32_t)readBE32s(meshData.data(), 0x84);
    uint32_t oldSize = (uint32_t)readBE32s(meshData.data(), 0x88);
    size_t oldStart = MESH_HEADER_SIZE + (size_t)oldOffset;
    size_t oldEnd = oldStart + (size_t)oldSize;
    if (oldStart > meshData.size() || oldEnd > meshData.size() || oldStart > oldEnd) return false;

    meshData.erase(meshData.begin() + (ptrdiff_t)oldStart, meshData.begin() + (ptrdiff_t)oldEnd);
    meshData.insert(meshData.begin() + (ptrdiff_t)oldStart, buffer.begin(), buffer.end());
    int64_t delta = (int64_t)buffer.size() - (int64_t)oldSize;
    writeBE32To(meshData.data() + 0x80, (uint32_t)actions.size());
    writeBE32To(meshData.data() + 0x88, (uint32_t)buffer.size());
    writeBE32To(meshData.data() + 0x1C, (uint32_t)((int64_t)readBE32s(meshData.data(), 0x1C) + delta));

    auto bumpSectionOffset = [&](size_t fieldOff) {
        uint32_t offset = (uint32_t)readBE32s(meshData.data(), fieldOff);
        if (offset > oldOffset) {
            writeBE32To(meshData.data() + fieldOff, (uint32_t)((int64_t)offset + delta));
        }
    };
    bumpSectionOffset(0xC0);  // media_coverage_region_offset
    bumpSectionOffset(0xCC);  // mesh_LOD_data_offset
    bumpSectionOffset(0x114); // connectors_offset
    bumpSectionOffset(0x120); // Myth II trailing section offset / connector-size field

    printf("Rebuilt map actions: %zu actions, %zu bytes from %s\n",
           actions.size(), buffer.size(), path.c_str());
    return true;
}

static bool zeroRegenerableMeshCacheSections(std::vector<uint8_t>& meshData) {
    constexpr size_t MESH_HEADER_SIZE = 1024;
    if (meshData.size() < MESH_HEADER_SIZE) return false;

    uint32_t actionOffset = (uint32_t)readBE32s(meshData.data(), 0x84);
    uint32_t actionSize = (uint32_t)readBE32s(meshData.data(), 0x88);
    uint64_t endOfActions = (uint64_t)actionOffset + (uint64_t)actionSize;
    if (endOfActions > (uint64_t)std::numeric_limits<uint32_t>::max()) return false;
    if (MESH_HEADER_SIZE + (size_t)endOfActions > meshData.size()) return false;

    uint32_t end = (uint32_t)endOfActions;
    writeBE32To(meshData.data() + 0x1C, end);

    writeBE32To(meshData.data() + 0xC0, end); // media_coverage_region_offset
    writeBE32To(meshData.data() + 0xC4, 0);   // media_coverage_region_size
    writeBE32To(meshData.data() + 0xC8, 0);   // runtime pointer

    writeBE32To(meshData.data() + 0xCC, end); // mesh_LOD_data_offset
    writeBE32To(meshData.data() + 0xD0, 0);   // mesh_LOD_data_size
    writeBE32To(meshData.data() + 0xD4, 0);   // runtime pointer

    writeBE32To(meshData.data() + 0x120, end); // Myth II trailing/cache section offset
    writeBE32To(meshData.data() + 0x124, 0);   // Myth II trailing/cache section size

    meshData.resize(MESH_HEADER_SIZE + (size_t)end);
    printf("Zeroed regenerable mesh cache sections; data_size now %u\n", end);
    return true;
}

static std::vector<UnitPlacementEdit> readUnitPlacementEdits(const std::string& path) {
    std::vector<UnitPlacementEdit> edits;
    std::string j = readTextFile(path);
    if (j.empty()) return edits;
    size_t p = 0;
    while ((p = j.find('{', p)) != std::string::npos) {
        size_t objStart = p;
        size_t objEnd = j.find('}', p);
        if (objStart == std::string::npos || objEnd == std::string::npos || objEnd <= objStart) {
            p++;
            continue;
        }
        std::string obj = j.substr(objStart, objEnd - objStart + 1);
        UnitPlacementEdit e;
        jsonBoolNear(obj, "add", e.add);
        bool hasMarker = jsonIntNear(obj, "marker_idx", e.markerIdx);
        bool hasSource = jsonIntNear(obj, "source_marker_idx", e.sourceMarkerIdx);
        if ((hasMarker || (e.add && hasSource)) &&
            jsonNumberNear(obj, "x", e.x) &&
            jsonNumberNear(obj, "y", e.y) &&
            jsonNumberNear(obj, "z", e.z)) {
            e.hasFacing = jsonNumberNear(obj, "facing_deg", e.facingDeg);
            edits.push_back(e);
        }
        p = objEnd + 1;
    }
    return edits;
}

static std::vector<MarkerPlacementEdit> readMarkerPlacementEdits(const std::string& path) {
    std::vector<MarkerPlacementEdit> edits;
    std::string j = readTextFile(path);
    if (j.empty()) return edits;

    for (const std::string& obj : jsonObjectsWithMarkerIdx(j)) {
        MarkerPlacementEdit e;
        if (jsonIntNear(obj, "marker_idx", e.markerIdx) &&
            jsonNumberNear(obj, "x", e.x) &&
            jsonNumberNear(obj, "y", e.y) &&
            jsonNumberNear(obj, "z", e.z)) {
            e.hasFacing = jsonNumberNear(obj, "facing_deg", e.facingDeg);
            edits.push_back(e);
        }
    }
    return edits;
}

static uint16_t yawFromDegrees(double deg) {
    while (deg < 0.0) deg += 360.0;
    while (deg >= 360.0) deg -= 360.0;
    int v = (int)std::lround((deg / 360.0) * 65536.0);
    v &= 0xFFFF;
    return (uint16_t)v;
}

static bool applyMarkerPlacementEdits(std::vector<uint8_t>& meshData, const std::string& path,
                                      int expectedMarkerType, const char* label) {
    constexpr int WORLD_ONE = 512;
    constexpr size_t MESH_HEADER_SIZE = 1024;
    constexpr size_t INSTANCE_SIZE = 64;
    if (meshData.size() < MESH_HEADER_SIZE) return false;

    std::vector<MarkerPlacementEdit> edits = readMarkerPlacementEdits(path);
    if (edits.empty()) return true;

    uint32_t instanceCount = (uint32_t)readBE32s(meshData.data(), 0x34);
    uint32_t instanceOffset = (uint32_t)readBE32s(meshData.data(), 0x38);
    size_t base = MESH_HEADER_SIZE + (size_t)instanceOffset;
    if (base + (size_t)instanceCount * INSTANCE_SIZE > meshData.size()) return false;

    int changed = 0;
    for (const MarkerPlacementEdit& e : edits) {
        if (e.markerIdx < 0 || (uint32_t)e.markerIdx >= instanceCount) continue;
        size_t off = base + (size_t)e.markerIdx * INSTANCE_SIZE;
        int16_t markerType = readBE16s(meshData.data(), off + 4);
        if (markerType != expectedMarkerType) continue;
        writeBE32To(meshData.data() + off + 12, (uint32_t)(int32_t)std::lround(e.x * WORLD_ONE));
        writeBE32To(meshData.data() + off + 16, (uint32_t)(int32_t)std::lround(e.y * WORLD_ONE));
        writeBE32To(meshData.data() + off + 20, (uint32_t)(int32_t)std::lround(e.z * WORLD_ONE));
        if (e.hasFacing) writeBE16To(meshData.data() + off + 32, yawFromDegrees(e.facingDeg));
        changed++;
    }
    printf("Applied %s marker placement edits: %d markers from %s\n", label, changed, path.c_str());
    return true;
}

static bool bumpUnitTypeInstanceCount(std::vector<uint8_t>& meshData, uint16_t paletteIdx) {
    constexpr size_t MESH_HEADER_SIZE = 1024;
    uint32_t unitTypeCount = (uint32_t)readBE32s(meshData.data(), 0x24);
    uint32_t unitTypeOffset = (uint32_t)readBE32s(meshData.data(), 0x28);
    size_t base = MESH_HEADER_SIZE + (size_t)unitTypeOffset;
    if (base + (size_t)unitTypeCount * 32 > meshData.size()) return false;

    uint16_t rel = 0;
    for (uint32_t i = 0; i < unitTypeCount; i++) {
        size_t off = base + (size_t)i * 32;
        int16_t markerType = readBE16s(meshData.data(), off);
        if (markerType != 3) continue;
        if (rel == paletteIdx) {
            uint16_t count = (uint16_t)readBE16s(meshData.data(), off + 28);
            writeBE16To(meshData.data() + off + 28, (uint16_t)(count + 1));
            return true;
        }
        rel++;
    }
    return false;
}

static bool applyUnitPlacementEdits(std::vector<uint8_t>& meshData, const std::string& path) {
    constexpr int WORLD_ONE = 512;
    constexpr size_t MESH_HEADER_SIZE = 1024;
    constexpr size_t INSTANCE_SIZE = 64;
    if (meshData.size() < MESH_HEADER_SIZE) return false;
    std::vector<UnitPlacementEdit> edits = readUnitPlacementEdits(path);
    if (edits.empty()) return true;

    uint32_t instanceCount = (uint32_t)readBE32s(meshData.data(), 0x34);
    uint32_t instanceOffset = (uint32_t)readBE32s(meshData.data(), 0x38);
    size_t base = MESH_HEADER_SIZE + (size_t)instanceOffset;
    if (base + (size_t)instanceCount * INSTANCE_SIZE > meshData.size()) return false;

    int changed = 0;
    int added = 0;
    for (const UnitPlacementEdit& e : edits) {
        if (e.add) {
            int sourceIdx = e.sourceMarkerIdx >= 0 ? e.sourceMarkerIdx : e.markerIdx;
            if (sourceIdx < 0 || (uint32_t)sourceIdx >= instanceCount) continue;
            size_t sourceOff = base + (size_t)sourceIdx * INSTANCE_SIZE;
            int16_t markerType = readBE16s(meshData.data(), sourceOff + 4);
            if (markerType != 3) continue;

            auto bumpSectionOffset = [&](size_t fieldOff) {
                uint32_t offset = (uint32_t)readBE32s(meshData.data(), fieldOff);
                if (offset > instanceOffset) {
                    writeBE32To(meshData.data() + fieldOff, offset + (uint32_t)INSTANCE_SIZE);
                }
            };
            size_t insertOff = base + (size_t)instanceCount * INSTANCE_SIZE;
            if (insertOff > meshData.size()) return false;
            std::vector<uint8_t> rec(meshData.begin() + (ptrdiff_t)sourceOff,
                                     meshData.begin() + (ptrdiff_t)sourceOff + (ptrdiff_t)INSTANCE_SIZE);
            writeBE32To(rec.data() + 12, (uint32_t)(int32_t)std::lround(e.x * WORLD_ONE));
            writeBE32To(rec.data() + 16, (uint32_t)(int32_t)std::lround(e.y * WORLD_ONE));
            writeBE32To(rec.data() + 20, (uint32_t)(int32_t)std::lround(e.z * WORLD_ONE));
            if (e.hasFacing) writeBE16To(rec.data() + 32, yawFromDegrees(e.facingDeg));

            int16_t maxIdentifier = 0;
            for (uint32_t i = 0; i < instanceCount; i++) {
                int16_t identifier = readBE16s(meshData.data(), base + (size_t)i * INSTANCE_SIZE + 8);
                if (identifier > maxIdentifier) maxIdentifier = identifier;
            }
            writeBE16To(rec.data() + 8, (uint16_t)(maxIdentifier + 1));
            std::fill(rec.begin() + 56, rec.end(), 0);

            meshData.insert(meshData.begin() + (ptrdiff_t)insertOff, rec.begin(), rec.end());
            instanceCount++;
            writeBE32To(meshData.data() + 0x34, instanceCount);
            writeBE32To(meshData.data() + 0x3C, instanceCount * (uint32_t)INSTANCE_SIZE);
            writeBE32To(meshData.data() + 0x1C, (uint32_t)(readBE32s(meshData.data(), 0x1C) + INSTANCE_SIZE));
            bumpSectionOffset(0x84);  // map_actions_offset
            bumpSectionOffset(0xC0);  // media_coverage_region_offset
            bumpSectionOffset(0xCC);  // mesh_LOD_data_offset
            bumpSectionOffset(0x114); // connectors_offset
            bumpSectionOffset(0x120); // Myth II trailing section offset / connector-size field
            uint16_t paletteIdx = (uint16_t)readBE16s(rec.data(), 6);
            bumpUnitTypeInstanceCount(meshData, paletteIdx);
            base = MESH_HEADER_SIZE + (size_t)instanceOffset;
            added++;
            continue;
        }
        if (e.markerIdx < 0 || (uint32_t)e.markerIdx >= instanceCount) continue;
        size_t off = base + (size_t)e.markerIdx * INSTANCE_SIZE;
        int16_t markerType = readBE16s(meshData.data(), off + 4);
        if (markerType != 3) continue;
        writeBE32To(meshData.data() + off + 12, (uint32_t)(int32_t)std::lround(e.x * WORLD_ONE));
        writeBE32To(meshData.data() + off + 16, (uint32_t)(int32_t)std::lround(e.y * WORLD_ONE));
        writeBE32To(meshData.data() + off + 20, (uint32_t)(int32_t)std::lround(e.z * WORLD_ONE));
        if (e.hasFacing) writeBE16To(meshData.data() + off + 32, yawFromDegrees(e.facingDeg));
        changed++;
    }
    printf("Applied unit placement edits: %d moved, %d added from %s\n", changed, added, path.c_str());
    return true;
}

static bool readManifest(const std::string& path, Manifest& m) {
    auto raw = readFile(path);
    if (raw.empty()) {
        fprintf(stderr, "Cannot read: %s\n", path.c_str());
        return false;
    }
    std::string j(raw.begin(), raw.end());
    m.meshTag = jsonString(j, "mesh_tag", "");
    m.submeshWidth = jsonInt(j, "width", jsonInt(j, "submesh_dimensions", 0));
    m.submeshHeight = jsonInt(j, "height", 0);
    if (m.submeshWidth == 0) m.submeshWidth = jsonInt(j, "width", 0);
    if (m.submeshHeight == 0) m.submeshHeight = jsonInt(j, "height", 0);
    m.landscapeTag = jsonString(j, "landscape_256", "");
    m.mapNameTag = jsonString(j, "map_name_stli", "");
    m.pregameTag = jsonString(j, "pregame_256", "");
    m.overheadTag = jsonString(j, "overhead_256", "");
    m.postgameTag = jsonString(j, "postgame_256", "");

    if (m.submeshWidth == 0 || m.submeshHeight == 0) {
        size_t p = j.find("\"submesh_dimensions\"");
        if (p != std::string::npos) {
            std::string tail = j.substr(p);
            m.submeshWidth = std::max(m.submeshWidth, jsonInt(tail, "width", 0));
            m.submeshHeight = std::max(m.submeshHeight, jsonInt(tail, "height", 0));
        }
    }
    if (m.landscapeTag.empty()) {
        size_t p = j.find("\"referenced_tags\"");
        if (p != std::string::npos) {
            std::string tail = j.substr(p);
            m.landscapeTag = jsonString(tail, "landscape_256", "");
            m.mapNameTag = jsonString(tail, "map_name_stli", "");
            m.pregameTag = jsonString(tail, "pregame_256", "");
            m.overheadTag = jsonString(tail, "overhead_256", "");
            m.postgameTag = jsonString(tail, "postgame_256", "");
        }
    }
    return m.meshTag.size() == 4;
}

struct TagSpec {
    std::string filePath;
    std::vector<uint8_t> data;
    uint32_t groupTag = 0;
    uint32_t subgroupTag = 0;
    std::string name;
    uint16_t version = 0;
    uint32_t offset = 0;
};

static uint32_t crcTable[256];
static bool crcTableReady = false;

static void initCRC() {
    if (crcTableReady) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
        crcTable[i] = crc;
    }
    crcTableReady = true;
}

static uint32_t crcUpdate(uint32_t crc, const uint8_t* data, size_t len) {
    initCRC();
    for (size_t i = 0; i < len; i++) {
        uint32_t a = (crc >> 8) & 0x00FFFFFFu;
        uint32_t b = crcTable[(crc ^ data[i]) & 0xFFu];
        crc = a ^ b;
    }
    return crc;
}

static std::vector<uint8_t> buildHeaderBytes(uint16_t type, uint16_t version,
                                             const std::string& name, const std::string& url,
                                             uint16_t entryPointCount, uint16_t tagCount,
                                             uint32_t checksum, uint32_t flags,
                                             uint32_t size, uint32_t headerChecksum,
                                             uint32_t signature) {
    std::vector<uint8_t> h(128, 0);
    writeBE16To(h.data() + 0, type);
    writeBE16To(h.data() + 2, version);
    memcpy(h.data() + 4, name.c_str(), std::min<size_t>(name.size(), 31));
    memcpy(h.data() + 36, url.c_str(), std::min<size_t>(url.size(), 63));
    writeBE16To(h.data() + 100, entryPointCount);
    writeBE16To(h.data() + 102, tagCount);
    writeBE32To(h.data() + 104, checksum);
    writeBE32To(h.data() + 108, flags);
    writeBE32To(h.data() + 112, size);
    writeBE32To(h.data() + 116, headerChecksum);
    writeBE32To(h.data() + 124, signature);
    return h;
}

static void writeFixedString(FILE* f, const std::string& s, size_t len) {
    std::vector<uint8_t> buf(len, 0);
    memcpy(buf.data(), s.c_str(), std::min(len - 1, s.size()));
    fwrite(buf.data(), 1, len, f);
}

static bool fileExistsNonEmpty(const std::string& path) {
    auto d = readFile(path);
    return !d.empty();
}

static std::string firstExistingPath(const std::initializer_list<std::string>& paths) {
    for (const auto& path : paths) {
        if (fileExists(path)) return path;
    }
    return {};
}

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II Plugin Assembler\n\n"
        "Usage:\n"
        "  %s <folder> [output] [--edit] [--obj <input.obj>] [--water-obj <input.obj>] [--heightscale <n>] [--water] [--water-flags] [--animation] [--zero-runtime-cache]\n\n"
        "  folder   Extracted Myth II map folder from extract\n"
        "  output   Output plugin path (default: <folder>_plugin)\n"
        "  --edit   Re-read edited assets from the folder before packing\n"
        "  --obj    Import terrain displacement from OBJ into raw/mesh_tag.bin during --edit\n"
        "  --water-obj Import wet-cell media_height from a water-surface OBJ during --edit\n"
        "  --water  Experimentally import terrain/water.bmp including media-height changes\n"
        "  --water-flags Import only water flags/types from terrain/water.bmp (default under --edit)\n"
        "  --animation Experimentally import terrain/animation.bmp during --edit\n"
        "  --zero-runtime-cache Experimentally omit regenerable post-action mesh cache sections during --edit\n"
        "  --heightscale OBJ vertical scale, default 1/512\n",
        p);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    std::string folder;
    std::string output;
    bool edit = false;
    bool importWater = false;
    bool importWaterFlags = false;
    bool importAnimation = false;
    bool zeroRuntimeCache = false;
    std::string objPath;
    std::string waterObjPath;
    float objHeightScale = 1.0f / 512.0f;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--edit") {
            edit = true;
        } else if (a == "--water") {
            importWater = true;
        } else if (a == "--water-flags") {
            importWaterFlags = true;
        } else if (a == "--animation") {
            importAnimation = true;
        } else if (a == "--zero-runtime-cache") {
            zeroRuntimeCache = true;
        } else if (a == "--obj") {
            if (i + 1 < argc) objPath = argv[++i];
            else {
                fprintf(stderr, "Error: --obj requires a path\n");
                return 1;
            }
        } else if (a == "--water-obj") {
            if (i + 1 < argc) waterObjPath = argv[++i];
            else {
                fprintf(stderr, "Error: --water-obj requires a path\n");
                return 1;
            }
        } else if (a == "--heightscale") {
            if (i + 1 < argc) objHeightScale = (float)atof(argv[++i]);
            else {
                fprintf(stderr, "Error: --heightscale requires a number\n");
                return 1;
            }
            if (objHeightScale <= 0.0f) {
                fprintf(stderr, "Error: --heightscale must be positive\n");
                return 1;
            }
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", a.c_str());
            return 1;
        } else if (folder.empty()) {
            folder = a;
        } else if (output.empty()) {
            output = a;
        } else {
            fprintf(stderr, "Too many positional arguments\n");
            return 1;
        }
    }
    if (folder.empty()) {
        usage(argv[0]);
        return 1;
    }
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();
    if (output.empty()) output = folder + "_plugin";

    Manifest mf;
    if (!readManifest(folder + "/manifest.json", mf)) return 1;

    std::string effectiveObjPath;
    std::string effectiveWaterObjPath;
    if (edit) {
        effectiveObjPath = objPath;
        if (effectiveObjPath.empty()) {
            effectiveObjPath = firstExistingPath({
                folder + "/assets/terrain/displacement.obj"
            });
        }

        effectiveWaterObjPath = waterObjPath;
        if (effectiveWaterObjPath.empty()) {
            effectiveWaterObjPath = firstExistingPath({
                folder + "/assets/terrain/water.obj"
            });
        }

        if (!importWater && effectiveWaterObjPath.empty()) importWaterFlags = true;
    }

    printf("Myth II Plugin Assembler\n");
    printf("========================\n");
    printf("Folder:        %s\n", folder.c_str());
    printf("Output:        %s\n", output.c_str());
    printf("Mesh tag:      %s\n", mf.meshTag.c_str());
    printf("Edit mode:     %s\n\n", edit ? "yes" : "no");
    if (edit) {
        if (!effectiveObjPath.empty()) printf("OBJ:           %s (scale %.9f)\n\n", effectiveObjPath.c_str(), objHeightScale);
        if (!effectiveWaterObjPath.empty()) printf("Water OBJ:     %s (scale %.9f)\n\n", effectiveWaterObjPath.c_str(), objHeightScale);
        if (!effectiveWaterObjPath.empty()) {
            printf("Water BMP:     skipped because water OBJ import is active\n\n");
        } else {
            if (importWater) printf("Water import:  enabled (experimental)\n\n");
            if (importWaterFlags && !importWater) printf("Water flags:   enabled\n\n");
        }
        if (importAnimation) printf("Animation import: enabled (experimental)\n\n");
        if (zeroRuntimeCache) printf("Runtime cache: zero regenerated mesh sections (experimental)\n\n");
    }

    std::vector<TagSpec> tags;
    int32_t meshTeamCount = 0;

    auto addTag = [&](const std::string& path, uint32_t group, const std::string& id,
                      const std::string& fallbackName, uint16_t version) {
        if (path.empty() || id.size() != 4) return;
        std::vector<uint8_t> data = readFile(path);
        if (data.empty()) return;
        TagSpec t;
        t.filePath = path;
        t.data = std::move(data);
        t.groupTag = group;
        t.subgroupTag = tagFromString(id);
        t.name = fallbackName.empty() ? id : fallbackName;
        t.version = version;
        tags.push_back(std::move(t));
    };

    auto addGeneratedTag = [&](const std::string& label, std::vector<uint8_t> data,
                               uint32_t group, const std::string& id,
                               const std::string& fallbackName, uint16_t version) {
        if (data.empty() || id.size() != 4) return;
        TagSpec t;
        t.filePath = label;
        t.data = std::move(data);
        t.groupTag = group;
        t.subgroupTag = tagFromString(id);
        t.name = fallbackName.empty() ? id : fallbackName;
        t.version = version;
        tags.push_back(std::move(t));
    };

    auto addScreenTag = [&](const std::string& tagPath, const std::initializer_list<std::string>& bmpPaths,
                            const std::string& id, const std::string& label) {
        if (isNullTagString(id)) return;
        if (fileExists(tagPath)) {
            addTag(tagPath, 0x2E323536u, id, id + " " + label, 4);
            return;
        }
        std::string bmpPath = firstExistingPath(bmpPaths);
        if (bmpPath.empty()) return;
        std::vector<uint8_t> data = buildSingleImage256FromBMP(bmpPath, label);
        if (!data.empty()) {
            addGeneratedTag(bmpPath, std::move(data), 0x2E323536u, id, id + " " + label, 4);
            printf("Generated %s .256 from %s\n", label.c_str(), bmpPath.c_str());
        }
    };

    addTag(folder + "/raw/mesh_tag.bin", 0x6D657368u, mf.meshTag, mf.meshTag, 11);
    addTag(folder + "/terrain/terrain_tag.bin", 0x2E323536u, mf.landscapeTag,
           isNullTagString(mf.landscapeTag) ? "" : (mf.landscapeTag + " terrain"), 4);
    if (!isNullTagString(mf.mapNameTag)) {
        std::string nameTagPath = folder + "/strings/name_tag.bin";
        if (fileExists(nameTagPath)) {
            addTag(nameTagPath, 0x73746C69u, mf.mapNameTag, mf.mapNameTag, 1);
        } else {
            std::string txtPath = firstExistingPath({
                folder + "/strings/name.txt",
                folder + "/layers/20_name.txt"
            });
            std::string txt = readTextFile(txtPath);
            if (!txt.empty()) {
                std::string stli = textToStli(txt);
                addGeneratedTag(txtPath.empty() ? "generated:name_stli" : txtPath,
                                std::vector<uint8_t>(stli.begin(), stli.end()),
                                0x73746C69u, mf.mapNameTag, mf.mapNameTag, 1);
                printf("Generated map name stli from %s\n", txtPath.c_str());
            }
        }
    }
    addScreenTag(folder + "/screens/pregame_tag.bin",
                 {folder + "/screens/pregame.bmp", folder + "/layers/11_pregame.bmp"},
                 mf.pregameTag, "pregame");
    addScreenTag(folder + "/screens/overhead_tag.bin",
                 {folder + "/screens/overhead.bmp", folder + "/layers/10_overhead.bmp"},
                 mf.overheadTag, "overhead");
    addScreenTag(folder + "/screens/postgame_tag.bin",
                 {folder + "/screens/postgame.bmp", folder + "/layers/12_postgame.bmp"},
                 mf.postgameTag, "postgame");

    if (tags.empty()) {
        fprintf(stderr, "No tag binaries found in %s\n", folder.c_str());
        return 1;
    }

    if (!tags.empty() && tags[0].groupTag == 0x6D657368u && tags[0].data.size() >= 92) {
        meshTeamCount = readBE32s(tags[0].data.data(), 88);
        if (meshTeamCount < 0 || meshTeamCount > 64) meshTeamCount = 0;
    }

    if (edit) {
        for (auto& t : tags) {
            if (t.groupTag == 0x6D657368u) {
                if (!effectiveObjPath.empty()) {
                    if (!applyObjToMyth2Mesh(t.data, mf.submeshWidth, mf.submeshHeight, effectiveObjPath, objHeightScale)) {
                        fprintf(stderr, "OBJ import failed: %s\n", effectiveObjPath.c_str());
                        return 1;
                    }
                }
                std::string passBmp = firstExistingPath({
                    folder + "/terrain/passability.bmp",
                    folder + "/layers/02_passability.bmp"
                });
                if (fileExists(passBmp)) {
                    if (!applyPassabilityToMyth2Mesh(t.data, mf.submeshWidth, mf.submeshHeight, passBmp)) {
                        fprintf(stderr, "Passability map import failed: %s\n", passBmp.c_str());
                        return 1;
                    }
                }
                if (effectiveWaterObjPath.empty() && (importWater || importWaterFlags)) {
                    std::string waterBmp = firstExistingPath({
                        folder + "/terrain/water.bmp",
                        folder + "/layers/03_water.bmp"
                    });
                    if (fileExists(waterBmp)) {
                        bool ok = importWater
                            ? applyWaterToMyth2Mesh(t.data, mf.submeshWidth, mf.submeshHeight, waterBmp)
                            : applyWaterFlagsToMyth2Mesh(t.data, mf.submeshWidth, mf.submeshHeight, waterBmp);
                        if (!ok) {
                            fprintf(stderr, "Water map import failed: %s\n", waterBmp.c_str());
                            return 1;
                        }
                    }
                }
                if (!effectiveWaterObjPath.empty()) {
                    if (!applyWaterObjToMyth2Mesh(t.data, mf.submeshWidth, mf.submeshHeight, effectiveWaterObjPath, objHeightScale)) {
                        fprintf(stderr, "Water OBJ import failed: %s\n", effectiveWaterObjPath.c_str());
                        return 1;
                    }
                }
                std::string reflectionBmp = firstExistingPath({
                    folder + "/terrain/reflection.bmp",
                    folder + "/layers/06_reflection.bmp"
                });
                if (fileExists(reflectionBmp)) {
                    if (!applyReflectionToMyth2Mesh(t.data, mf.submeshWidth, mf.submeshHeight, reflectionBmp)) {
                        fprintf(stderr, "Reflection map import failed: %s\n", reflectionBmp.c_str());
                        return 1;
                    }
                }
                if (importAnimation) {
                    std::string animationBmp = firstExistingPath({
                        folder + "/terrain/animation.bmp",
                        folder + "/layers/04_animation.bmp"
                    });
                    if (fileExists(animationBmp)) {
                        if (!applyAnimationToMyth2Mesh(t.data, mf.submeshWidth, mf.submeshHeight, animationBmp)) {
                            fprintf(stderr, "Animation map import failed: %s\n", animationBmp.c_str());
                            return 1;
                        }
                    }
                }
                std::string unitPlacementPath = firstExistingPath({
                    folder + "/assets/sprites/units_edited.json"
                });
                if (fileExists(unitPlacementPath)) {
                    if (!applyUnitPlacementEdits(t.data, unitPlacementPath)) {
                        fprintf(stderr, "Unit placement import failed: %s\n", unitPlacementPath.c_str());
                        return 1;
                    }
                }
                struct MarkerJsonImport {
                    const char* path;
                    int markerType;
                    const char* label;
                };
                const MarkerJsonImport markerImports[] = {
                    {"/assets/sprites/scenery.json", 1, "sprite scenery"},
                    {"/assets/sounds/sounds.json", 5, "sound"},
                    {"/assets/models/projectiles.json", 9, "projectile"},
                    {"/placement.json", 6, "direct model"},
                    {"/assets/models/animations.json", 11, "model animation"},
                };
                for (const auto& markerImport : markerImports) {
                    std::string markerPath = folder + markerImport.path;
                    if (!fileExists(markerPath)) continue;
                    if (!applyMarkerPlacementEdits(t.data, markerPath, markerImport.markerType, markerImport.label)) {
                        fprintf(stderr, "%s marker placement import failed: %s\n",
                                markerImport.label, markerPath.c_str());
                        return 1;
                    }
                }
                std::string actionPath = firstExistingPath({
                    folder + "/assets/actions/actions.json"
                });
                if (fileExists(actionPath)) {
                    if (!applyMapActionsFromJson(t.data, actionPath)) {
                        fprintf(stderr, "Map action import failed: %s\n", actionPath.c_str());
                        return 1;
                    }
                }
                if (zeroRuntimeCache) {
                    if (!zeroRegenerableMeshCacheSections(t.data)) {
                        fprintf(stderr, "Zeroing mesh runtime cache sections failed\n");
                        return 1;
                    }
                }
            } else if (t.groupTag == 0x73746C69u && t.subgroupTag == tagFromString(mf.mapNameTag)) {
                std::string txtPath = firstExistingPath({
                    folder + "/strings/name.txt",
                    folder + "/layers/20_name.txt"
                });
                std::string txt = readTextFile(txtPath);
                if (!txt.empty()) {
                    std::string stli = textToStli(txt);
                    t.data.assign(stli.begin(), stli.end());
                    printf("Rebuilt map name stli from %s\n", txtPath.c_str());
                }
            } else if (t.groupTag == 0x2E323536u && t.subgroupTag == tagFromString(mf.landscapeTag)) {
                int bW = 0, bH = 0;
                std::string bmpPath = firstExistingPath({
                    folder + "/terrain/terrain.bmp",
                    folder + "/layers/01_terrain.bmp"
                });
                auto bmpRaw = readFile(bmpPath);
                auto bmp = readBMP8(bmpPath, bW, bH);
                if (!bmp.empty() && bW == mf.submeshWidth * 256 && bH == mf.submeshHeight * 256) {
                    if (injectTerrainColor(t.data, bmp, bW, bH, bmpRaw, mf.submeshWidth, mf.submeshHeight)) {
                        printf("Applied %s\n", bmpPath.c_str());
                    }
                }
                int sW = 0, sH = 0;
                std::string shadowPath = firstExistingPath({
                    folder + "/terrain/shadow.bmp",
                    folder + "/layers/02_shadow.bmp"
                });
                auto shadowRaw = readFile(shadowPath);
                auto shadow = readBMP8(shadowPath, sW, sH);
                if (!shadow.empty() && sW == mf.submeshWidth * 256 && sH == mf.submeshHeight * 256) {
                    if (injectTerrainShadow(t.data, shadow, sW, sH, shadowRaw, mf.submeshWidth, mf.submeshHeight)) {
                        printf("Applied %s\n", shadowPath.c_str());
                    }
                }
            } else if (t.groupTag == 0x2E323536u) {
                std::string bmpPath;
                if (t.subgroupTag == tagFromString(mf.pregameTag)) {
                    bmpPath = firstExistingPath({
                        folder + "/screens/pregame.bmp",
                        folder + "/layers/11_pregame.bmp"
                    });
                } else if (t.subgroupTag == tagFromString(mf.overheadTag)) {
                    bmpPath = firstExistingPath({
                        folder + "/screens/overhead.bmp",
                        folder + "/layers/10_overhead.bmp"
                    });
                } else if (t.subgroupTag == tagFromString(mf.postgameTag)) {
                    bmpPath = firstExistingPath({
                        folder + "/screens/postgame.bmp",
                        folder + "/layers/12_postgame.bmp"
                    });
                }
                if (!bmpPath.empty() && fileExists(bmpPath)) {
                    int bW = 0, bH = 0;
                    auto bmpRaw = readFile(bmpPath);
                    auto bmp = readBMP8(bmpPath, bW, bH);
                    if (!bmp.empty() && injectSingleImage256(t.data, bmpRaw, bmp, bW, bH)) {
                        printf("Applied %s\n", bmpPath.c_str());
                    }
                }
            }
        }
        printf("\n");
    }

    const uint32_t headerSize = 128;
    const uint32_t entryPointSize = 112;
    const uint32_t tagHeaderSize = 64;
    uint32_t dataOffset = headerSize + entryPointSize + (uint32_t)tags.size() * tagHeaderSize;
    for (auto& t : tags) {
        t.offset = dataOffset;
        dataOffset += (uint32_t)t.data.size();
    }
    uint32_t fileSize = dataOffset;

    uint32_t dataCRC = 0xFFFFFFFFu;
    for (const auto& t : tags) dataCRC = crcUpdate(dataCRC, t.data.data(), t.data.size());

    std::string pluginName = dirNameOf(folder);
    std::vector<uint8_t> headerBE = buildHeaderBytes(
        1, 2, pluginName, "", 1, (uint16_t)tags.size(), dataCRC, 0, fileSize, 0, 0x646E6732u
    );
    uint32_t headerCRC = crcUpdate(0xFFFFFFFFu, headerBE.data(), headerBE.size());
    headerBE = buildHeaderBytes(
        1, 2, pluginName, "", 1, (uint16_t)tags.size(), dataCRC, 0, fileSize, headerCRC, 0x646E6732u
    );

    std::string printedName = mf.meshTag;
    auto nameText = readFile(folder + "/strings/name.txt");
    if (!nameText.empty()) {
        printedName.assign(nameText.begin(), nameText.end());
        while (!printedName.empty() && (printedName.back() == '\r' || printedName.back() == '\n'))
            printedName.pop_back();
    }

    FILE* out = fopen(output.c_str(), "wb");
    if (!out) {
        fprintf(stderr, "Cannot create: %s\n", output.c_str());
        return 1;
    }

    fwrite(headerBE.data(), 1, headerBE.size(), out);

    writeBE32(out, tagFromString(mf.meshTag));
    writeBE32(out, (uint32_t)meshTeamCount);
    writeBE32(out, 0);
    writeBE32(out, 0);
    writeFixedString(out, mf.meshTag, 32);
    writeFixedString(out, printedName, 64);

    for (size_t i = 0; i < tags.size(); i++) {
        const TagSpec& t = tags[i];
        writeBE16(out, (uint16_t)i);
        fputc(0, out);
        fputc(3, out);
        writeFixedString(out, t.name, 32);
        writeBE32(out, t.groupTag);
        writeBE32(out, t.subgroupTag);
        writeBE32(out, t.offset);
        writeBE32(out, (uint32_t)t.data.size());
        writeBE32(out, 0);
        writeBE16(out, t.version);
        fputc(0, out);
        fputc(0xFF, out);
        writeBE32(out, 0x6D746832u); // mth2
    }

    for (const auto& t : tags) {
        if (!t.data.empty()) fwrite(t.data.data(), 1, t.data.size(), out);
    }
    fclose(out);

    printf("Packed tags:   %zu\n", tags.size());
    for (const auto& t : tags) {
        printf("  %-12s %.4s  %6u bytes  %s\n",
               tagToString(t.groupTag).c_str(), tagToString(t.subgroupTag).c_str(),
               (unsigned)t.data.size(), t.filePath.c_str());
    }
    printf("\nDone.\n");
    printf("  Wrote Myth II plugin: %s\n", output.c_str());
    return 0;
}
