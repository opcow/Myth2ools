// mesh_import.cpp
// Read a Wavefront OBJ exported by mesh and patch Myth II terrain
// displacement back into raw/mesh_tag.bin.
//
// Usage:
//   mesh_import <tag_folder> <input.obj> [heightscale]
//
// Notes:
//   - This importer updates physical_height and regenerates the packed normal word.
//   - The on-disk render_height field is specially encoded, so it is preserved.
//   - flags, first_object_index, and media_height are preserved.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

static constexpr int FULL = 1024;
static constexpr int Q14_ONE = 0x4000;
static constexpr int TABLE_SIZE = 256;
static constexpr double WORLD_HEIGHT_SCALE = 1.0 / 512.0;

static uint16_t swap16(uint16_t n) { return (uint16_t)((n >> 8) | (n << 8)); }
static int16_t readBE16s(const uint8_t* b, size_t o) {
    uint16_t v;
    memcpy(&v, b + o, 2);
    return (int16_t)swap16(v);
}
static void writeBE16s(uint8_t* b, size_t o, int16_t v) {
    uint16_t u = (uint16_t)v;
    b[o] = (uint8_t)(u >> 8);
    b[o + 1] = (uint8_t)(u & 0xFF);
}

static std::vector<uint8_t> readFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fclose(f);
        return {};
    }
    std::vector<uint8_t> buf((size_t)sz);
    if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) {
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
    bool ok = (fwrite(data.data(), 1, data.size(), f) == data.size());
    fclose(f);
    return ok;
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

struct Manifest {
    std::string meshTag;
    int submeshWidth = 0;
    int submeshHeight = 0;
    int dataOffset = 1024;
    int meshOffset = 0;
    int meshSize = 0;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct RuntimeEntry {
    Vec3 n;
};

static Vec3 sub(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

static Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double length(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

static Vec3 normalized(const Vec3& v) {
    double len = length(v);
    if (len < 1e-12) return Vec3{0.0, 1.0, 0.0};
    return Vec3{v.x / len, v.y / len, v.z / len};
}

static double clamp1(double v) {
    if (v < -1.0) return -1.0;
    if (v > 1.0) return 1.0;
    return v;
}

static double angleDeg(const Vec3& a, const Vec3& b) {
    return std::acos(clamp1(dot(a, b))) * (180.0 / 3.14159265358979323846);
}

static int16_t q14FromFloat(double v) {
    long n = lround(v * (double)Q14_ONE);
    if (n < -32768) n = -32768;
    if (n > 32767) n = 32767;
    return (int16_t)n;
}

static void buildTrigTables(std::vector<int16_t>& sinQ14, std::vector<int16_t>& cosQ14) {
    sinQ14.resize(FULL);
    cosQ14.resize(FULL);
    const double twoPi = 6.28318530717958647692;
    for (int i = 0; i < FULL; i++) {
        double a = ((double)i * twoPi) / (double)FULL;
        sinQ14[(size_t)i] = q14FromFloat(std::sin(a));
        cosQ14[(size_t)i] = q14FromFloat(std::cos(a));
    }
    sinQ14[0]   = 0;         cosQ14[0]   =  Q14_ONE;
    sinQ14[256] = Q14_ONE;   cosQ14[256] =  0;
    sinQ14[512] = 0;         cosQ14[512] = -Q14_ONE;
    sinQ14[768] = -Q14_ONE;  cosQ14[768] =  0;
}

static std::vector<RuntimeEntry> buildRuntimeTable() {
    std::vector<int16_t> sinQ14, cosQ14;
    buildTrigTables(sinQ14, cosQ14);

    std::vector<RuntimeEntry> out(TABLE_SIZE);
    int entry = 0;
    int local8 = 0x4020;
    while (entry < TABLE_SIZE) {
        unsigned pitchIdx = (unsigned)((local8 >> 6) & 0x3FF);
        int ringCount = (((int)cosQ14[pitchIdx] << 2) / 0x444) + 1;
        if (ringCount > 0) {
            int localC = 0x20;
            while (ringCount > 0 && entry < TABLE_SIZE) {
                unsigned azIdx = (unsigned)((localC >> 6) & 0x3FF);
                int16_t x = (int16_t)(((int)cosQ14[azIdx] * (int)cosQ14[pitchIdx]) >> 14);
                int16_t y = (int16_t)(((int)sinQ14[azIdx] * (int)cosQ14[pitchIdx]) >> 14);
                int16_t z = sinQ14[pitchIdx];
                out[(size_t)entry].n = normalized(Vec3{
                    (double)x / (double)Q14_ONE,
                    (double)y / (double)Q14_ONE,
                    (double)z / (double)Q14_ONE
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

// From the verified comparator result:
// empirical OBJ basis (x=-col, y=up, z=row) = (-z,+y,+x) applied to runtime basis.
// Therefore runtime basis = inverse: (-empirical.x, empirical.z, empirical.y).
static Vec3 empiricalToRuntime(const Vec3& v) {
    return normalized(Vec3{-v.x, v.z, v.y});
}

static uint8_t nearestRuntimeNormalIndex(const Vec3& empiricalNormal,
                                         const std::vector<RuntimeEntry>& table) {
    Vec3 runtimeNormal = empiricalToRuntime(empiricalNormal);
    double bestErr = 1e9;
    int bestIdx = 0;
    for (int i = 0; i < TABLE_SIZE; i++) {
        double err = angleDeg(runtimeNormal, table[(size_t)i].n);
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
    const bool tri0Wet = (flags & 0x0800u) != 0;
    const bool tri1Wet = (flags & 0x1000u) != 0;
    if (oldWord == 0) return true;
    if ((flags & 0x4000u) != 0) return true; // is_not_rendered
    if (tri0Type == 13 || tri1Type == 13) return true; // unused / legacy special terrain
    if (mediaHeight == (int16_t)-30981) return true; // sentinel seen in stock special cells
    if ((tri0Type <= 3 && !tri0Wet) || (tri1Type <= 3 && !tri1Wet)) return true; // legacy dry media-type cells
    return false;
}

static bool readManifest(const std::string& path, Manifest& m) {
    auto raw = readFile(path);
    if (raw.empty()) {
        fprintf(stderr, "Cannot read: %s\n", path.c_str());
        return false;
    }
    std::string j(raw.begin(), raw.end());
    m.meshTag = jsonString(j, "mesh_tag", "mesh");
    m.submeshWidth = jsonInt(j, "width", 0);
    m.submeshHeight = jsonInt(j, "height", 0);
    m.dataOffset = jsonInt(j, "data_offset", 1024);
    m.meshOffset = jsonInt(j, "mesh_offset", 0);
    m.meshSize = jsonInt(j, "mesh_size", 0);
    return m.submeshWidth > 0 && m.submeshHeight > 0;
}

static bool parseOBJ(const std::string& path, float hs,
                     int vertexWidth, int vertexHeight,
                     std::vector<int16_t>& heightsOut) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "Cannot open OBJ: %s\n", path.c_str());
        return false;
    }

    float halfW = (float)(vertexWidth - 1) * 0.5f;
    float halfH = (float)(vertexHeight - 1) * 0.5f;
    int total = vertexWidth * vertexHeight;
    heightsOut.assign((size_t)total, 0);
    std::vector<bool> filled((size_t)total, false);

    int vCount = 0, mapped = 0, clampedH = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != 'v' || line[1] != ' ') continue;
        float x, y, z;
        if (sscanf(line + 2, "%f %f %f", &x, &y, &z) != 3) continue;
        vCount++;

        int col = (int)roundf(halfW - x);
        int row = (int)roundf(z + halfH);
        if (col < 0 || col >= vertexWidth || row < 0 || row >= vertexHeight) {
            fprintf(stderr, "  Warning: vertex out of range: v %g %g %g -> (%d,%d)\n",
                    x, y, z, row, col);
            continue;
        }

        float hf = y / hs;
        int16_t h;
        if (hf < -32768.0f) { h = -32768; clampedH++; }
        else if (hf > 32767.0f) { h = 32767; clampedH++; }
        else h = (int16_t)roundf(hf);

        int idx = row * vertexWidth + col;
        heightsOut[(size_t)idx] = h;
        filled[(size_t)idx] = true;
        mapped++;
    }
    fclose(f);

    int missing = 0;
    for (int i = 0; i < total; i++) if (!filled[(size_t)i]) missing++;
    printf("  OBJ vertices: %d read, %d mapped to grid (%d missing, %d height-clamped)\n",
           vCount, mapped, missing, clampedH);
    if (missing > 0) {
        fprintf(stderr, "  Warning: %d vertex-grid points were not covered by the OBJ.\n", missing);
        fprintf(stderr, "  Heights for those points default to 0.\n");
    }
    return mapped > 0;
}

static int patchMesh(std::vector<uint8_t>& meshBin,
                     int cellOffset, int cellWidth, int cellHeight,
                     const std::vector<int16_t>& vertexHeights,
                     int vertexWidth) {
    const int cellSize = 12;
    int total = cellWidth * cellHeight;
    int hChanged = 0;
    int nChanged = 0;
    std::vector<RuntimeEntry> runtimeTable = buildRuntimeTable();
    std::vector<int16_t> originalVertexHeights((size_t)(cellWidth + 1) * (cellHeight + 1), 0);

    for (int vy = 0; vy <= cellHeight; vy++) {
        for (int vx = 0; vx <= cellWidth; vx++) {
            int cx = vx;
            int cy = vy;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx >= cellWidth) cx = cellWidth - 1;
            if (cy >= cellHeight) cy = cellHeight - 1;
            size_t off = (size_t)cellOffset + (size_t)(cy * cellWidth + cx) * cellSize;
            originalVertexHeights[(size_t)vy * vertexWidth + vx] = readBE16s(meshBin.data(), off);
        }
    }

    double halfW = (double)(vertexWidth - 1) * 0.5;
    double halfH = (double)cellHeight * 0.5;
    for (int y = 0; y < cellHeight; y++) {
        for (int x = 0; x < cellWidth; x++) {
            int cellIndex = y * cellWidth + x;
            int vertexIndex = y * vertexWidth + x;
            size_t off = (size_t)cellOffset + (size_t)cellIndex * cellSize;
            if (off + cellSize > meshBin.size()) break;
            int16_t newH = vertexHeights[(size_t)vertexIndex];
            int16_t oldH = readBE16s(meshBin.data(), off);
            if (oldH != newH) {
                writeBE16s(meshBin.data(), off, newH);
                hChanged++;
            }

            int A = y * vertexWidth + x;
            int B = y * vertexWidth + (x + 1);
            int C = (y + 1) * vertexWidth + (x + 1);
            int D = (y + 1) * vertexWidth + x;
            bool cornersChanged =
                originalVertexHeights[(size_t)A] != vertexHeights[(size_t)A] ||
                originalVertexHeights[(size_t)B] != vertexHeights[(size_t)B] ||
                originalVertexHeights[(size_t)C] != vertexHeights[(size_t)C] ||
                originalVertexHeights[(size_t)D] != vertexHeights[(size_t)D];
            if (!cornersChanged) {
                continue;
            }
            Vec3 pA{halfW - (double)x, (double)vertexHeights[(size_t)A] * WORLD_HEIGHT_SCALE, (double)y - halfH};
            Vec3 pB{halfW - (double)(x + 1), (double)vertexHeights[(size_t)B] * WORLD_HEIGHT_SCALE, (double)y - halfH};
            Vec3 pC{halfW - (double)(x + 1), (double)vertexHeights[(size_t)C] * WORLD_HEIGHT_SCALE, (double)(y + 1) - halfH};
            Vec3 pD{halfW - (double)x, (double)vertexHeights[(size_t)D] * WORLD_HEIGHT_SCALE, (double)(y + 1) - halfH};

            Vec3 n0;
            Vec3 n1;
            if (((x ^ y) & 1) == 0) {
                n0 = normalized(cross(sub(pB, pA), sub(pC, pA)));
                n1 = normalized(cross(sub(pC, pA), sub(pD, pA)));
            } else {
                n0 = normalized(cross(sub(pB, pA), sub(pD, pA)));
                n1 = normalized(cross(sub(pC, pB), sub(pD, pB)));
            }

            // Myth II stores the even-parity cell triangles in the opposite
            // slot order from the natural A-B-C / A-C-D construction above.
            if (((x ^ y) & 1) == 0) {
                std::swap(n0, n1);
            }

            uint16_t oldWord = (uint16_t)readBE16s(meshBin.data(), off + 2);
            uint16_t flags = (uint16_t)readBE16s(meshBin.data(), off + 4);
            int16_t mediaHeight = readBE16s(meshBin.data(), off + 8);
            uint16_t newWord = oldWord;
            if (!preserveLegacyNormalWord(flags, mediaHeight, oldWord)) {
                newWord = (uint16_t)((uint16_t)nearestRuntimeNormalIndex(n0, runtimeTable) << 8)
                        | (uint16_t)nearestRuntimeNormalIndex(n1, runtimeTable);
            }
            if (oldWord != newWord) {
                writeBE16s(meshBin.data(), off + 2, (int16_t)newWord);
                nChanged++;
            }
        }
    }
    printf("  Terrain displacement cells changed: %d / %d\n", hChanged, total);
    printf("  Terrain normal words changed:       %d / %d\n", nChanged, total);
    return hChanged;
}

static void usage(const char* prog) {
    fprintf(stderr,
        "Myth II Terrain Displacement Importer\n\n"
        "Usage:\n"
        "  %s <tag_folder> <input.obj> [heightscale]\n\n"
        "Arguments:\n"
        "  tag_folder    Extracted Myth II map folder from extract\n"
        "  input.obj     Modified displacement OBJ from mesh\n"
        "  heightscale   Scale used when exporting (default: 1/512)\n\n"
        "Patches raw/mesh_tag.bin in-place with new terrain displacement.\n"
        "Packed triangle normals are regenerated from the edited displacement mesh.\n"
        "flags, media_height, and encoded render_height are preserved.\n",
        prog);
}

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        usage(argv[0]);
        return 1;
    }

    std::string folder = argv[1];
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) {
        folder.pop_back();
    }
    std::string objPath = argv[2];
    float hs = (argc >= 4) ? (float)atof(argv[3]) : (1.0f / 512.0f);
    if (hs <= 0.0f) {
        fprintf(stderr, "Error: heightscale must be positive\n");
        return 1;
    }

    std::string manifestPath = folder + "/manifest.json";
    std::string binPath = folder + "/raw/mesh_tag.bin";

    printf("Myth II Terrain Mesh Importer\n");
    printf("=============================\n");
    printf("Tag folder:   %s\n", folder.c_str());
    printf("OBJ:          %s\n", objPath.c_str());
    printf("Height scale: %.9f\n\n", hs);

    printf("Step 1: Reading manifest...\n");
    Manifest mf;
    if (!readManifest(manifestPath, mf)) return 1;

    int cellWidth = mf.submeshWidth * 32;
    int cellHeight = mf.submeshHeight * 32;
    int vertexWidth = cellWidth + 1;
    int vertexHeight = cellHeight + 1;
    int cellOffset = mf.dataOffset + mf.meshOffset;
    int expectedBytes = cellWidth * cellHeight * 12;

    printf("  Mesh tag:     %s\n", mf.meshTag.c_str());
    printf("  Submeshes:    %d x %d\n", mf.submeshWidth, mf.submeshHeight);
    printf("  Cell grid:    %d x %d = %d\n", cellWidth, cellHeight, cellWidth * cellHeight);
    printf("  Vertex grid:  %d x %d = %d\n", vertexWidth, vertexHeight, vertexWidth * vertexHeight);
    printf("  Mesh offset:  %d bytes from file start\n\n", cellOffset);

    printf("Step 2: Reading %s...\n", binPath.c_str());
    std::vector<uint8_t> meshBin = readFile(binPath);
    if (meshBin.empty()) {
        fprintf(stderr, "Cannot read: %s\n", binPath.c_str());
        return 1;
    }
    printf("  File size:    %zu bytes\n", meshBin.size());
    printf("  Mesh bytes:   %d expected from grid dimensions\n", expectedBytes);
    if ((size_t)cellOffset + (size_t)expectedBytes > meshBin.size()) {
        fprintf(stderr, "Error: raw mesh data is smaller than expected\n");
        return 1;
    }
    if (mf.meshSize > 0 && mf.meshSize != expectedBytes) {
        fprintf(stderr, "  Warning: manifest mesh_size is %d but computed cell bytes are %d\n",
                mf.meshSize, expectedBytes);
    }
    printf("\n");

    printf("Step 3: Parsing OBJ vertices...\n");
    std::vector<int16_t> vertexHeights;
    if (!parseOBJ(objPath, hs, vertexWidth, vertexHeight, vertexHeights)) return 1;

    int16_t hmin = vertexHeights[0], hmax = vertexHeights[0];
    for (int16_t v : vertexHeights) {
        hmin = std::min(hmin, v);
        hmax = std::max(hmax, v);
    }
    printf("  Height range: %d .. %d (raw units)\n\n", hmin, hmax);

    printf("Step 4: Patching binary...\n");
    patchMesh(meshBin, cellOffset, cellWidth, cellHeight, vertexHeights, vertexWidth);
    printf("\n");

    printf("Step 5: Writing %s...\n", binPath.c_str());
    if (!writeFile(binPath, meshBin)) {
        fprintf(stderr, "Write failed.\n");
        return 1;
    }
    printf("  Written: %zu bytes\n\n", meshBin.size());

    printf("Done.\n");
    printf("  raw/mesh_tag.bin updated in-place.\n");
    return 0;
}
