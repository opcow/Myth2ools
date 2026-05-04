// myth2_normal_analyze.cpp
// Empirically correlate Myth II mesh normal-byte indices with geometric triangle normals.
//
// Usage:
//   myth2_normal_analyze <folder> [index]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

static uint32_t swap32(uint32_t n) {
    return ((n & 0xFF000000u) >> 24) | ((n & 0x00FF0000u) >> 8)
         | ((n & 0x0000FF00u) << 8)  | ((n & 0x000000FFu) << 24);
}
static uint16_t swap16(uint16_t n) { return (uint16_t)((n >> 8) | (n << 8)); }

static int16_t readBE16s(const uint8_t* b, size_t o) {
    uint16_t v;
    memcpy(&v, b + o, 2);
    return (int16_t)swap16(v);
}
static uint16_t readBE16u(const uint8_t* b, size_t o) {
    uint16_t v;
    memcpy(&v, b + o, 2);
    return swap16(v);
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
    int dataOffset = 0;
    int meshOffset = 0;
    int meshSize = 0;
};

static bool readManifest(const std::string& path, Manifest& m) {
    auto raw = readFile(path);
    if (raw.empty()) return false;
    std::string j(raw.begin(), raw.end());
    m.meshTag = jsonString(j, "mesh_tag", "");
    m.submeshWidth = jsonInt(j, "width", 0);
    m.submeshHeight = jsonInt(j, "height", 0);
    m.dataOffset = jsonInt(j, "data_offset", 0);
    m.meshOffset = jsonInt(j, "mesh_offset", 0);
    m.meshSize = jsonInt(j, "mesh_size", 0);
    return m.meshTag.size() == 4 && m.submeshWidth > 0 && m.submeshHeight > 0;
}

struct Myth2Cell {
    int16_t physicalHeight = 0;
    uint16_t normal = 0;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
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

static double degrees(double rad) {
    return rad * (180.0 / 3.14159265358979323846);
}

struct Sample {
    int x = 0;
    int y = 0;
    int slot = 0;
    Vec3 n;
};

struct IndexStats {
    int count = 0;
    double sumX = 0.0;
    double sumY = 0.0;
    double sumZ = 0.0;
    std::vector<Sample> samples;
};

static void addSample(IndexStats& st, int x, int y, int slot, const Vec3& n) {
    st.count++;
    st.sumX += n.x;
    st.sumY += n.y;
    st.sumZ += n.z;
    st.samples.push_back(Sample{x, y, slot, n});
}

static int cellIndexClamped(int x, int y, int cellWidth, int cellHeight) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= cellWidth) x = cellWidth - 1;
    if (y >= cellHeight) y = cellHeight - 1;
    return y * cellWidth + x;
}

static constexpr double WORLD_HEIGHT_SCALE = 1.0 / 512.0;

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II Normal Analyzer\n\n"
        "Usage:\n"
        "  %s <folder> [index]\n\n"
        "Arguments:\n"
        "  folder   Extracted Myth II map folder\n"
        "  index    Optional normal-byte index (0..255) for detailed output\n",
        p);
}

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return 1;
    }

    std::string folder = argv[1];
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();

    int wantedIndex = -1;
    if (argc == 3) {
        wantedIndex = atoi(argv[2]);
        if (wantedIndex < 0 || wantedIndex > 255) {
            fprintf(stderr, "Index must be in the range 0..255\n");
            return 1;
        }
    }

    Manifest mf;
    if (!readManifest(folder + "/manifest.json", mf)) {
        fprintf(stderr, "Cannot read manifest: %s\n", (folder + "/manifest.json").c_str());
        return 1;
    }

    auto raw = readFile(folder + "/raw/mesh_tag.bin");
    if (raw.empty()) {
        fprintf(stderr, "Cannot read: %s\n", (folder + "/raw/mesh_tag.bin").c_str());
        return 1;
    }

    const int cellW = mf.submeshWidth * 32;
    const int cellH = mf.submeshHeight * 32;
    const int vertexW = cellW + 1;
    const int vertexH = cellH + 1;
    const int expectedCells = cellW * cellH;
    const int cellOffset = mf.dataOffset + mf.meshOffset;
    const int cellSize = 12;
    const int expectedBytes = expectedCells * cellSize;

    if (cellOffset < 0 || (size_t)cellOffset + (size_t)expectedBytes > raw.size()) {
        fprintf(stderr, "Mesh data overruns file\n");
        return 1;
    }

    std::vector<Myth2Cell> cells((size_t)expectedCells);
    for (int i = 0; i < expectedCells; i++) {
        size_t off = (size_t)cellOffset + (size_t)i * cellSize;
        cells[(size_t)i].physicalHeight = readBE16s(raw.data(), off + 0);
        cells[(size_t)i].normal = readBE16u(raw.data(), off + 2);
    }

    std::vector<double> vx((size_t)vertexW * (size_t)vertexH);
    std::vector<double> vy((size_t)vertexW * (size_t)vertexH);
    std::vector<double> vz((size_t)vertexW * (size_t)vertexH);
    double halfW = (double)(vertexW - 1) * 0.5;
    double halfH = (double)(vertexH - 1) * 0.5;
    for (int y = 0; y < vertexH; y++) {
        for (int x = 0; x < vertexW; x++) {
            int vi = y * vertexW + x;
            const Myth2Cell& c = cells[(size_t)cellIndexClamped(x, y, cellW, cellH)];
            vx[(size_t)vi] = (double)y - halfH;
            vy[(size_t)vi] = (double)c.physicalHeight * WORLD_HEIGHT_SCALE;
            vz[(size_t)vi] = (double)x - halfW;
        }
    }

    std::vector<IndexStats> combined(256);
    std::vector<IndexStats> highSlot(256);
    std::vector<IndexStats> lowSlot(256);

    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            int A = y * vertexW + x;
            int B = y * vertexW + (x + 1);
            int C = (y + 1) * vertexW + (x + 1);
            int D = (y + 1) * vertexW + x;

            Vec3 pA{vx[(size_t)A], vy[(size_t)A], vz[(size_t)A]};
            Vec3 pB{vx[(size_t)B], vy[(size_t)B], vz[(size_t)B]};
            Vec3 pC{vx[(size_t)C], vy[(size_t)C], vz[(size_t)C]};
            Vec3 pD{vx[(size_t)D], vy[(size_t)D], vz[(size_t)D]};

            Vec3 n0;
            Vec3 n1;
            if (((x ^ y) & 1) == 0) {
                n0 = normalized(cross(sub(pB, pA), sub(pC, pA)));
                n1 = normalized(cross(sub(pC, pA), sub(pD, pA)));
            } else {
                n0 = normalized(cross(sub(pB, pA), sub(pD, pA)));
                n1 = normalized(cross(sub(pC, pB), sub(pD, pB)));
            }

            uint16_t word = cells[(size_t)(y * cellW + x)].normal;
            uint8_t hi = (uint8_t)((word >> 8) & 0xFF);
            uint8_t lo = (uint8_t)(word & 0xFF);

            addSample(combined[(size_t)hi], x, y, 0, n0);
            addSample(combined[(size_t)lo], x, y, 1, n1);
            addSample(highSlot[(size_t)hi], x, y, 0, n0);
            addSample(lowSlot[(size_t)lo], x, y, 1, n1);
        }
    }

    auto printStats = [](const char* title, const IndexStats& st) {
        Vec3 avg = normalized(Vec3{st.sumX, st.sumY, st.sumZ});
        double avgPitch = degrees(std::acos(clamp1(avg.y)));
        double avgYaw = degrees(std::atan2(avg.z, avg.x));
        double sumAng = 0.0;
        double maxAng = 0.0;
        for (const Sample& s : st.samples) {
            double ang = degrees(std::acos(clamp1(dot(avg, s.n))));
            sumAng += ang;
            if (ang > maxAng) maxAng = ang;
        }
        double meanAng = st.count ? (sumAng / st.count) : 0.0;
        printf("%s\n", title);
        printf("  count                  %d\n", st.count);
        printf("  avg_normal             (%.6f, %.6f, %.6f)\n", avg.x, avg.y, avg.z);
        printf("  avg_yaw_deg            %.3f\n", avgYaw);
        printf("  avg_pitch_from_up_deg  %.3f\n", avgPitch);
        printf("  mean_ang_error_deg     %.3f\n", meanAng);
        printf("  max_ang_error_deg      %.3f\n", maxAng);
    };

    printf("Myth II Normal Analyzer\n");
    printf("=======================\n");
    printf("Folder:        %s\n", folder.c_str());
    printf("Mesh tag:      %s\n", mf.meshTag.c_str());
    printf("Cell grid:     %d x %d = %d\n", cellW, cellH, expectedCells);
    printf("Samples:       %d triangles\n\n", expectedCells * 2);

    if (wantedIndex >= 0) {
        char title[64];
        sprintf(title, "Combined Index %d", wantedIndex);
        printStats(title, combined[(size_t)wantedIndex]);
        printf("\n");
        sprintf(title, "High-byte Slot Index %d", wantedIndex);
        printStats(title, highSlot[(size_t)wantedIndex]);
        printf("\n");
        sprintf(title, "Low-byte Slot Index %d", wantedIndex);
        printStats(title, lowSlot[(size_t)wantedIndex]);
        printf("\nSample triangles\n");
        printf("----------------\n");
        int shown = 0;
        for (const Sample& s : combined[(size_t)wantedIndex].samples) {
            printf("  (%3d,%3d) slot=%d normal=(%.6f, %.6f, %.6f)\n",
                   s.x, s.y, s.slot, s.n.x, s.n.y, s.n.z);
            if (++shown >= 12) break;
        }
        return 0;
    }

    struct Row {
        int index = 0;
        int count = 0;
        Vec3 avg;
        double meanAng = 0.0;
    };
    std::vector<Row> rows;
    rows.reserve(256);
    for (int i = 0; i < 256; i++) {
        const IndexStats& st = combined[(size_t)i];
        if (!st.count) continue;
        Vec3 avg = normalized(Vec3{st.sumX, st.sumY, st.sumZ});
        double sumAng = 0.0;
        for (const Sample& s : st.samples) {
            sumAng += degrees(std::acos(clamp1(dot(avg, s.n))));
        }
        rows.push_back(Row{i, st.count, avg, st.count ? (sumAng / st.count) : 0.0});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.index < b.index;
    });

    printf("%-5s %-8s %-10s %-10s %-10s %-10s %-10s\n",
           "idx", "count", "avg_nx", "avg_ny", "avg_nz", "yaw_deg", "mean_err");
    printf("%s\n", std::string(72, '-').c_str());
    for (const Row& r : rows) {
        double yaw = degrees(std::atan2(r.avg.z, r.avg.x));
        printf("%-5d %-8d %-10.6f %-10.6f %-10.6f %-10.3f %-10.3f\n",
               r.index, r.count, r.avg.x, r.avg.y, r.avg.z, yaw, r.meanAng);
    }

    printf("\nUnique indices used: %zu\n", rows.size());
    return 0;
}
