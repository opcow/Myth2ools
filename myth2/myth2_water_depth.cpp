// myth2_water_depth.cpp
// Generate a Myth II water type bitmap from terrain and water OBJ geometry.
//
// Usage:
//   myth2_water_depth <tag_folder> <terrain.obj> <water.obj> [level1] [level2] [level3] [output.bmp] [heightscale] [--smooth]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
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
        if (c == '\\' && pos < j.size()) out.push_back(j[pos++]);
        else out.push_back(c);
    }
    return def;
}

struct Manifest {
    std::string meshTag;
    int submeshWidth = 0;
    int submeshHeight = 0;
};

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
    return m.submeshWidth > 0 && m.submeshHeight > 0;
}

struct ObjData {
    std::vector<float> y;
    std::vector<std::array<int, 3>> faces;
};

struct TriRecord {
    std::array<int, 3> v = {0, 0, 0};
    bool wet = false;
    float depth = 0.0f;
    int type = 0;
};

static bool parseOBJ(const std::string& path, ObjData& out) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "Cannot open OBJ: %s\n", path.c_str());
        return false;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            float x, y, z;
            if (sscanf(line + 2, "%f %f %f", &x, &y, &z) == 3) {
                (void)x; (void)z;
                out.y.push_back(y);
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            int a = 0, b = 0, c = 0;
            if (sscanf(line + 2, "%d/%*d/%*d %d/%*d/%*d %d/%*d/%*d", &a, &b, &c) == 3 ||
                sscanf(line + 2, "%d %d %d", &a, &b, &c) == 3) {
                out.faces.push_back({a, b, c});
            }
        }
    }
    fclose(f);
    return !out.y.empty();
}

static bool writeBMP24(const char* path, int w, int h, const std::vector<uint8_t>& rgb) {
    if ((int)rgb.size() != w * h * 3) return false;
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot create: %s\n", path);
        return false;
    }
    int stride = (w * 3 + 3) & ~3;
    int dataOff = 14 + 40;
    int fileSz = dataOff + stride * h;
    uint8_t fh[14] = {'B','M',0,0,0,0,0,0,0,0,0,0,0,0};
    auto put32 = [](uint8_t* b, int v){ b[0]=v&0xFF; b[1]=(v>>8)&0xFF; b[2]=(v>>16)&0xFF; b[3]=(v>>24)&0xFF; };
    put32(fh+2,fileSz); put32(fh+10,dataOff); fwrite(fh,1,14,f);
    uint8_t ih[40] = {}; ih[0] = 40;
    put32(ih+4,w); put32(ih+8,h); ih[12] = 1; ih[14] = 24; fwrite(ih,1,40,f);
    std::vector<uint8_t> row((size_t)stride, 0);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t* src = rgb.data() + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            row[(size_t)x * 3 + 0] = src[(size_t)x * 3 + 2];
            row[(size_t)x * 3 + 1] = src[(size_t)x * 3 + 1];
            row[(size_t)x * 3 + 2] = src[(size_t)x * 3 + 0];
        }
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    return true;
}

static uint64_t faceKey(int a, int b, int c) {
    int v[3] = {a, b, c};
    std::sort(v, v + 3);
    return ((uint64_t)(uint32_t)v[0] << 42) | ((uint64_t)(uint32_t)v[1] << 21) | (uint64_t)(uint32_t)v[2];
}

static int vertexIndex(int x, int y, int vw) {
    return y * vw + x;
}

static int classifyDepth(float depth, bool hasLevel1, float level1,
                         bool hasLevel2, float level2,
                         bool hasLevel3, float level3) {
    int type = 0;
    if (hasLevel1 && depth >= level1) type = 1;
    if (hasLevel2 && depth >= level2) type = 2;
    if (hasLevel3 && depth >= level3) type = 3;
    return type;
}

static const uint8_t WATER_TYPE_COLORS[4][3] = {
    {0x00,0x00,0xFF},
    {0x00,0x00,0xB0},
    {0x00,0x00,0x80},
    {0x00,0x00,0x40}
};

struct P2 {
    float x;
    float y;
};

static void barycentric(const P2& p, const P2& a, const P2& b, const P2& c,
                        float& w0, float& w1, float& w2) {
    float det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (det == 0.0f) {
        w0 = 1.0f; w1 = 0.0f; w2 = 0.0f;
        return;
    }
    w0 = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / det;
    w1 = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / det;
    w2 = 1.0f - w0 - w1;
}

static float sampleTriY(const std::vector<float>& yValues, const std::array<int,3>& triVerts,
                        float u, float v, bool evenDiag, int triIndex) {
    P2 p0, p1, p2;
    if (evenDiag) {
        if (triIndex == 0) {
            p0 = {0.0f, 0.0f}; p1 = {1.0f, 1.0f}; p2 = {0.0f, 1.0f};
        } else {
            p0 = {0.0f, 0.0f}; p1 = {1.0f, 0.0f}; p2 = {1.0f, 1.0f};
        }
    } else {
        if (triIndex == 0) {
            p0 = {0.0f, 0.0f}; p1 = {1.0f, 0.0f}; p2 = {0.0f, 1.0f};
        } else {
            p0 = {1.0f, 0.0f}; p1 = {1.0f, 1.0f}; p2 = {0.0f, 1.0f};
        }
    }
    float w0, w1, w2;
    barycentric({u, v}, p0, p1, p2, w0, w1, w2);
    float y0 = yValues[(size_t)triVerts[0] - 1];
    float y1 = yValues[(size_t)triVerts[1] - 1];
    float y2 = yValues[(size_t)triVerts[2] - 1];
    return y0 * w0 + y1 * w1 + y2 * w2;
}

static int smoothTypeImage(std::vector<uint8_t>& typeImg, int w, int h, int passes) {
    int totalChanged = 0;
    for (int pass = 0; pass < passes; pass++) {
        std::vector<uint8_t> next = typeImg;
        int changedThisPass = 0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int idx = y * w + x;
                uint8_t cur = typeImg[(size_t)idx];
                if (cur == 255) continue;

                int counts[4] = {0, 0, 0, 0};
                counts[cur]++;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = x + dx;
                        int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        uint8_t v = typeImg[(size_t)(ny * w + nx)];
                        if (v == 255) continue;
                        if (v < 4) counts[v]++;
                    }
                }

                int best = cur;
                int bestCount = counts[cur];
                for (int t = 0; t < 4; t++) {
                    if (counts[t] > bestCount) {
                        best = t;
                        bestCount = counts[t];
                    }
                }
                if (best != cur) {
                    next[(size_t)idx] = (uint8_t)best;
                    changedThisPass++;
                }
            }
        }
        typeImg.swap(next);
        totalChanged += changedThisPass;
        if (changedThisPass == 0) break;
    }
    return totalChanged;
}

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II Water Depth Generator\n\n"
        "Usage:\n"
        "  %s <tag_folder> <terrain.obj> <water.obj> [level1] [level2] [level3] [output.bmp] [heightscale] [--smooth]\n\n"
        "Arguments:\n"
        "  tag_folder    Extracted Myth II map folder\n"
        "  terrain.obj   Terrain OBJ from myth2_mesh\n"
        "  water.obj     Water OBJ from myth2_water_mesh\n"
        "  level1        Depth where water type 1 begins (raw units, optional)\n"
        "  level2        Depth where water type 2 begins (raw units, optional)\n"
        "  level3        Depth where water type 3 begins (raw units, optional)\n"
        "  output.bmp    Output BMP path (default: <folder>/terrain/water_generated.bmp)\n"
        "  heightscale   OBJ height scale used on export (default: 1/512)\n"
        "  --smooth      Run one 2D majority cleanup pass on the generated type image\n",
        p);
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    std::string folder = argv[1];
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();
    std::string terrainObjPath = argv[2];
    std::string waterObjPath = argv[3];
    int argi = 4;
    bool hasLevel1 = false, hasLevel2 = false, hasLevel3 = false;
    float level1 = 0.0f, level2 = 0.0f, level3 = 0.0f;
    bool smooth = false;

    auto isNumber = [](const char* s) -> bool {
        if (!s || !*s) return false;
        char* end = nullptr;
        strtof(s, &end);
        return end && *end == '\0';
    };

    if (argi < argc && isNumber(argv[argi])) { hasLevel1 = true; level1 = (float)atof(argv[argi++]); }
    if (argi < argc && isNumber(argv[argi])) { hasLevel2 = true; level2 = (float)atof(argv[argi++]); }
    if (argi < argc && isNumber(argv[argi])) { hasLevel3 = true; level3 = (float)atof(argv[argi++]); }

    std::string outPath = (argi < argc && argv[argi][0] != '-') ? argv[argi++] : (folder + "/terrain/water_generated.bmp");
    float hs = (argi < argc && argv[argi][0] != '-') ? (float)atof(argv[argi++]) : (1.0f / 512.0f);

    while (argi < argc) {
        std::string opt = argv[argi++];
        if (opt == "--smooth") {
            smooth = true;
        } else {
            fprintf(stderr, "Error: unknown option %s\n", opt.c_str());
            return 1;
        }
    }

    if (hs <= 0.0f) {
        fprintf(stderr, "Error: heightscale must be positive\n");
        return 1;
    }
    if (hasLevel2 && (!hasLevel1 || level2 < level1)) {
        fprintf(stderr, "Error: level2 must be >= level1\n");
        return 1;
    }
    if (hasLevel3 && ((hasLevel2 && level3 < level2) || (!hasLevel2 && hasLevel1 && level3 < level1))) {
        fprintf(stderr, "Error: level3 must be >= earlier levels\n");
        return 1;
    }

    auto thresholdLabel = [](bool hasValue, float value) -> std::string {
        return hasValue ? std::to_string(value) : std::string("(off)");
    };

    Manifest mf;
    if (!readManifest(folder + "/manifest.json", mf)) return 1;
    const int cellW = mf.submeshWidth * 32;
    const int cellH = mf.submeshHeight * 32;
    const int vertexW = cellW + 1;
    const int vertexH = cellH + 1;
    const int expectedVerts = vertexW * vertexH;
    const int pixPerCell = 8;
    const int outW = cellW * pixPerCell;
    const int outH = cellH * pixPerCell;

    printf("Myth II Water Depth Generator\n");
    printf("=============================\n");
    printf("Folder:        %s\n", folder.c_str());
    printf("Terrain OBJ:   %s\n", terrainObjPath.c_str());
    printf("Water OBJ:     %s\n", waterObjPath.c_str());
    std::string level1Label = thresholdLabel(hasLevel1, level1);
    std::string level2Label = thresholdLabel(hasLevel2, level2);
    std::string level3Label = thresholdLabel(hasLevel3, level3);
    printf("Thresholds:    type1=%s  type2=%s  type3=%s\n",
           level1Label.c_str(), level2Label.c_str(), level3Label.c_str());
    printf("Height scale:  %.9f\n", hs);
    printf("Smoothing:     %s\n", smooth ? "on" : "off");
    printf("Output BMP:    %s\n\n", outPath.c_str());

    ObjData terrainObj, waterObj;
    if (!parseOBJ(terrainObjPath, terrainObj)) return 1;
    if (!parseOBJ(waterObjPath, waterObj)) return 1;

    if ((int)terrainObj.y.size() != expectedVerts) {
        fprintf(stderr, "Terrain OBJ vertex count mismatch: got %zu, expected %d\n",
                terrainObj.y.size(), expectedVerts);
        return 1;
    }
    if ((int)waterObj.y.size() != expectedVerts) {
        fprintf(stderr, "Water OBJ vertex count mismatch: got %zu, expected %d\n",
                waterObj.y.size(), expectedVerts);
        return 1;
    }

    std::vector<uint64_t> waterFaceKeys;
    waterFaceKeys.reserve(waterObj.faces.size());
    for (const auto& f : waterObj.faces) waterFaceKeys.push_back(faceKey(f[0], f[1], f[2]));
    std::sort(waterFaceKeys.begin(), waterFaceKeys.end());

    std::vector<TriRecord> tris((size_t)cellW * cellH * 2);
    int triCount[4] = {0,0,0,0};
    float depthMin = 1e30f, depthMax = -1e30f;

    for (int cy = 0; cy < cellH; cy++) {
        for (int cx = 0; cx < cellW; cx++) {
            int A = vertexIndex(cx,     cy,     vertexW) + 1;
            int B = vertexIndex(cx + 1, cy,     vertexW) + 1;
            int C = vertexIndex(cx + 1, cy + 1, vertexW) + 1;
            int D = vertexIndex(cx,     cy + 1, vertexW) + 1;
            bool evenDiag = ((cx ^ cy) & 1) == 0;

            std::array<int,3> triVerts[2];
            if (evenDiag) {
                triVerts[0] = {A, C, D};
                triVerts[1] = {A, B, C};
            } else {
                triVerts[0] = {A, B, D};
                triVerts[1] = {B, C, D};
            }

            int triBase = (cy * cellW + cx) * 2;

            for (int t = 0; t < 2; t++) {
                TriRecord& tr = tris[(size_t)(triBase + t)];
                tr.v = triVerts[t];
                tr.wet = false;
                tr.depth = 0.0f;
                tr.type = 0;
                uint64_t key = faceKey(triVerts[t][0], triVerts[t][1], triVerts[t][2]);
                if (std::binary_search(waterFaceKeys.begin(), waterFaceKeys.end(), key)) {
                    tr.wet = true;
                    float terrainAvg = 0.0f;
                    float waterAvg = 0.0f;
                    for (int k = 0; k < 3; k++) {
                        int vi = triVerts[t][k] - 1;
                        terrainAvg += terrainObj.y[(size_t)vi] / hs;
                        waterAvg += waterObj.y[(size_t)vi] / hs;
                    }
                    terrainAvg /= 3.0f;
                    waterAvg /= 3.0f;
                    float depth = waterAvg - terrainAvg;
                    depthMin = std::min(depthMin, depth);
                    depthMax = std::max(depthMax, depth);
                    tr.depth = depth;
                }
            }
        }
    }

    int wetTriTotal = 0;
    for (const auto& tr : tris) if (tr.wet) wetTriTotal++;

    std::vector<uint8_t> typeImg((size_t)outW * outH, 255);
    for (int cy = 0; cy < cellH; cy++) {
        for (int cx = 0; cx < cellW; cx++) {
            bool evenDiag = ((cx ^ cy) & 1) == 0;
            int triBase = (cy * cellW + cx) * 2;
            for (int py = 0; py < pixPerCell; py++) {
                for (int px = 0; px < pixPerCell; px++) {
                    float u = ((float)px + 0.5f) / (float)pixPerCell;
                    float v = ((float)py + 0.5f) / (float)pixPerCell;
                    int tri = 0;
                    if (!evenDiag) tri = (u + v < 1.0f) ? 0 : 1;
                    else tri = (u < v) ? 0 : 1;
                    int ox = (cellW - 1 - cx) * pixPerCell + (pixPerCell - 1 - px);
                    int oy = cy * pixPerCell + py;
                    const TriRecord& tr = tris[(size_t)(triBase + tri)];
                    if (tr.wet) {
                        float terrainY = sampleTriY(terrainObj.y, tr.v, u, v, evenDiag, tri);
                        float waterY = sampleTriY(waterObj.y, tr.v, u, v, evenDiag, tri);
                        float depth = (waterY - terrainY) / hs;
                        int type = classifyDepth(depth, hasLevel1, level1, hasLevel2, level2, hasLevel3, level3);
                        typeImg[(size_t)oy * outW + ox] = (uint8_t)type;
                    }
                }
            }
        }
    }

    int smoothed = 0;
    if (smooth) {
        smoothed = smoothTypeImage(typeImg, outW, outH, 1);
    }

    std::vector<uint8_t> out((size_t)outW * outH * 3, 0);
    triCount[0] = triCount[1] = triCount[2] = triCount[3] = 0;
    for (int i = 0; i < outW * outH; i++) {
        uint8_t t = typeImg[(size_t)i];
        if (t == 255 || t > 3) continue;
        out[(size_t)i * 3 + 0] = WATER_TYPE_COLORS[t][0];
        out[(size_t)i * 3 + 1] = WATER_TYPE_COLORS[t][1];
        out[(size_t)i * 3 + 2] = WATER_TYPE_COLORS[t][2];
        triCount[t]++;
    }

    if (!writeBMP24(outPath.c_str(), outW, outH, out)) {
        fprintf(stderr, "Failed to write %s\n", outPath.c_str());
        return 1;
    }

    printf("Vertices:      terrain=%zu water=%zu\n", terrainObj.y.size(), waterObj.y.size());
    printf("Water faces:   %zu\n", waterObj.faces.size());
    printf("Wet triangles: %d\n", wetTriTotal);
    printf("Smoothed:      %d triangle reassignment(s)\n", smoothed);
    if (wetTriTotal > 0) {
        printf("Depth range:   %.2f .. %.2f (raw units)\n", depthMin, depthMax);
    }
    printf("Pixel counts:  0=%d  1=%d  2=%d  3=%d\n",
           triCount[0], triCount[1], triCount[2], triCount[3]);
    printf("\nDone.\n");
    return 0;
}
