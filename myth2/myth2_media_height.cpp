// myth2_media_height.cpp
// Set the Myth II media_height field across an extracted mesh.
//
// Usage:
//   myth2_media_height <folder> <value>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

static uint16_t swap16(uint16_t n) {
    return (uint16_t)((n >> 8) | (n << 8));
}

static int16_t readBE16s(const uint8_t* b, size_t o) {
    uint16_t v;
    memcpy(&v, b + o, 2);
    return (int16_t)swap16(v);
}

static void writeBE16s(uint8_t* b, size_t o, int16_t value) {
    uint16_t v = swap16((uint16_t)value);
    memcpy(b + o, &v, 2);
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

static bool writeFile(const std::string& path, const std::vector<uint8_t>& buf) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = true;
    if (!buf.empty() && fwrite(buf.data(), 1, buf.size(), f) != buf.size()) ok = false;
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

struct Manifest {
    int submeshWidth = 0;
    int submeshHeight = 0;
};

static bool readManifest(const std::string& path, Manifest& m) {
    auto raw = readFile(path);
    if (raw.empty()) return false;
    std::string j(raw.begin(), raw.end());
    m.submeshWidth = jsonInt(j, "width", jsonInt(j, "submesh_dimensions", 0));
    m.submeshHeight = jsonInt(j, "height", 0);
    if (m.submeshWidth == 0) m.submeshWidth = jsonInt(j, "width", 0);
    if (m.submeshHeight == 0) m.submeshHeight = jsonInt(j, "height", 0);
    if (m.submeshWidth == 0 || m.submeshHeight == 0) {
        size_t p = j.find("\"submesh_dimensions\"");
        if (p != std::string::npos) {
            std::string tail = j.substr(p);
            m.submeshWidth = jsonInt(tail, "width", m.submeshWidth);
            m.submeshHeight = jsonInt(tail, "height", m.submeshHeight);
        }
    }
    return m.submeshWidth > 0 && m.submeshHeight > 0;
}

static bool parseInt16(const char* s, int16_t& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0') return false;
    if (v < -32768L || v > 32767L) return false;
    out = (int16_t)v;
    return true;
}

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II Media Height Setter\n\n"
        "Usage:\n"
        "  %s <folder> <value>\n\n"
        "Example:\n"
        "  %s .\\85gy -1146\n",
        p, p);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    std::string folder = argv[1];
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();

    int16_t targetHeight = 0;
    if (!parseInt16(argv[2], targetHeight)) {
        fprintf(stderr, "Invalid media height: %s\n", argv[2]);
        return 1;
    }

    Manifest mf;
    if (!readManifest(folder + "/manifest.json", mf)) {
        fprintf(stderr, "Cannot read manifest from %s\n", folder.c_str());
        return 1;
    }

    std::string meshPath = folder + "/raw/mesh_tag.bin";
    auto mesh = readFile(meshPath);
    if (mesh.empty()) {
        fprintf(stderr, "Cannot read %s\n", meshPath.c_str());
        return 1;
    }

    const int cellW = mf.submeshWidth * 32;
    const int cellH = mf.submeshHeight * 32;
    const int cellCount = cellW * cellH;
    const size_t expectedBytes = 1024u + (size_t)cellCount * 12u;
    if (mesh.size() < expectedBytes) {
        fprintf(stderr, "Mesh file too small for declared dimensions (%d x %d)\n", cellW, cellH);
        return 1;
    }

    int changed = 0;
    int same = 0;
    int16_t oldMin = 32767;
    int16_t oldMax = -32768;
    for (int i = 0; i < cellCount; i++) {
        size_t off = 1024u + (size_t)i * 12u + 8u;
        int16_t oldValue = readBE16s(mesh.data(), off);
        if (oldValue < oldMin) oldMin = oldValue;
        if (oldValue > oldMax) oldMax = oldValue;
        if (oldValue == targetHeight) same++;
        else changed++;
        writeBE16s(mesh.data(), off, targetHeight);
    }

    if (!writeFile(meshPath, mesh)) {
        fprintf(stderr, "Failed to write %s\n", meshPath.c_str());
        return 1;
    }

    printf("Myth II Media Height Setter\n");
    printf("===========================\n");
    printf("Folder:        %s\n", folder.c_str());
    printf("Mesh:          %s\n", meshPath.c_str());
    printf("Cell grid:     %d x %d = %d\n", cellW, cellH, cellCount);
    printf("Old range:     %d .. %d\n", (int)oldMin, (int)oldMax);
    printf("New value:     %d\n", (int)targetHeight);
    printf("Changed cells: %d\n", changed);
    printf("Unchanged:     %d\n", same);
    printf("\nDone.\n");
    printf("  raw/mesh_tag.bin updated in-place.\n");

    return 0;
}
