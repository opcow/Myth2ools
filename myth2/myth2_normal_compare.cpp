// myth2_normal_compare.cpp
// Compare the generated Myth II runtime normal table against empirical mesh-normal buckets
// and score simple axis/sign permutations to find the best basis alignment.
//
// Usage:
//   myth2_normal_compare <folder>

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
    return m.meshTag.size() == 4 && m.submeshWidth > 0 && m.submeshHeight > 0;
}

struct Cell {
    int16_t h = 0;
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
static double angleDeg(const Vec3& a, const Vec3& b) {
    return degrees(std::acos(clamp1(dot(a, b))));
}

static int cellIndexClamped(int x, int y, int cellWidth, int cellHeight) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= cellWidth) x = cellWidth - 1;
    if (y >= cellHeight) y = cellHeight - 1;
    return y * cellWidth + x;
}

static constexpr int FULL = 1024;
static constexpr int Q14_ONE = 0x4000;
static constexpr int TABLE_SIZE = 256;
static constexpr double WORLD_HEIGHT_SCALE = 1.0 / 512.0;

struct RuntimeEntry {
    Vec3 n;
};

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

struct Bucket {
    int count = 0;
    double sx = 0.0;
    double sy = 0.0;
    double sz = 0.0;
    Vec3 avg;
};

struct Perm {
    int ax[3];
    int sg[3];
};

static Vec3 applyPerm(const Vec3& v, const Perm& p) {
    const double c[3] = { v.x, v.y, v.z };
    return normalized(Vec3{
        c[p.ax[0]] * (double)p.sg[0],
        c[p.ax[1]] * (double)p.sg[1],
        c[p.ax[2]] * (double)p.sg[2]
    });
}

static const char* axisName(int i) {
    return i == 0 ? "x" : (i == 1 ? "y" : "z");
}

static void usage(const char* p) {
    fprintf(stderr, "Usage:\n  %s <folder>\n", p);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }

    std::string folder = argv[1];
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();

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
    if (cellOffset < 0 || (size_t)cellOffset + (size_t)expectedCells * 12u > raw.size()) {
        fprintf(stderr, "Mesh data overruns file\n");
        return 1;
    }

    std::vector<Cell> cells((size_t)expectedCells);
    for (int i = 0; i < expectedCells; i++) {
        size_t off = (size_t)cellOffset + (size_t)i * 12u;
        cells[(size_t)i].h = readBE16s(raw.data(), off + 0);
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
            const Cell& c = cells[(size_t)cellIndexClamped(x, y, cellW, cellH)];
            vx[(size_t)vi] = (double)y - halfH;
            vy[(size_t)vi] = (double)c.h * WORLD_HEIGHT_SCALE;
            vz[(size_t)vi] = (double)x - halfW;
        }
    }

    std::vector<Bucket> buckets(TABLE_SIZE);
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

            buckets[(size_t)hi].count++;
            buckets[(size_t)hi].sx += n0.x;
            buckets[(size_t)hi].sy += n0.y;
            buckets[(size_t)hi].sz += n0.z;

            buckets[(size_t)lo].count++;
            buckets[(size_t)lo].sx += n1.x;
            buckets[(size_t)lo].sy += n1.y;
            buckets[(size_t)lo].sz += n1.z;
        }
    }

    for (Bucket& b : buckets) {
        if (b.count > 0) b.avg = normalized(Vec3{b.sx, b.sy, b.sz});
    }

    auto runtime = buildRuntimeTable();

    std::vector<Perm> perms;
    int axes[6][3] = {
        {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}
    };
    int signs[8][3] = {
        { 1, 1, 1}, { 1, 1,-1}, { 1,-1, 1}, { 1,-1,-1},
        {-1, 1, 1}, {-1, 1,-1}, {-1,-1, 1}, {-1,-1,-1}
    };
    for (int a = 0; a < 6; a++) {
        for (int s = 0; s < 8; s++) {
            perms.push_back(Perm{{axes[a][0], axes[a][1], axes[a][2]},
                                 {signs[s][0], signs[s][1], signs[s][2]}});
        }
    }

    struct Score {
        Perm p{};
        double weighted = 0.0;
        double maxErr = 0.0;
        int used = 0;
    };
    std::vector<Score> scores;
    for (const Perm& p : perms) {
        Score sc;
        sc.p = p;
        double sum = 0.0;
        double weight = 0.0;
        for (int i = 0; i < TABLE_SIZE; i++) {
            if (buckets[(size_t)i].count < 16) continue;
            Vec3 rt = applyPerm(runtime[(size_t)i].n, p);
            double err = angleDeg(rt, buckets[(size_t)i].avg);
            sum += err * (double)buckets[(size_t)i].count;
            weight += (double)buckets[(size_t)i].count;
            sc.used++;
            if (err > sc.maxErr) sc.maxErr = err;
        }
        sc.weighted = weight > 0.0 ? (sum / weight) : 1e9;
        scores.push_back(sc);
    }

    std::sort(scores.begin(), scores.end(), [](const Score& a, const Score& b) {
        if (a.weighted != b.weighted) return a.weighted < b.weighted;
        return a.maxErr < b.maxErr;
    });

    printf("Myth II Normal Compare\n");
    printf("======================\n");
    printf("Folder:        %s\n", folder.c_str());
    printf("Mesh tag:      %s\n", mf.meshTag.c_str());
    printf("Compared ids:  counts >= 16 only\n\n");

    printf("Top basis permutations\n");
    printf("----------------------\n");
    for (size_t i = 0; i < scores.size() && i < 12; i++) {
        const Score& sc = scores[i];
        printf("%2zu. map=(%s%s,%s%s,%s%s)  weighted_err=%.3f  max_err=%.3f  used=%d\n",
               i + 1,
               sc.p.sg[0] < 0 ? "-" : "+", axisName(sc.p.ax[0]),
               sc.p.sg[1] < 0 ? "-" : "+", axisName(sc.p.ax[1]),
               sc.p.sg[2] < 0 ? "-" : "+", axisName(sc.p.ax[2]),
               sc.weighted, sc.maxErr, sc.used);
    }

    const Score& best = scores[0];
    printf("\nBest-basis detailed comparison\n");
    printf("------------------------------\n");
    printf("basis=(%s%s,%s%s,%s%s)\n\n",
           best.p.sg[0] < 0 ? "-" : "+", axisName(best.p.ax[0]),
           best.p.sg[1] < 0 ? "-" : "+", axisName(best.p.ax[1]),
           best.p.sg[2] < 0 ? "-" : "+", axisName(best.p.ax[2]));

    struct Row {
        int idx = 0;
        int count = 0;
        double err = 0.0;
        Vec3 runtime{};
        Vec3 empirical{};
    };
    std::vector<Row> rows;
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (buckets[(size_t)i].count < 16) continue;
        Vec3 rt = applyPerm(runtime[(size_t)i].n, best.p);
        rows.push_back(Row{i, buckets[(size_t)i].count, angleDeg(rt, buckets[(size_t)i].avg), rt, buckets[(size_t)i].avg});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.err != b.err) return a.err < b.err;
        return a.idx < b.idx;
    });

    printf("%-5s %-8s %-9s %-28s %-28s\n", "idx", "count", "err_deg", "runtime", "empirical");
    printf("%s\n", std::string(92, '-').c_str());
    for (size_t i = 0; i < rows.size() && i < 40; i++) {
        const Row& r = rows[i];
        printf("%-5d %-8d %-9.3f (%.3f, %.3f, %.3f)   (%.3f, %.3f, %.3f)\n",
               r.idx, r.count, r.err,
               r.runtime.x, r.runtime.y, r.runtime.z,
               r.empirical.x, r.empirical.y, r.empirical.z);
    }

    struct MatchRow {
        int meshIdx = 0;
        int runtimeIdx = 0;
        int count = 0;
        double err = 0.0;
        Vec3 runtime{};
        Vec3 empirical{};
    };

    std::vector<MatchRow> matches;
    matches.reserve(rows.size());
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (buckets[(size_t)i].count < 16) continue;
        double bestErr = 1e9;
        int bestIdx = -1;
        Vec3 bestVec{};
        for (int j = 0; j < TABLE_SIZE; j++) {
            Vec3 rt = applyPerm(runtime[(size_t)j].n, best.p);
            double err = angleDeg(rt, buckets[(size_t)i].avg);
            if (err < bestErr) {
                bestErr = err;
                bestIdx = j;
                bestVec = rt;
            }
        }
        matches.push_back(MatchRow{i, bestIdx, buckets[(size_t)i].count, bestErr, bestVec, buckets[(size_t)i].avg});
    }

    std::sort(matches.begin(), matches.end(), [](const MatchRow& a, const MatchRow& b) {
        if (a.err != b.err) return a.err < b.err;
        if (a.count != b.count) return a.count > b.count;
        return a.meshIdx < b.meshIdx;
    });

    printf("\nNearest runtime-entry matches\n");
    printf("----------------------------\n");
    printf("%-8s %-10s %-8s %-9s %-28s %-28s\n",
           "mesh_idx", "runtime_idx", "count", "err_deg", "runtime", "empirical");
    printf("%s\n", std::string(100, '-').c_str());
    for (size_t i = 0; i < matches.size() && i < 60; i++) {
        const MatchRow& m = matches[i];
        printf("%-8d %-10d %-8d %-9.3f (%.3f, %.3f, %.3f)   (%.3f, %.3f, %.3f)\n",
               m.meshIdx, m.runtimeIdx, m.count, m.err,
               m.runtime.x, m.runtime.y, m.runtime.z,
               m.empirical.x, m.empirical.y, m.empirical.z);
    }

    struct RuntimeUse {
        int runtimeIdx = 0;
        int hits = 0;
    };
    std::vector<RuntimeUse> use(TABLE_SIZE);
    for (int i = 0; i < TABLE_SIZE; i++) use[(size_t)i].runtimeIdx = i;
    for (const MatchRow& m : matches) {
        if (m.runtimeIdx >= 0) use[(size_t)m.runtimeIdx].hits++;
    }
    std::sort(use.begin(), use.end(), [](const RuntimeUse& a, const RuntimeUse& b) {
        if (a.hits != b.hits) return a.hits > b.hits;
        return a.runtimeIdx < b.runtimeIdx;
    });

    printf("\nRuntime-entry reuse counts\n");
    printf("--------------------------\n");
    for (size_t i = 0; i < use.size() && i < 24; i++) {
        if (use[i].hits == 0) break;
        printf("runtime_idx %-4d used by %d mesh buckets\n", use[i].runtimeIdx, use[i].hits);
    }

    return 0;
}
