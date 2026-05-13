// mesh_build.cpp
// Reconstruct mesh_tag.bin from extracted 85gy folder assets.
// Generates: cell grid, flags, heights, normals, unit types,
// object instances (with cell-linked list), actions, and header.
//
// Usage:
//   mesh_build <folder>
//   mesh_build <folder> --output <path>
//   mesh_build <folder> --compare <original_mesh_tag.bin>

#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <limits>
#include <unordered_map>

#include "mesh_flags.h"

// ---- Big-endian I/O ----

static uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t swap32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}
static int16_t readBE16s(const uint8_t* b, size_t o) { int16_t v; memcpy(&v, b+o, 2); return (int16_t)swap16((uint16_t)v); }
static uint16_t readBE16u(const uint8_t* b, size_t o) { uint16_t v; memcpy(&v, b+o, 2); return swap16(v); }
static uint32_t readBE32u(const uint8_t* b, size_t o) { uint32_t v; memcpy(&v, b+o, 4); return swap32(v); }
static void writeBE16(uint8_t* b, size_t o, uint16_t v) { uint16_t s = swap16(v); memcpy(b+o, &s, 2); }
static void writeBE32(uint8_t* b, size_t o, uint32_t v) { uint32_t s = swap32(v); memcpy(b+o, &s, 4); }

static std::vector<uint8_t> readFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return {}; }
    std::vector<uint8_t> buf((size_t)sz);
    if (sz > 0 && fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return {}; }
    fclose(f);
    return buf;
}

static std::string readTextFile(const std::string& path) {
    auto d = readFile(path);
    return std::string(d.begin(), d.end());
}

static bool fileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// ---- JSON helpers ----

static int jsonInt(const std::string& j, const std::string& key, int def = 0) {
    for (const char* sep : {"\": ", "\":"}) {
        std::string needle = "\"" + key + sep;
        size_t pos = j.find(needle);
        if (pos == std::string::npos) continue;
        pos += needle.size();
        while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) pos++;
        bool neg = (pos < j.size() && j[pos] == '-');
        if (neg) pos++;
        int val = 0;
        while (pos < j.size() && j[pos] >= '0' && j[pos] <= '9') {
            val = val * 10 + (j[pos] - '0');
            pos++;
        }
        return neg ? -val : val;
    }
    return def;
}

static double jsonDouble(const std::string& j, const std::string& key, double def = 0.0) {
    for (const char* sep : {"\": ", "\":"}) {
        std::string needle = "\"" + key + sep;
        size_t pos = j.find(needle);
        if (pos == std::string::npos) continue;
        pos += needle.size();
        while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) pos++;
        bool neg = (pos < j.size() && j[pos] == '-');
        if (neg) pos++;
        double val = 0.0;
        bool afterDot = false;
        double frac = 1.0;
        while (pos < j.size()) {
            char c = j[pos];
            if (c >= '0' && c <= '9') {
                if (afterDot) { frac /= 10.0; val += (double)(c - '0') * frac; }
                else val = val * 10.0 + (double)(c - '0');
                pos++;
            } else if (c == '.' && !afterDot) {
                afterDot = true;
                pos++;
            } else break;
        }
        return neg ? -val : val;
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
        if (c == '\\' && pos < j.size()) out.push_back(j[pos++]);
        else out.push_back(c);
    }
    return def;
}

static std::string jsonStringAt(const std::string& j, size_t start, const std::string& key, const std::string& def = "") {
    size_t p = j.find("\"" + key + "\"", start);
    if (p == std::string::npos) return def;
    size_t colon = j.find(':', p);
    if (colon == std::string::npos) return def;
    size_t q = j.find('"', colon);
    if (q == std::string::npos) return def;
    q++;
    std::string out;
    while (q < j.size()) {
        char c = j[q++];
        if (c == '"') return out;
        if (c == '\\' && q < j.size()) out.push_back(j[q++]);
        else out.push_back(c);
    }
    return def;
}

static int jsonIntAt(const std::string& j, size_t start, const std::string& key, int def = 0) {
    size_t p = j.find("\"" + key + "\"", start);
    if (p == std::string::npos) return def;
    return jsonInt(j.substr(p), key, def);
}

static double jsonDoubleAt(const std::string& j, size_t start, const std::string& key, double def = 0.0) {
    size_t p = j.find("\"" + key + "\"", start);
    if (p == std::string::npos) return def;
    return jsonDouble(j.substr(p), key, def);
}

static uint32_t tagFromString(const std::string& s) {
    if (s.size() != 4) return 0;
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16)
         | ((uint32_t)(uint8_t)s[2] << 8)  | (uint32_t)(uint8_t)s[3];
}

// ---- BMP Reader (4-bit indexed, bottom-up) ----

struct BMP4Reader {
    int w = 0, h = 0;
    std::vector<uint8_t> pixels;

    bool load(const std::string& path) {
        auto d = readFile(path);
        if (d.size() < 118) return false;
        int dataOff = (int)d[10] | ((int)d[11] << 8) | ((int)d[12] << 16) | ((int)d[13] << 24);
        w = (int)d[18] | ((int)d[19] << 8) | ((int)d[20] << 16) | ((int)d[21] << 24);
        h = (int)d[22] | ((int)d[23] << 8) | ((int)d[24] << 16) | ((int)d[25] << 24);
        int bpp = (int)d[28];
        if (bpp != 4) return false;
        if ((size_t)dataOff + (size_t)(((w + 1) / 2 + 3) & ~3) * (size_t)h > d.size()) return false;
        int stride = ((w + 1) / 2 + 3) & ~3;
        pixels.resize((size_t)w * h);
        for (int y = 0; y < h; y++) {
            const uint8_t* row = d.data() + dataOff + (size_t)(h - 1 - y) * stride;
            for (int x = 0; x < w; x++) {
                uint8_t byte = row[x / 2];
                pixels[(size_t)y * w + x] = (x & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
            }
        }
        return true;
    }

    uint8_t pixel(int x, int y) const {
        if (x < 0 || x >= w || y < 0 || y >= h) return 0;
        return pixels[(size_t)y * w + x];
    }
};

// ---- OBJ Height Reader ----

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

// ---- Normal Table (256-entry, Q14) ----

struct NormalVec3 { double x = 0, y = 0, z = 0; };

static constexpr int NORMAL_FULL = 1024;
static constexpr int NORMAL_Q14_ONE = 0x4000;
static constexpr int NORMAL_TABLE_SIZE = 256;
static constexpr double WORLD_HEIGHT_SCALE = 1.0 / 512.0;

static NormalVec3 normalSub(const NormalVec3& a, const NormalVec3& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static NormalVec3 normalCross(const NormalVec3& a, const NormalVec3& b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static double normalDot(const NormalVec3& a, const NormalVec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static double normalLength(const NormalVec3& v) { return std::sqrt(normalDot(v, v)); }
static NormalVec3 normalNormalized(const NormalVec3& v) {
    double len = normalLength(v);
    if (len < 1e-12) return {0.0, 1.0, 0.0};
    return {v.x/len, v.y/len, v.z/len};
}
static double normalClamp1(double v) { return v < -1.0 ? -1.0 : v > 1.0 ? 1.0 : v; }
static double normalAngleDeg(const NormalVec3& a, const NormalVec3& b) {
    return std::acos(normalClamp1(normalDot(a, b))) * (180.0 / 3.14159265358979323846);
}
static int16_t normalQ14FromFloat(double v) {
    long n = lround(v * (double)NORMAL_Q14_ONE);
    if (n < -32768) n = -32768;
    if (n > 32767) n = 32767;
    return (int16_t)n;
}

static std::vector<NormalVec3> buildRuntimeNormalTable() {
    std::vector<int16_t> sinQ14(NORMAL_FULL), cosQ14(NORMAL_FULL);
    const double twoPi = 6.28318530717958647692;
    for (int i = 0; i < NORMAL_FULL; i++) {
        double a = ((double)i * twoPi) / (double)NORMAL_FULL;
        sinQ14[(size_t)i] = normalQ14FromFloat(std::sin(a));
        cosQ14[(size_t)i] = normalQ14FromFloat(std::cos(a));
    }
    sinQ14[0] = 0;                    cosQ14[0]   =  NORMAL_Q14_ONE;
    sinQ14[256] = NORMAL_Q14_ONE;     cosQ14[256] =  0;
    sinQ14[512] = 0;                  cosQ14[512] = -NORMAL_Q14_ONE;
    sinQ14[768] = -NORMAL_Q14_ONE;    cosQ14[768] =  0;

    std::vector<NormalVec3> out(NORMAL_TABLE_SIZE);
    int entry = 0;
    int local8 = 0x4020;
    while (entry < NORMAL_TABLE_SIZE) {
        unsigned pitchIdx = (unsigned)((local8 >> 6) & 0x3FF);
        int ringCount = (((int)cosQ14[pitchIdx] << 2) / 0x444) + 1;
        if (ringCount > 0) {
            int localC = 0x20;
            while (ringCount > 0 && entry < NORMAL_TABLE_SIZE) {
                unsigned azIdx = (unsigned)((localC >> 6) & 0x3FF);
                int16_t x = (int16_t)(((int)cosQ14[azIdx] * (int)cosQ14[pitchIdx]) >> 14);
                int16_t y = (int16_t)(((int)sinQ14[azIdx] * (int)cosQ14[pitchIdx]) >> 14);
                int16_t z = sinQ14[pitchIdx];
                out[(size_t)entry] = normalNormalized({
                    (double)x / (double)NORMAL_Q14_ONE,
                    (double)y / (double)NORMAL_Q14_ONE,
                    (double)z / (double)NORMAL_Q14_ONE
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
    return normalNormalized({-v.x, v.z, v.y});
}

static uint8_t nearestNormalIndex(const NormalVec3& empiricalNormal,
                                  const std::vector<NormalVec3>& table) {
    NormalVec3 rn = empiricalToRuntimeNormal(empiricalNormal);
    double bestErr = std::numeric_limits<double>::infinity();
    int bestIdx = 0;
    for (int i = 0; i < NORMAL_TABLE_SIZE; i++) {
        double err = normalAngleDeg(rn, table[(size_t)i]);
        if (err < bestErr) { bestErr = err; bestIdx = i; }
    }
    return (uint8_t)bestIdx;
}

// ---- Triangle sampling conventions ----

static void sampleTriangleCenters(int x, int y, int& t0x, int& t0y, int& t1x, int& t1y) {
    if (((x ^ y) & 1) != 0) {
        t0x = 2; t0y = 2;
        t1x = 5; t1y = 5;
    } else {
        t0x = 2; t0y = 5;
        t1x = 5; t1y = 2;
    }
}

static size_t meshMapSampleIndexOffset(int bmpW, int cellW, int x, int y, int lx, int ly) {
    int mx = (cellW - 1 - x) * 8 + (7 - lx);
    int py = y * 8 + ly;
    return (size_t)py * bmpW + mx;
}

// ---- Parsed JSON instance (intermediate) ----

struct JsonInst {
    std::string tag;
    double x, y, z;
    double facingDeg;
    int palIdx;
    int markerIdx;
};

// ---- Unit type definition ----

struct UnitTypeDef {
    uint16_t markerType = 0;
    uint32_t typeTag = 0;
    int palIdx = 0;
    int instanceCount = 0;
    int uniqueIdx = 0;
};

// ---- Action entry ----

struct ActionEntry {
    uint16_t id = 0;
    uint16_t expirationMode = 0;
    uint32_t type = 0xFFFFFFFFu;
    uint32_t flags = 0;
    uint32_t triggerLower = 0;
    uint32_t triggerDelta = 0;
    uint16_t numParams = 0;
    uint16_t paramSize = 0;
    uint32_t paramOffset = 0;
    uint16_t indent = 0;
    std::vector<uint8_t> paramData;
};

// ---- Main mesh builder ----

static int usage(const char* p) {
    fprintf(stderr,
        "Myth II Mesh Builder - reconstruct mesh_tag.bin from extracted assets\n\n"
        "Usage:\n"
        "  %s <folder> [--output <path>] [--compare <original_bin>]\n",
        p);
    return 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) return usage(argv[0]);

    std::string folder = argv[1];
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();

    std::string outputPath = folder + "/raw/mesh_tag.bin";
    std::string comparePath;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--output" && i+1 < argc) outputPath = argv[++i];
        else if (a == "--compare" && i+1 < argc) comparePath = argv[++i];
    }

    // ---- Read manifest ----
    std::string manifest = readTextFile(folder + "/manifest.json");
    int subW = jsonInt(manifest, "width", jsonInt(manifest, "submesh_dimensions", 0));
    int subH = jsonInt(manifest, "height", 0);
    if (subW == 0 || subH == 0) {
        size_t p = manifest.find("\"submesh_dimensions\"");
        if (p != std::string::npos) {
            subW = jsonInt(manifest.substr(p), "width", 0);
            subH = jsonInt(manifest.substr(p), "height", 0);
        }
    }
    if (subW == 0 || subH == 0) { fprintf(stderr, "Cannot read submesh dimensions\n"); return 1; }

    std::string meshTag = jsonString(manifest, "mesh_tag", "");
    if (meshTag.size() != 4) { fprintf(stderr, "Cannot read mesh tag\n"); return 1; }

    std::string landTag = jsonStringAt(manifest, 0, "landscape_256", "85gi");
    std::string mediaTag = jsonStringAt(manifest, 0, "media_tag", "wate");
    uint32_t meshFlags = (uint32_t)jsonInt(manifest, "mesh_flags", 0);

    int cellW = subW * 32;
    int cellH = subH * 32;
    int vertexW = cellW + 1;
    int vertexH = cellH + 1;
    int totalCells = cellW * cellH;

    printf("Mesh: %s, submesh %dx%d = %dx%d cells (%d total)\n",
           meshTag.c_str(), subW, subH, cellW, cellH, totalCells);

    size_t headerSize = 1024;
    size_t cellDataSize = (size_t)totalCells * 12;

    // ---- Read displacement OBJ ----
    std::vector<int16_t> heights;
    // Try assets/terrain/ first, then terrain/
    auto findObj = [&](const std::string& rel) -> std::string {
        std::string p = folder + "/assets/" + rel;
        if (fileExists(p)) return p;
        p = folder + "/" + rel;
        if (fileExists(p)) return p;
        return "";
    };

    std::string objPath = findObj("terrain/displacement.obj");
    if (objPath.empty()) { fprintf(stderr, "Cannot find displacement.obj\n"); return 1; }
    float heightScale = 1.0f / 512.0f;
    if (!parseOBJHeights(objPath, heightScale, vertexW, vertexH, heights)) {
        fprintf(stderr, "Failed to parse OBJ heights from: %s\n", objPath.c_str());
        return 1;
    }
    printf("Read %d OBJ heights from displacement\n", (int)heights.size());

    // ---- Read water OBJ ----
    std::vector<int16_t> waterHeights;
    bool hasWaterObj = false;
    std::string waterObjPath = findObj("terrain/water.obj");
    if (!waterObjPath.empty()) {
        hasWaterObj = parseOBJHeights(waterObjPath, heightScale, vertexW, vertexH, waterHeights);
        if (hasWaterObj) printf("Read %d water OBJ heights\n", (int)waterHeights.size());
    }

    // ---- Read terrain BMPs ----
    BMP4Reader passBMP, waterMaskBMP, animBMP, reflBMP;
    const int bmpW = cellW * 8;

    if (!passBMP.load(folder + "/terrain/passability.bmp")) {
        fprintf(stderr, "Cannot read passability.bmp\n"); return 1;
    }
    bool hasWaterMask = waterMaskBMP.load(folder + "/terrain/water_mask.bmp");
    bool hasAnim = animBMP.load(folder + "/terrain/animation.bmp");
    bool hasRefl = reflBMP.load(folder + "/terrain/reflection.bmp");
    printf("Loaded BMPs: pass=y water_mask=%d anim=%d refl=%d\n", hasWaterMask, hasAnim, hasRefl);

    // ---- Build cell grid ----
    std::vector<uint8_t> cellData(cellDataSize, 0);
    std::vector<NormalVec3> normalTable = buildRuntimeNormalTable();

    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            size_t off = (size_t)(y * cellW + x) * 12;

            int t0x, t0y, t1x, t1y;
            sampleTriangleCenters(x, y, t0x, t0y, t1x, t1y);

            size_t px0 = meshMapSampleIndexOffset(bmpW, cellW, x, y, t0x, t0y);
            size_t px1 = meshMapSampleIndexOffset(bmpW, cellW, x, y, t1x, t1y);

            uint8_t t0 = passBMP.pixels[px0];
            uint8_t t1 = passBMP.pixels[px1];

            bool w0 = hasWaterMask && waterMaskBMP.pixels[px0] != 0;
            bool w1 = hasWaterMask && waterMaskBMP.pixels[px1] != 0;

            bool animated = false;
            if (hasAnim) {
                int ac = 0;
                for (int ly = 0; ly < 8; ly++)
                    for (int lx = 0; lx < 8; lx++)
                        if (animBMP.pixels[meshMapSampleIndexOffset(bmpW, cellW, x, y, lx, ly)] != 0) ac++;
                animated = (ac >= 32);
            }

            bool reflective = false;
            if (hasRefl) {
                int rc = 0;
                for (int ly = 0; ly < 8; ly++)
                    for (int lx = 0; lx < 8; lx++)
                        if (reflBMP.pixels[meshMapSampleIndexOffset(bmpW, cellW, x, y, lx, ly)] != 0) rc++;
                reflective = (rc >= 32);
            }

            uint16_t flags = 0;
            flags |= (uint16_t)(t0 & 0x0F) << 4;
            flags |= (uint16_t)(t1 & 0x0F);
            if (w0) flags |= MYTH2_MESH_CELL_TRIANGLE0_IS_MEDIA_FLAG;
            if (w1) flags |= MYTH2_MESH_CELL_TRIANGLE1_IS_MEDIA_FLAG;
            if (animated) flags |= MYTH2_MESH_VERTEX_IS_ANIMATED_MEDIA_FLAG;
            if (reflective) flags |= MYTH2_MESH_CELL_HAS_REFLECTION_FLAG;

            int A = y * vertexW + x;
            int B = y * vertexW + (x + 1);
            int C = (y + 1) * vertexW + (x + 1);
            int D = (y + 1) * vertexW + x;
            int16_t physH = heights[(size_t)A];

            int16_t mediaH = 0;
            if (hasWaterObj && (w0 || w1)) {
                mediaH = waterHeights[(size_t)A];
            }

            int16_t renderH = physH;
            // For cells with media, render height rides on the media surface.
            // For non-media cells adjacent to media or on the map edge,
            // the engine typically uses phys or a sentinel. We use phys here.
            if (w0 || w1) {
                float medf = (float)mediaH * WORLD_HEIGHT_SCALE;
                float physf = (float)physH * WORLD_HEIGHT_SCALE;
                renderH = (medf >= physf) ? mediaH : physH;
            }

            double halfW = (double)(cellW - 1) * 0.5;
            double halfH = (double)cellH * 0.5;

            NormalVec3 pA{halfW - (double)x,     (double)heights[(size_t)A] * WORLD_HEIGHT_SCALE, (double)y - halfH};
            NormalVec3 pB{halfW - (double)(x+1), (double)heights[(size_t)B] * WORLD_HEIGHT_SCALE, (double)y - halfH};
            NormalVec3 pC{halfW - (double)(x+1), (double)heights[(size_t)C] * WORLD_HEIGHT_SCALE, (double)(y+1) - halfH};
            NormalVec3 pD{halfW - (double)x,     (double)heights[(size_t)D] * WORLD_HEIGHT_SCALE, (double)(y+1) - halfH};

            NormalVec3 n0, n1;
            if (((x ^ y) & 1) == 0) {
                n0 = normalNormalized(normalCross(normalSub(pB, pA), normalSub(pC, pA)));
                n1 = normalNormalized(normalCross(normalSub(pC, pA), normalSub(pD, pA)));
            } else {
                n0 = normalNormalized(normalCross(normalSub(pB, pA), normalSub(pD, pA)));
                n1 = normalNormalized(normalCross(normalSub(pC, pB), normalSub(pD, pB)));
            }

            if (((x ^ y) & 1) == 0) std::swap(n0, n1);

            uint8_t ni0 = nearestNormalIndex(n0, normalTable);
            uint8_t ni1 = nearestNormalIndex(n1, normalTable);
            uint16_t normalWord = ((uint16_t)ni0 << 8) | (uint16_t)ni1;

            writeBE16(cellData.data(), off + 0, (uint16_t)physH);
            writeBE16(cellData.data(), off + 2, normalWord);
            writeBE16(cellData.data(), off + 4, flags);
            writeBE16(cellData.data(), off + 6, 0xFFFF);
            writeBE16(cellData.data(), off + 8, (uint16_t)mediaH);
            writeBE16(cellData.data(), off + 10, (uint16_t)renderH);
        }
    }

    // Second pass: vertex media flag (bit 8)
    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            size_t off = (size_t)(y * cellW + x) * 12;
            uint16_t flags = readBE16u(cellData.data(), off + 4);
            bool wet = false;
            for (int oy = -1; oy <= 0 && !wet; oy++) {
                for (int ox = -1; ox <= 0 && !wet; ox++) {
                    int cx = x + ox, cy = y + oy;
                    if (cx < 0 || cy < 0 || cx >= cellW || cy >= cellH) continue;
                    uint16_t nf = readBE16u(cellData.data(), (size_t)(cy * cellW + cx) * 12 + 4);
                    wet = wet || (nf & MYTH2_MESH_CELL_TRIANGLE0_IS_MEDIA_FLAG) != 0
                              || (nf & MYTH2_MESH_CELL_TRIANGLE1_IS_MEDIA_FLAG) != 0;
                }
            }
            if (wet) flags |= MYTH2_MESH_VERTEX_IS_MEDIA_FLAG;
            writeBE16(cellData.data(), off + 4, flags);
        }
    }

    printf("Cell grid built: %d cells\n", totalCells);

    // ---- Collect instances from JSON and build unit types ----
    std::vector<JsonInst> jsonInsts;
    std::unordered_map<std::string, int> srcMarkerType;

    auto parseJsonInstArray = [&](const std::string& path, const std::string& arrayKey, int markerType) {
        std::string content = readTextFile(path);
        if (content.empty()) return;
        size_t arrStart = content.find("\"" + arrayKey + "\":");
        if (arrStart == std::string::npos) return;
        arrStart = content.find('[', arrStart);
        if (arrStart == std::string::npos) return;
        size_t pos = arrStart;
        while (true) {
            size_t ob = content.find('{', pos);
            if (ob == std::string::npos) break;
            // Brace-match to find the matching '}'
            int depth = 1;
            size_t cb = ob + 1;
            while (cb < content.size() && depth > 0) {
                if (content[cb] == '{') depth++;
                else if (content[cb] == '}') depth--;
                if (depth > 0) cb++;
            }
            if (depth != 0) break;
            std::string obj = content.substr(ob, cb - ob + 1);
            std::string tag = jsonString(obj, "tag", "");
            if (tag.empty()) { pos = cb + 1; continue; }
            JsonInst ji;
            ji.tag = tag;
            ji.x = jsonDouble(obj, "x", 0.0);
            ji.y = jsonDouble(obj, "y", 0.0);
            ji.z = jsonDouble(obj, "z", 0.0);
            ji.facingDeg = jsonDouble(obj, "facing_deg", 0.0);
            ji.palIdx = jsonInt(obj, "pal_idx", 0);
            ji.markerIdx = jsonInt(obj, "marker_idx", -1);
            if (ji.markerIdx < 0) { pos = cb + 1; continue; }
            jsonInsts.push_back(ji);
            srcMarkerType[tag] = markerType;
            pos = cb + 1;
        }
    };

    parseJsonInstArray(folder + "/assets/sprites/units.json", "units", 3);
    parseJsonInstArray(folder + "/assets/sprites/scenery.json", "scenery", 1);
    parseJsonInstArray(folder + "/assets/sounds/sounds.json", "sounds", 5);
    parseJsonInstArray(folder + "/assets/models/projectiles.json", "projectiles", 9);

    printf("Parsed %zu JSON instances\n", jsonInsts.size());

    // Build unit types by (marker_type, tag) pair, preserving original JSON order.
    struct TagMarkerKey { uint32_t tag; uint16_t markerType; };
    struct TagKeyHash {
        size_t operator()(const TagMarkerKey& k) const {
            return (size_t)k.tag ^ ((size_t)k.markerType << 16);
        }
    };
    struct TagKeyEq {
        bool operator()(const TagMarkerKey& a, const TagMarkerKey& b) const {
            return a.tag == b.tag && a.markerType == b.markerType;
        }
    };

    std::unordered_map<TagMarkerKey, int, TagKeyHash, TagKeyEq> keyToOrder;
    std::vector<UnitTypeDef> unitTypes;

    // First pass: discover unique (marker_type, tag) pairs in JSON order.
    // This preserves the original binary's type ordering since the JSON
    // was exported in the same order.
    for (const auto& ji : jsonInsts) {
        uint32_t ttag = tagFromString(ji.tag);
        uint16_t mt = (uint16_t)srcMarkerType[ji.tag];
        TagMarkerKey key{ttag, mt};
        if (keyToOrder.find(key) == keyToOrder.end()) {
            UnitTypeDef utd;
            utd.markerType = mt;
            utd.typeTag = ttag;
            utd.instanceCount = 0;
            utd.uniqueIdx = (int)unitTypes.size();
            keyToOrder[key] = (int)unitTypes.size();
            unitTypes.push_back(utd);
        }
    }

    // Build marker_type -> type index groups (preserving order)
    std::unordered_map<uint16_t, std::vector<int>> markerTypeToTypeIdxs;
    for (int i = 0; i < (int)unitTypes.size(); i++) {
        markerTypeToTypeIdxs[unitTypes[i].markerType].push_back(i);
    }

    // Build pal_idx -> flat type index lookup
    // For each marker type, pal_idx = position in first-appearance order
    std::unordered_map<int, int> palIdxToTypeIdx;
    for (auto& kv : markerTypeToTypeIdxs) {
        for (int j = 0; j < (int)kv.second.size(); j++) {
            palIdxToTypeIdx[kv.second[j]] = j;
        }
    }

    // Also build tag -> flat type idx map
    std::unordered_map<uint32_t, int> tagToTypeIdx;
    for (int i = 0; i < (int)unitTypes.size(); i++) {
        tagToTypeIdx[unitTypes[i].typeTag] = i;
    }

    // Count instances per type
    for (const auto& ji : jsonInsts) {
        uint32_t ttag = tagFromString(ji.tag);
        auto it = tagToTypeIdx.find(ttag);
        if (it != tagToTypeIdx.end()) unitTypes[(size_t)it->second].instanceCount++;
    }

    printf("Built %d unit type definitions\n", (int)unitTypes.size());

    // ---- Build object instance records ----
    struct PlacedInstance {
        int markerType = 0;
        int markerIdx = 0;
        uint32_t typeTag = 0;
        int32_t xw = 0, yw = 0, zw = 0;
        uint16_t facing = 0;
        int cellX = 0, cellY = 0;
        int nextIdx = -1;
    };
    std::vector<PlacedInstance> instances;

    for (const auto& ji : jsonInsts) {
        PlacedInstance pi;
        pi.markerIdx = ji.markerIdx;
        pi.markerType = (int)srcMarkerType[ji.tag];
        pi.typeTag = tagFromString(ji.tag);
        pi.xw = (int32_t)lround(ji.x * 512.0);
        pi.yw = (int32_t)lround(ji.y * 512.0);
        pi.zw = (int32_t)lround(ji.z * 512.0);
        pi.facing = (uint16_t)((uint32_t)lround(ji.facingDeg * (65536.0 / 360.0)) & 0xFFFF);
        double cx = ji.x + (double)cellW * 0.5;
        double cy = ji.z + (double)cellH * 0.5;
        pi.cellX = (int)std::floor(cx);
        pi.cellY = (int)std::floor(cy);
        if (pi.cellX < 0) pi.cellX = 0;
        if (pi.cellY < 0) pi.cellY = 0;
        if (pi.cellX >= cellW) pi.cellX = cellW - 1;
        if (pi.cellY >= cellH) pi.cellY = cellH - 1;
        instances.push_back(pi);
    }

    // Build per-cell linked list
    std::vector<std::vector<int>> cellInstances((size_t)totalCells);
    for (int i = 0; i < (int)instances.size(); i++) {
        int ci = instances[(size_t)i].cellY * cellW + instances[(size_t)i].cellX;
        if (ci >= 0 && ci < totalCells) cellInstances[(size_t)ci].push_back(i);
    }

    for (int cy = 0; cy < cellH; cy++) {
        for (int cx = 0; cx < cellW; cx++) {
            int ci = cy * cellW + cx;
            auto& list = cellInstances[(size_t)ci];
            if (list.empty()) continue;
            std::sort(list.begin(), list.end(), [&](int a, int b) {
                return instances[(size_t)a].markerIdx < instances[(size_t)b].markerIdx;
            });
            for (int ii = 0; ii < (int)list.size(); ii++) {
                int idx = list[(size_t)ii];
                instances[(size_t)idx].nextIdx = (ii + 1 < (int)list.size()) ? list[(size_t)ii + 1] : -1;
            }
            size_t cellOff = (size_t)ci * 12;
            writeBE16(cellData.data(), cellOff + 6, (uint16_t)list[0]);
        }
    }

    printf("Linked %d instances into cell grid\n", (int)instances.size());

    // ---- Build unit type binary data ----
    size_t unitTypeSize = unitTypes.size() * 32;
    std::vector<uint8_t> unitTypeData(unitTypeSize, 0);
    for (size_t i = 0; i < unitTypes.size(); i++) {
        size_t o = i * 32;
        writeBE16(unitTypeData.data(), o + 0, unitTypes[i].markerType);
        writeBE32(unitTypeData.data(), o + 4, unitTypes[i].typeTag);
        writeBE16(unitTypeData.data(), o + 8, 0xFFFF);
        writeBE16(unitTypeData.data(), o + 14, 0xFFFF);
        writeBE16(unitTypeData.data(), o + 28, (uint16_t)unitTypes[i].instanceCount);
        writeBE16(unitTypeData.data(), o + 30, (uint16_t)i);
    }
    printf("Unit type section: %zu bytes (%d types)\n", unitTypeSize, (int)unitTypes.size());

    // ---- Build instance binary data ----
    size_t instanceSize = instances.size() * 64;
    std::vector<uint8_t> instanceData(instanceSize, 0);
    // Build tag -> flat palette index map
    std::unordered_map<uint32_t, int> tagToPalIdx;
    for (int i = 0; i < (int)unitTypes.size(); i++)
        tagToPalIdx[unitTypes[i].typeTag] = i;

    for (size_t i = 0; i < instances.size(); i++) {
        size_t o = i * 64;
        const auto& pi = instances[i];

        int flatPalIdx = 0;
        auto it = tagToPalIdx.find(pi.typeTag);
        if (it != tagToPalIdx.end()) flatPalIdx = it->second;

        // Instance record format (export_map_objects.cpp:538-557):
        // [0:4]   flags (u32)
        // [4:6]   marker_type (s16) — 0=team, 1=scenery, 3=monster, 5=sound, 9=projectile
        // [6:8]   palette_index (u16) — index into marker palette (unit types)
        // [8:10]  identifier (s16)
        // [10:12] minimum_difficulty_level (s16)
        // [12:16] position.x (s32, world_distance, *512 = cell)
        // [16:20] position.y (s32)
        // [20:24] position.z (s32)
        // [24:30] velocity (3× s16)
        // [30:32] height_above_ground (s16)
        // [32:34] yaw (u16, 0..65535 = 360°)
        // [34:36] pitch (u16)
        // [36:52] user_data[16]
        // [52:54] roll (u16)
        // [54:56] unused
        // [56:60] render_chain (ptr, zero on disk)
        // [60:62] data_index / next_object_index (s16)
        // [62:64] data_identifier (s16)

        writeBE32(instanceData.data(), o + 0, 0); // flags
        writeBE16(instanceData.data(), o + 4, (uint16_t)pi.markerType);
        writeBE16(instanceData.data(), o + 6, (uint16_t)flatPalIdx);
        writeBE16(instanceData.data(), o + 8, (uint16_t)pi.markerIdx); // identifier = markerIdx
        writeBE16(instanceData.data(), o + 10, 0); // min difficulty
        writeBE32(instanceData.data(), o + 12, (uint32_t)pi.xw); // position.x
        writeBE32(instanceData.data(), o + 16, (uint32_t)pi.yw); // position.y
        writeBE32(instanceData.data(), o + 20, (uint32_t)pi.zw); // position.z
        // [24:30] velocity zeros
        writeBE16(instanceData.data(), o + 30, 0); // height above ground
        writeBE16(instanceData.data(), o + 32, pi.facing); // yaw
        writeBE16(instanceData.data(), o + 34, 0); // pitch
        // [36:52] user_data zeros
        writeBE16(instanceData.data(), o + 52, 0); // roll
        // [54:60] unused + render_chain zeros
        writeBE16(instanceData.data(), o + 60, (uint16_t)(pi.nextIdx >= 0 ? pi.nextIdx : 0xFFFF));
        writeBE16(instanceData.data(), o + 62, 0); // data_identifier
    }
    printf("Instance section: %zu bytes (%d instances)\n", instanceSize, (int)instances.size());

    // ---- Build action buffer ----
    std::string actionsContent = readTextFile(folder + "/assets/actions/actions.json");
    std::vector<ActionEntry> actions;
    if (!actionsContent.empty()) {
        size_t arrStart = actionsContent.find("\"actions\"");
        if (arrStart == std::string::npos) { fprintf(stderr, "No actions array found\n"); return 1; }
        arrStart = actionsContent.find('[', arrStart);
        if (arrStart == std::string::npos) { fprintf(stderr, "No actions array\n"); return 1; }

        size_t pos = arrStart;
        while (true) {
            size_t ob = actionsContent.find('{', pos);
            if (ob == std::string::npos) break;
            // Brace-match to find the matching '}'
            int depth = 1;
            size_t cb = ob + 1;
            while (cb < actionsContent.size() && depth > 0) {
                if (actionsContent[cb] == '{') depth++;
                else if (actionsContent[cb] == '}') depth--;
                if (depth > 0) cb++;
            }
            if (depth != 0) break;
            std::string obj = actionsContent.substr(ob, cb - ob + 1);

            ActionEntry ae;
            ae.id = (uint16_t)jsonInt(obj, "id", 0);
            ae.expirationMode = (uint16_t)jsonInt(obj, "expiration_mode_id", 0);
            ae.flags = (uint32_t)jsonInt(obj, "flags_raw", 0);
            ae.indent = (uint16_t)jsonInt(obj, "indent", 0);
            std::string typeStr = jsonString(obj, "type", "");
            if (!typeStr.empty()) ae.type = tagFromString(typeStr);
            ae.triggerLower = (uint32_t)lround(jsonDouble(obj, "trigger_time_lower_bound_seconds", 0.0) * 30.0);
            ae.triggerDelta = (uint32_t)lround(jsonDouble(obj, "trigger_time_delta_seconds", 0.0) * 30.0);
            ae.paramOffset = (uint32_t)jsonInt(obj, "parameter_data_offset", 0);
            ae.paramSize = (uint16_t)jsonInt(obj, "parameter_data_size", 0);

            // Count numParams from JSON parameters array (+1 for action name as string param)
            ae.numParams = 0;
            size_t parArrPos = obj.find("\"parameters\"");
            if (parArrPos != std::string::npos) {
                parArrPos = obj.find('[', parArrPos);
                if (parArrPos != std::string::npos) {
                    size_t pp = parArrPos;
                    while (true) {
                        pp = obj.find('{', pp + 1);
                        if (pp == std::string::npos || pp > obj.size()) break;
                        if (pp > parArrPos + obj.size() - parArrPos - 2) break;
                        ae.numParams++;
                        size_t close = obj.find('}', pp);
                        if (close == std::string::npos) break;
                        pp = close;
                    }
                }
            }
            // The action name is stored as a string parameter entry in the binary
            ae.numParams++;

            std::string hex = jsonString(obj, "parameter_data_hex", "");
            for (size_t hi = 0; hi + 1 < hex.size(); hi += 2) {
                int hn = 0, ln = 0;
                char c1 = hex[hi], c2 = hex[hi+1];
                if (c1 >= '0' && c1 <= '9') hn = c1 - '0';
                else if (c1 >= 'a' && c1 <= 'f') hn = c1 - 'a' + 10;
                else if (c1 >= 'A' && c1 <= 'F') hn = c1 - 'A' + 10;
                else continue;
                if (c2 >= '0' && c2 <= '9') ln = c2 - '0';
                else if (c2 >= 'a' && c2 <= 'f') ln = c2 - 'a' + 10;
                else if (c2 >= 'A' && c2 <= 'F') ln = c2 - 'A' + 10;
                else continue;
                ae.paramData.push_back((uint8_t)((hn << 4) | ln));
            }
            actions.push_back(ae);
            pos = cb + 1;
        }
    }

    size_t actionHeaderSize = actions.size() * 64;
    // JSON param_offsets are relative to the param data area (after all headers).
    // Compute total param area size max(end_of_param).
    size_t paramAreaSize = 0;
    for (const auto& ae : actions) {
        size_t end = (size_t)ae.paramOffset + ae.paramData.size();
        if (end > paramAreaSize) paramAreaSize = end;
    }
    size_t actionBufSize = actionHeaderSize + paramAreaSize;

    std::vector<uint8_t> actionBuf(actionBufSize, 0);
    for (size_t i = 0; i < actions.size(); i++) {
        const auto& ae = actions[i];
        size_t hOff = i * 64;
        // Binary paramOff is stored relative to END of headers (not absolute buffer position)
        uint32_t storedParamOff = ae.paramOffset;
        size_t actualParamOff = actionHeaderSize + (size_t)ae.paramOffset;
        writeBE16(actionBuf.data(), hOff + 0, ae.id);
        writeBE16(actionBuf.data(), hOff + 2, ae.expirationMode);
        writeBE32(actionBuf.data(), hOff + 4, ae.type);
        writeBE32(actionBuf.data(), hOff + 8, ae.flags);
        writeBE32(actionBuf.data(), hOff + 12, ae.triggerLower);
        writeBE32(actionBuf.data(), hOff + 16, ae.triggerDelta);
        writeBE16(actionBuf.data(), hOff + 20, ae.numParams);
        writeBE16(actionBuf.data(), hOff + 22, ae.paramSize);
        writeBE32(actionBuf.data(), hOff + 24, storedParamOff);
        writeBE16(actionBuf.data(), hOff + 28, ae.indent);
        if (!ae.paramData.empty() && actualParamOff + ae.paramData.size() <= actionBufSize)
            memcpy(actionBuf.data() + actualParamOff, ae.paramData.data(), ae.paramData.size());
    }
    printf("Action section: %zu bytes (%d actions)\n", actionBufSize, (int)actions.size());

    // ---- Assemble sections ----
    size_t unitTypeOff = cellDataSize;
    size_t instanceOff = unitTypeOff + unitTypeSize;
    size_t actionOff = instanceOff + instanceSize;
    size_t dataSize = actionOff + actionBufSize;

    std::vector<uint8_t> meshData;
    // From scratch: zero buffer, write every known header field at correct offsets.
    // From Python headers: mesh_header starts at file offset 0 in raw mesh_tag.bin.
    meshData.assign(1024 + dataSize, 0);

        // [0-3]   landscape_collection_tag (4s)
        memcpy(meshData.data() + 0, landTag.c_str(), 4);
        // [4-7]   media_tag (4s)
        memcpy(meshData.data() + 4, mediaTag.c_str(), 4);
        // [8-9]   submesh_width (H)
        writeBE16(meshData.data(), 8, (uint16_t)subW);
        // [10-11] submesh_height (H)
        writeBE16(meshData.data(), 10, (uint16_t)subH);
        // [12-15] mesh_offset (L) = offset to cell data from data_start (always 0)
        writeBE32(meshData.data(), 12, 0);
        // [16-19] mesh_size (L) = cell grid size
        writeBE32(meshData.data(), 16, (uint32_t)cellDataSize);
        // [24-27] data_offset (L) = usually 1024
        writeBE32(meshData.data(), 24, 1024);
        // [28-31] data_size (L) = total data after this header
        writeBE32(meshData.data(), 28, (uint32_t)dataSize);

        // [36-39] marker_palette_entries (L)
        writeBE32(meshData.data(), 36, (uint32_t)unitTypes.size());
        // [40-43] marker_palette_offset (L)
        writeBE32(meshData.data(), 40, (uint32_t)unitTypeOff);
        // [44-47] marker_palette_size (L)
        writeBE32(meshData.data(), 44, (uint32_t)unitTypeSize);
        // [52-55] marker_count (L)
        writeBE32(meshData.data(), 52, (uint32_t)instances.size());
        // [56-59] markers_offset (L)
        writeBE32(meshData.data(), 56, (uint32_t)instanceOff);
        // [60-63] markers_size (L)
        writeBE32(meshData.data(), 60, (uint32_t)instanceSize);

        // [68-71] mesh_lighting_tag: 0 (none)
        // [72-75] connector_tag: 0 (none)
        // [76-79] flags: from manifest (SINGLE_PLAYER_MAP=0x10000, etc.)
        writeBE32(meshData.data(), 76, meshFlags);
        // [80-83] particle_system_tag: 0 (none)

        // [84-87] team_count: 0
        // [88-115]: lighting, ceiling, vtfl: 0

        // [116-123]: edge_of_mesh_buffer_zones: 0

        // [124-127]: global_ambient_sound_tag: 0 (none)

        // [128-131] map_action_count (L)
        writeBE32(meshData.data(), 128, (uint32_t)actions.size());
        // [132-135] map_actions_offset (L)
        writeBE32(meshData.data(), 132, (uint32_t)actionOff);
        // [136-139] map_action_buffer_size (L)
        writeBE32(meshData.data(), 136, (uint32_t)actionBufSize);
        // [140-143] map_description_string_list_tag: from manifest (name STLI)
        std::string nameStli = jsonStringAt(manifest, 0, "map_name_stli", "");
        if (!nameStli.empty()) memcpy(meshData.data() + 140, nameStli.c_str(), 4);

        // [144-147] postgame_collection_tag: from manifest
        std::string postgame = jsonStringAt(manifest, 0, "postgame_256", "");
        if (!postgame.empty()) memcpy(meshData.data() + 144, postgame.c_str(), 4);
        // [148-151] pregame_collection_tag: from manifest
        std::string pregame = jsonStringAt(manifest, 0, "pregame_256", "");
        if (!pregame.empty()) memcpy(meshData.data() + 148, pregame.c_str(), 4);
        // [152-155] overhead_map_collection_tag: from manifest
        std::string overhead = jsonStringAt(manifest, 0, "overhead_256", "");
        if (!overhead.empty()) memcpy(meshData.data() + 152, overhead.c_str(), 4);
        // [156-191]: next_mesh, cutscene, storyline tags: 0

        // [192-195] media_coverage_region_offset: 0 (engine regenerates)
        // [196-199] media_coverage_region_size: 0
        // [204-207] mesh_LOD_data_offset: 0
        // [208-211] mesh_LOD_data_size: 0
        // [216-226]: tint color, fraction: 0

        // [228-243]: wind_tag (4s), screen_collections: 0
        // [244-251]: blood_color: 0
        // [252-267]: picture_caption_tag, narration, ambient sounds: 0
        // [256-259] narration_sound_tag: from manifest
        std::string narrSound = jsonStringAt(manifest, 0, "narration_sound", "");
        if (!narrSound.empty()) memcpy(meshData.data() + 256, narrSound.c_str(), 4);
        // [260-263] win_ambient_sound_tag: from manifest
        std::string winSound = jsonStringAt(manifest, 0, "win_ambient_sound", "");
        if (!winSound.empty()) memcpy(meshData.data() + 260, winSound.c_str(), 4);
        // [264-267] loss_ambient_sound_tag: from manifest
        std::string lossSound = jsonStringAt(manifest, 0, "loss_ambient_sound", "");
        if (!lossSound.empty()) memcpy(meshData.data() + 264, lossSound.c_str(), 4);

        // [268-283]: reverb: 0
        // [284-287] connector_count: 0
        // [288-291] connectors_offset: 0
        // [292-295] connectors_size: 0

        // [300-491]: cutscene file paths: all 0

        // [492-515]: hints, fog, difficulty, plugin_name: 0

        // [548-587]: extra_flags, narration tags, zoom: 0

        // [588-993]: padding: all 0 (from zero init)

        // [994-1007]: runtime fields: 0
        // [1008-1011] editor_data_cookie: 0
        // [1012-1015] editor_data_size: 0
        // [1016-1019] editor_data_offset: 0

    // Cell grid at data-relative offset 0 (file offset 1024)
    if (1024 + cellDataSize > meshData.size()) {
        fprintf(stderr, "Buffer too small for cell grid\n");
        return 1;
    }
    memcpy(meshData.data() + 1024, cellData.data(), cellDataSize);

    // Unit types
    if (unitTypeSize > 0) {
        size_t utFileOff = 1024 + (size_t)unitTypeOff;
        if (utFileOff + unitTypeSize <= meshData.size())
            memcpy(meshData.data() + utFileOff, unitTypeData.data(), unitTypeSize);
    }

    // Instances
    if (instanceSize > 0) {
        size_t instFileOffBase = 1024 + (size_t)instanceOff;
        if (instFileOffBase + instanceSize <= meshData.size())
            memcpy(meshData.data() + instFileOffBase, instanceData.data(), instanceSize);
    }
    // Update cell grid firstObjectIndex
    for (int cy = 0; cy < cellH; cy++) {
        for (int cx = 0; cx < cellW; cx++) {
            int ci = cy * cellW + cx;
            uint16_t foi = readBE16u(cellData.data(), (size_t)ci * 12 + 6);
            size_t cellFileOff = 1024 + (size_t)ci * 12 + 6;
            if (cellFileOff + 2 <= meshData.size())
                writeBE16(meshData.data(), cellFileOff, foi);
        }
    }
    // Update instance nextIdx
    for (size_t i = 0; i < instanceSize / 64 && i * 64 + 62 <= instanceData.size(); i++) {
        size_t instFileOff = 1024 + (size_t)instanceOff + i * 64 + 60;
        if (instFileOff + 2 <= meshData.size()) {
            uint16_t nextIdx = readBE16u(instanceData.data(), i * 64 + 60);
            writeBE16(meshData.data(), instFileOff, nextIdx);
        }
    }

    // Actions
    if (actionBufSize > 0) {
        size_t actFileOff = 1024 + (size_t)actionOff;
        if (actFileOff + actionBufSize <= meshData.size())
            memcpy(meshData.data() + actFileOff, actionBuf.data(), actionBufSize);
    }

    // ---- Write output ----
    FILE* f = fopen(outputPath.c_str(), "wb");
    if (!f) { fprintf(stderr, "Cannot create: %s\n", outputPath.c_str()); return 1; }
    fwrite(meshData.data(), 1, meshData.size(), f);
    fclose(f);

    size_t totalSize = meshData.size();
    printf("\nWrote: %s (%zu bytes)\n", outputPath.c_str(), totalSize);
    printf("  Header:     %zu bytes\n", headerSize);
    printf("  Cell grid:  %zu bytes (%dx%d cells)\n", cellDataSize, cellW, cellH);
    printf("  Unit types: %zu bytes (%d types)\n", unitTypeSize, (int)unitTypes.size());
    printf("  Instances:  %zu bytes (%d instances)\n", instanceSize, (int)instances.size());
    printf("  Actions:    %zu bytes (%d actions)\n", actionBufSize, (int)actions.size());

    // ---- Compare ----
    if (!comparePath.empty()) {
        auto orig = readFile(comparePath);
        auto built = readFile(outputPath);
        if (orig.empty()) { fprintf(stderr, "Cannot read original: %s\n", comparePath.c_str()); return 1; }
        printf("\n=== Comparison with %s ===\n", comparePath.c_str());
        printf("Original: %zu bytes\n", orig.size());
        printf("Built:    %zu bytes\n", built.size());
        int headerDiffs = 0, cellDiffs = 0, utDiffs = 0, instDiffs = 0, actDiffs = 0;
        size_t minLen = std::min(orig.size(), built.size());
        for (size_t i = 0; i < minLen; i++) {
            if (orig[i] == built[i]) continue;
            if (i < 1024) headerDiffs++;
            else if (i - 1024 < cellDataSize) cellDiffs++;
            else if (i - 1024 < unitTypeOff + unitTypeSize) utDiffs++;
            else if (i - 1024 < instanceOff + instanceSize) instDiffs++;
            else if (i - 1024 < actionOff + actionBufSize) actDiffs++;
        }
        printf("\nSection diff summary:\n");
        printf("  Header:       %d diffs\n", headerDiffs);
        printf("  Cell grid:    %d diffs\n", cellDiffs);
        printf("  Unit types:   %d diffs\n", utDiffs);
        printf("  Instances:    %d diffs\n", instDiffs);
        printf("  Actions:      %d diffs\n", actDiffs);
        if (orig.size() != built.size())
            printf("  Size mismatch: orig=%zu built=%zu\n", orig.size(), built.size());
        if (headerDiffs+cellDiffs+utDiffs+instDiffs+actDiffs == 0 && orig.size() == built.size())
            printf("\n*** Files are byte-identical! ***\n");
    }

    return 0;
}
