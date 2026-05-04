// myth2_mesh_dump.cpp
// Dump Myth II mesh cell fields for inspection, especially water/media cells.
//
// Usage:
//   myth2_mesh_dump <folder> [mesh_tag.bin|plugin] [all|wet]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

#include "myth2_mesh_flags.h"

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

static bool extractPluginMesh(const std::string& path, uint32_t wantedSubgroup, std::vector<uint8_t>& out) {
    auto data = readFile(path);
    if (data.size() < 128) return false;
    if (readBE32u(data.data(), 124) != 0x646E6732u) return false;
    uint16_t entryPointCount = readBE16u(data.data(), 100);
    uint16_t tagCount = readBE16u(data.data(), 102);
    size_t pos = 128u + (size_t)entryPointCount * 112u;
    for (uint16_t i = 0; i < tagCount; i++) {
        if (pos + 64 > data.size()) return false;
        uint32_t groupTag = readBE32u(data.data(), pos + 36);
        uint32_t subgroupTag = readBE32u(data.data(), pos + 40);
        uint32_t offset = readBE32u(data.data(), pos + 44);
        uint32_t size = readBE32u(data.data(), pos + 48);
        if (groupTag == 0x6D657368u && subgroupTag == wantedSubgroup) {
            if ((size_t)offset + size > data.size()) return false;
            out.assign(data.begin() + offset, data.begin() + offset + size);
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

static int triType(uint16_t flags, int which) {
    return which ? (flags & 0x0F) : ((flags >> 4) & 0x0F);
}

static bool triWet(uint16_t flags, int which) {
    return (flags & (which ? (1u << 12) : (1u << 11))) != 0;
}

static bool vertexMedia(uint16_t flags) {
    return (flags & MYTH2_MESH_VERTEX_IS_MEDIA_FLAG) != 0;
}

static bool animatedMedia(uint16_t flags) {
    return (flags & MYTH2_MESH_VERTEX_IS_ANIMATED_MEDIA_FLAG) != 0;
}

static bool reflection(uint16_t flags) {
    return (flags & MYTH2_MESH_CELL_HAS_REFLECTION_FLAG) != 0;
}

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II Mesh Dump\n\n"
        "Usage:\n"
        "  %s <folder> [mesh_tag.bin|plugin] [all|wet]\n",
        p);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    std::string folder = argv[1];
    std::string target;
    std::string mode = "wet";
    if (argc >= 3) target = argv[2];
    if (argc >= 4) mode = argv[3];
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();

    Manifest mf;
    if (!readManifest(folder + "/manifest.json", mf)) {
        fprintf(stderr, "Cannot read manifest from %s\n", folder.c_str());
        return 1;
    }

    std::vector<uint8_t> mesh;
    if (target.empty()) {
        mesh = readFile(folder + "/raw/mesh_tag.bin");
        target = folder + "/raw/mesh_tag.bin";
    } else if (fileExists(target) && target.size() >= 4 &&
               target.substr(target.size() - 4) == ".bin") {
        mesh = readFile(target);
    } else {
        if (!extractPluginMesh(target, tagFromString(mf.meshTag), mesh)) {
            fprintf(stderr, "Could not extract mesh %.4s from %s\n", mf.meshTag.c_str(), target.c_str());
            return 1;
        }
    }
    if (mesh.empty()) {
        fprintf(stderr, "No mesh data loaded from %s\n", target.c_str());
        return 1;
    }

    int cellW = mf.submeshWidth * 32;
    int cellH = mf.submeshHeight * 32;
    size_t expectedBytes = 1024u + (size_t)cellW * cellH * 12u;
    if (mesh.size() < expectedBytes) {
        fprintf(stderr, "Mesh data too small for declared dimensions\n");
        return 1;
    }

    bool wetOnly = (mode != "all");

    printf("Myth II Mesh Dump\n");
    printf("=================\n");
    printf("Folder:    %s\n", folder.c_str());
    printf("Mesh:      %s\n", target.c_str());
    printf("Mesh tag:  %s\n", mf.meshTag.c_str());
    printf("Mode:      %s\n", wetOnly ? "wet" : "all");
    printf("Cell grid: %d x %d = %d\n\n", cellW, cellH, cellW * cellH);

    printf("x,y,phys,normal,flags,first_obj,media,render,t0,t1,wet0,wet1,vertex_media,animated,reflection\n");
    int printed = 0;
    for (int y = 0; y < cellH; y++) {
        for (int x = 0; x < cellW; x++) {
            MeshCell c = readCell(mesh, cellW, x, y);
            bool wet0 = triWet(c.flags, 0);
            bool wet1 = triWet(c.flags, 1);
            if (wetOnly && !wet0 && !wet1) continue;
            printf("%d,%d,%d,%u,%04X,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                   x, y,
                   (int)c.physicalHeight,
                   (unsigned)c.normal,
                   (unsigned)c.flags,
                   (int)c.firstObjectIndex,
                   (int)c.mediaHeight,
                   (int)c.renderHeight,
                   triType(c.flags, 0),
                   triType(c.flags, 1),
                   wet0 ? 1 : 0,
                   wet1 ? 1 : 0,
                   vertexMedia(c.flags) ? 1 : 0,
                   animatedMedia(c.flags) ? 1 : 0,
                   reflection(c.flags) ? 1 : 0);
            printed++;
        }
    }

    printf("\nRows written: %d\n", printed);
    return 0;
}
