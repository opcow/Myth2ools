// mesh_diff.cpp
// Compare a reference Myth II mesh tag from an extracted folder against
// another mesh tag or a rebuilt Myth II plugin.
//
// Usage:
//   mesh_diff <folder> <mesh_tag.bin|plugin>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
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
static uint32_t readBE32u(const uint8_t* b, size_t o) {
    uint32_t v;
    memcpy(&v, b + o, 4);
    return swap32(v);
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

static bool fileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
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

struct Manifest {
    std::string meshTag;
    int submeshWidth = 0;
    int submeshHeight = 0;
};

static bool readManifest(const std::string& path, Manifest& m) {
    auto raw = readFile(path);
    if (raw.empty()) return false;
    std::string j(raw.begin(), raw.end());
    m.meshTag = jsonString(j, "mesh_tag", "");
    m.submeshWidth = jsonInt(j, "width", jsonInt(j, "submesh_dimensions", 0));
    m.submeshHeight = jsonInt(j, "height", 0);
    if (m.submeshWidth == 0) m.submeshWidth = jsonInt(j, "width", 0);
    if (m.submeshHeight == 0) m.submeshHeight = jsonInt(j, "height", 0);
    if (m.submeshWidth == 0 || m.submeshHeight == 0) {
        size_t p = j.find("\"submesh_dimensions\"");
        if (p != std::string::npos) {
            std::string tail = j.substr(p);
            m.submeshWidth = std::max(m.submeshWidth, jsonInt(tail, "width", 0));
            m.submeshHeight = std::max(m.submeshHeight, jsonInt(tail, "height", 0));
        }
    }
    return m.meshTag.size() == 4 && m.submeshWidth > 0 && m.submeshHeight > 0;
}

struct TagHeader {
    uint32_t groupTag = 0;
    uint32_t subgroupTag = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

static bool extractPluginMesh(const std::string& path, uint32_t wantedSubgroup, std::vector<uint8_t>& out) {
    auto data = readFile(path);
    if (data.size() < 128) return false;
    if (readBE32u(data.data(), 124) != 0x646E6732u) return false; // dng2
    uint16_t entryPointCount = readBE16u(data.data(), 100);
    uint16_t tagCount = readBE16u(data.data(), 102);
    size_t pos = 128u + (size_t)entryPointCount * 112u;
    for (uint16_t i = 0; i < tagCount; i++) {
        if (pos + 64 > data.size()) return false;
        TagHeader t;
        t.groupTag = readBE32u(data.data(), pos + 36);
        t.subgroupTag = readBE32u(data.data(), pos + 40);
        t.offset = readBE32u(data.data(), pos + 44);
        t.size = readBE32u(data.data(), pos + 48);
        if (t.groupTag == 0x6D657368u && t.subgroupTag == wantedSubgroup) {
            if ((size_t)t.offset + t.size > data.size()) return false;
            out.assign(data.begin() + t.offset, data.begin() + t.offset + t.size);
            return true;
        }
        pos += 64;
    }
    return false;
}

struct MeshCell {
    int16_t physicalHeight = 0;
    uint16_t normal = 0;
    uint16_t flags = 0;
    int16_t firstObjectIndex = 0;
    int16_t mediaHeight = 0;
    int16_t renderHeight = 0;
};

static MeshCell readCell(const std::vector<uint8_t>& mesh, int cellW, int x, int y) {
    size_t off = 1024u + (size_t)(y * cellW + x) * 12u;
    MeshCell c;
    c.physicalHeight = readBE16s(mesh.data(), off + 0);
    c.normal = readBE16u(mesh.data(), off + 2);
    c.flags = readBE16u(mesh.data(), off + 4);
    c.firstObjectIndex = readBE16s(mesh.data(), off + 6);
    c.mediaHeight = readBE16s(mesh.data(), off + 8);
    c.renderHeight = readBE16s(mesh.data(), off + 10);
    return c;
}

static bool isWet(const MeshCell& c) {
    return (c.flags & ((1u << 11) | (1u << 12))) != 0;
}

struct DiffCounts {
    int physical = 0;
    int normal = 0;
    int flags = 0;
    int firstObject = 0;
    int media = 0;
    int render = 0;
    int any = 0;
    int wetAny = 0;
};

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II Mesh Diff\n\n"
        "Usage:\n"
        "  %s <folder> <mesh_tag.bin|plugin>\n",
        p);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    std::string folder = argv[1];
    std::string target = argv[2];
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();

    Manifest mf;
    if (!readManifest(folder + "/manifest.json", mf)) {
        fprintf(stderr, "Cannot read manifest from %s\n", folder.c_str());
        return 1;
    }

    auto refMesh = readFile(folder + "/raw/mesh_tag.bin");
    if (refMesh.empty()) {
        fprintf(stderr, "Cannot read %s/raw/mesh_tag.bin\n", folder.c_str());
        return 1;
    }

    std::vector<uint8_t> testMesh;
    if (fileExists(target) && target.size() >= 4 &&
        target.substr(target.size() - 4) == ".bin") {
        testMesh = readFile(target);
    } else {
        if (!extractPluginMesh(target, tagFromString(mf.meshTag), testMesh)) {
            fprintf(stderr, "Could not extract mesh %.4s from %s\n", mf.meshTag.c_str(), target.c_str());
            return 1;
        }
    }
    if (testMesh.empty()) {
        fprintf(stderr, "No mesh data loaded from %s\n", target.c_str());
        return 1;
    }

    int cellW = mf.submeshWidth * 32;
    int cellH = mf.submeshHeight * 32;
    size_t expectedBytes = 1024u + (size_t)cellW * cellH * 12u;
    if (refMesh.size() < expectedBytes || testMesh.size() < expectedBytes) {
        fprintf(stderr, "Mesh data too small for declared dimensions\n");
        return 1;
    }

    DiffCounts counts = {};
    struct Sample { int x, y; MeshCell a, b; };
    std::vector<Sample> samples;

    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            MeshCell a = readCell(refMesh, cellW, x, y);
            MeshCell b = readCell(testMesh, cellW, x, y);
            bool any = false;
            if (a.physicalHeight != b.physicalHeight) { counts.physical++; any = true; }
            if (a.normal != b.normal) { counts.normal++; any = true; }
            if (a.flags != b.flags) { counts.flags++; any = true; }
            if (a.firstObjectIndex != b.firstObjectIndex) { counts.firstObject++; any = true; }
            if (a.mediaHeight != b.mediaHeight) { counts.media++; any = true; }
            if (a.renderHeight != b.renderHeight) { counts.render++; any = true; }
            if (any) {
                counts.any++;
                if (isWet(a) || isWet(b)) counts.wetAny++;
                if (samples.size() < 20) samples.push_back({x, y, a, b});
            }
        }
    }

    printf("Myth II Mesh Diff\n");
    printf("=================\n");
    printf("Folder:         %s\n", folder.c_str());
    printf("Reference mesh: %s\n", (folder + "/raw/mesh_tag.bin").c_str());
    printf("Compare mesh:   %s\n", target.c_str());
    printf("Mesh tag:       %s\n", mf.meshTag.c_str());
    printf("Cell grid:      %d x %d = %d\n\n", cellW, cellH, cellW * cellH);

    printf("Changed cells by field:\n");
    printf("  physical_height   %d\n", counts.physical);
    printf("  normal            %d\n", counts.normal);
    printf("  flags             %d\n", counts.flags);
    printf("  first_object      %d\n", counts.firstObject);
    printf("  media_height      %d\n", counts.media);
    printf("  render_height     %d\n", counts.render);
    printf("  any field         %d\n", counts.any);
    printf("  any field (wet)   %d\n\n", counts.wetAny);

    if (!samples.empty()) {
        printf("First changed cells:\n");
        for (const auto& s : samples) {
            printf("  (%3d,%3d) flags %04X -> %04X  media %6d -> %6d  render %6d -> %6d  phys %6d -> %6d\n",
                   s.x, s.y,
                   (unsigned)s.a.flags, (unsigned)s.b.flags,
                   (int)s.a.mediaHeight, (int)s.b.mediaHeight,
                   (int)s.a.renderHeight, (int)s.b.renderHeight,
                   (int)s.a.physicalHeight, (int)s.b.physicalHeight);
        }
    } else {
        printf("No cell differences found.\n");
    }

    return 0;
}
