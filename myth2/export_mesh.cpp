// export_mesh.cpp
// Export a Myth II extracted mesh folder to Wavefront OBJ.
//
// Usage:
//   export_mesh <tag_folder> [output.obj] [heightscale]
//
// Example:
//   export_mesh 85gy
//   export_mesh 85gy displacement.obj 0.001953125

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

static uint32_t swap32(uint32_t n) {
    return ((n & 0xFF000000u) >> 24) | ((n & 0x00FF0000u) >> 8)
         | ((n & 0x0000FF00u) << 8)  | ((n & 0x000000FFu) << 24);
}
static uint16_t swap16(uint16_t n) { return (uint16_t)((n >> 8) | (n << 8)); }
static int32_t readBE32s(const uint8_t* b, size_t o) {
    uint32_t v;
    memcpy(&v, b + o, 4);
    return (int32_t)swap32(v);
}
static int16_t readBE16s(const uint8_t* b, size_t o) {
    uint16_t v;
    memcpy(&v, b + o, 2);
    return (int16_t)swap16(v);
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

static std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

static std::string stemOf(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    std::string n = s == std::string::npos ? path : path.substr(s + 1);
    size_t d = n.rfind('.');
    return d == std::string::npos ? n : n.substr(0, d);
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

struct Myth2Cell {
    int16_t physicalHeight = 0;
    uint16_t normal = 0;
    uint16_t flags = 0;
    int16_t firstObjectIndex = 0;
    int16_t mediaHeight = 0;
    int16_t renderHeight = 0;
};

struct MeshInfo {
    std::string meshTag;
    int submeshWidth = 0;
    int submeshHeight = 0;
    int cellWidth = 0;
    int cellHeight = 0;
    int vertexWidth = 0;
    int vertexHeight = 0;
    std::vector<Myth2Cell> cells;
};

static bool readMesh(const std::string& folder, MeshInfo& out) {
    Manifest manifest;
    if (!readManifest(folder + "/manifest.json", manifest)) return false;

    auto raw = readFile(folder + "/raw/mesh_tag.bin");
    if (raw.empty()) {
        fprintf(stderr, "Cannot read: %s\n", (folder + "/raw/mesh_tag.bin").c_str());
        return false;
    }

    const int cellSize = 12;
    const int expectedCells = manifest.submeshWidth * 32 * manifest.submeshHeight * 32;
    const int cellOffset = manifest.dataOffset + manifest.meshOffset;
    const int expectedBytes = expectedCells * cellSize;

    if (cellOffset < 0 || expectedCells <= 0 || (size_t)cellOffset > raw.size()) {
        fprintf(stderr, "Invalid mesh offsets in manifest\n");
        return false;
    }
    if ((size_t)cellOffset + (size_t)expectedBytes > raw.size()) {
        fprintf(stderr, "Mesh data overruns file: need %d bytes at %d, file has %zu bytes\n",
                expectedBytes, cellOffset, raw.size());
        return false;
    }
    if (manifest.meshSize > 0 && manifest.meshSize != expectedBytes) {
        fprintf(stderr, "Warning: manifest mesh_size is %d but computed cell bytes are %d\n",
                manifest.meshSize, expectedBytes);
    }

    out.meshTag = manifest.meshTag;
    out.submeshWidth = manifest.submeshWidth;
    out.submeshHeight = manifest.submeshHeight;
    out.cellWidth = manifest.submeshWidth * 32;
    out.cellHeight = manifest.submeshHeight * 32;
    out.vertexWidth = out.cellWidth + 1;
    out.vertexHeight = out.cellHeight + 1;
    out.cells.resize((size_t)expectedCells);

    for (int i = 0; i < expectedCells; i++) {
        size_t off = (size_t)cellOffset + (size_t)i * cellSize;
        Myth2Cell& c = out.cells[(size_t)i];
        c.physicalHeight = readBE16s(raw.data(), off + 0);
        c.normal = (uint16_t)readBE16s(raw.data(), off + 2);
        c.flags = (uint16_t)readBE16s(raw.data(), off + 4);
        c.firstObjectIndex = readBE16s(raw.data(), off + 6);
        c.mediaHeight = readBE16s(raw.data(), off + 8);
        c.renderHeight = readBE16s(raw.data(), off + 10);
    }

    int16_t lo = out.cells[0].physicalHeight;
    int16_t hi = out.cells[0].physicalHeight;
    for (const Myth2Cell& c : out.cells) {
        lo = std::min(lo, c.physicalHeight);
        hi = std::max(hi, c.physicalHeight);
    }

    printf("Mesh tag:      %s\n", out.meshTag.c_str());
    printf("Submeshes:     %d x %d\n", out.submeshWidth, out.submeshHeight);
    printf("Cell grid:     %d x %d\n", out.cellWidth, out.cellHeight);
    printf("Vertex grid:   %d x %d\n", out.vertexWidth, out.vertexHeight);
    printf("Mesh offset:   %d bytes from file start\n", cellOffset);
    printf("Mesh bytes:    %d\n", expectedBytes);
    printf("Height range:  %d .. %d\n\n", lo, hi);
    return true;
}

static int vertexIndex(int x, int y, int vertexWidth) {
    return y * vertexWidth + x;
}

static int cellIndexClamped(int x, int y, int cellWidth, int cellHeight) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= cellWidth) x = cellWidth - 1;
    if (y >= cellHeight) y = cellHeight - 1;
    return y * cellWidth + x;
}

static void addFaceNormal(int i0, int i1, int i2,
                          const std::vector<float>& vx,
                          const std::vector<float>& vy,
                          const std::vector<float>& vz,
                          std::vector<float>& nx,
                          std::vector<float>& ny,
                          std::vector<float>& nz) {
    float ax = vx[i1] - vx[i0];
    float ay = vy[i1] - vy[i0];
    float az = vz[i1] - vz[i0];
    float bx = vx[i2] - vx[i0];
    float by = vy[i2] - vy[i0];
    float bz = vz[i2] - vz[i0];
    float cx = ay * bz - az * by;
    float cy = az * bx - ax * bz;
    float cz = ax * by - ay * bx;
    nx[i0] += cx; ny[i0] += cy; nz[i0] += cz;
    nx[i1] += cx; ny[i1] += cy; nz[i1] += cz;
    nx[i2] += cx; ny[i2] += cy; nz[i2] += cz;
}

static bool writeOBJ(const std::string& objPath, const MeshInfo& mesh, float hs,
                     const std::string& folder) {
    std::string mtlPath = dirOf(objPath) + stemOf(objPath) + ".mtl";

    // Copy terrain texture into models/textures/ alongside the OBJ
    std::string texName = "terrain.png";
    std::string texDest = dirOf(objPath) + "textures/" + texName;
    std::error_code ec;
    fs::create_directories(dirOf(objPath) + "textures", ec);
    std::string texSrc = folder + "/layers/01_terrain.png";
    if (fs::exists(texSrc))
        fs::copy_file(texSrc, texDest, fs::copy_options::overwrite_existing, ec);
    std::string texturePath;
    if (fs::exists(texDest))
        texturePath = "textures/" + texName;
    else if (fs::exists(texSrc))
        texturePath = texSrc;

    FILE* obj = fopen(objPath.c_str(), "w");
    if (!obj) {
        fprintf(stderr, "Cannot create: %s\n", objPath.c_str());
        return false;
    }
    FILE* mtl = fopen(mtlPath.c_str(), "w");
    if (!mtl) {
        fclose(obj);
        fprintf(stderr, "Cannot create: %s\n", mtlPath.c_str());
        return false;
    }

    int vw = mesh.vertexWidth;
    int vh = mesh.vertexHeight;
    int totalVerts = vw * vh;
    int totalFaces = mesh.cellWidth * mesh.cellHeight * 2;
    float halfW = (float)(vw - 1) * 0.5f;
    float halfH = (float)(vh - 1) * 0.5f;

    std::vector<float> vx((size_t)totalVerts), vy((size_t)totalVerts), vz((size_t)totalVerts);
    for (int y = 0; y < vh; y++) {
        for (int x = 0; x < vw; x++) {
            int vi = vertexIndex(x, y, vw);
            const Myth2Cell& c = mesh.cells[(size_t)cellIndexClamped(x, y, mesh.cellWidth, mesh.cellHeight)];
            vx[vi] = halfW - (float)x;
            vy[vi] = (float)c.physicalHeight * hs;
            vz[vi] = (float)y - halfH;
        }
    }

    std::vector<float> nx((size_t)totalVerts, 0.0f);
    std::vector<float> ny((size_t)totalVerts, 0.0f);
    std::vector<float> nz((size_t)totalVerts, 0.0f);
    for (int y = 0; y < mesh.cellHeight; y++) {
        for (int x = 0; x < mesh.cellWidth; x++) {
            int A = vertexIndex(x,     y,     vw);
            int B = vertexIndex(x + 1, y,     vw);
            int C = vertexIndex(x + 1, y + 1, vw);
            int D = vertexIndex(x,     y + 1, vw);
            if (((x ^ y) & 1) == 0) {
                addFaceNormal(A, B, C, vx, vy, vz, nx, ny, nz);
                addFaceNormal(A, C, D, vx, vy, vz, nx, ny, nz);
            } else {
                addFaceNormal(A, B, D, vx, vy, vz, nx, ny, nz);
                addFaceNormal(B, C, D, vx, vy, vz, nx, ny, nz);
            }
        }
    }

    auto basename = [](const std::string& p) {
        size_t s = p.find_last_of("/\\");
        return s == std::string::npos ? p : p.substr(s + 1);
    };

    fprintf(mtl, "# Myth II terrain material\n");
    fprintf(mtl, "newmtl terrain\n");
    fprintf(mtl, "Ka 1.0 1.0 1.0\n");
    fprintf(mtl, "Kd 1.0 1.0 1.0\n");
    fprintf(mtl, "Ks 0.0 0.0 0.0\n");
    fprintf(mtl, "illum 1\n");
    if (!texturePath.empty())
        fprintf(mtl, "map_Kd %s\n", texturePath.c_str());
    fclose(mtl);

    fprintf(obj, "# Myth II terrain -- %s\n", mesh.meshTag.c_str());
    fprintf(obj, "# Submeshes: %dx%d  Cells: %dx%d  Verts: %d  Tris: %d\n",
            mesh.submeshWidth, mesh.submeshHeight,
            mesh.cellWidth, mesh.cellHeight, totalVerts, totalFaces);
    fprintf(obj, "# Height scale: %.9f\n", hs);
    fprintf(obj, "mtllib %s\n\n", basename(mtlPath).c_str());

    fprintf(obj, "# Vertices\n");
    for (int i = 0; i < totalVerts; i++) {
        fprintf(obj, "v %.4f %.6f %.4f\n", vx[i], vy[i], vz[i]);
    }
    fprintf(obj, "\n");

    fprintf(obj, "# UVs\n");
    float uS = vw > 1 ? 1.0f / (float)(vw - 1) : 1.0f;
    float vS = vh > 1 ? 1.0f / (float)(vh - 1) : 1.0f;
    for (int y = 0; y < vh; y++) {
        for (int x = 0; x < vw; x++) {
            fprintf(obj, "vt %.6f %.6f\n", 1.0f - (float)x * uS, 1.0f - (float)y * vS);
        }
    }
    fprintf(obj, "\n");

    fprintf(obj, "# Normals\n");
    for (int i = 0; i < totalVerts; i++) {
        float len = sqrtf(nx[i] * nx[i] + ny[i] * ny[i] + nz[i] * nz[i]);
        if (len < 1e-6f) len = 1.0f;
        fprintf(obj, "vn %.6f %.6f %.6f\n", nx[i] / len, ny[i] / len, nz[i] / len);
    }
    fprintf(obj, "\n");

    fprintf(obj, "usemtl terrain\n");
    fprintf(obj, "s 1\n\n");
    for (int y = 0; y < mesh.cellHeight; y++) {
        for (int x = 0; x < mesh.cellWidth; x++) {
            int A = vertexIndex(x,     y,     vw) + 1;
            int B = vertexIndex(x + 1, y,     vw) + 1;
            int C = vertexIndex(x + 1, y + 1, vw) + 1;
            int D = vertexIndex(x,     y + 1, vw) + 1;
            if (((x ^ y) & 1) == 0) {
                fprintf(obj, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", A, A, A, B, B, B, C, C, C);
                fprintf(obj, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", A, A, A, C, C, C, D, D, D);
            } else {
                fprintf(obj, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", A, A, A, B, B, B, D, D, D);
                fprintf(obj, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", B, B, B, C, C, C, D, D, D);
            }
        }
    }

    fclose(obj);

    printf("Height scale:  %.9f\n", hs);
    printf("OBJ written:   %s\n", objPath.c_str());
    printf("MTL written:   %s\n", mtlPath.c_str());
    printf("Texture path:  %s\n", texturePath.empty() ? "(none)" : texturePath.c_str());
    printf("Vertices:      %d\n", totalVerts);
    printf("Triangles:     %d\n", totalFaces);
    return true;
}

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II Terrain Mesh Exporter\n\n"
        "Usage:\n"
        "  %s <tag_folder> [output.obj] [heightscale]\n\n"
        "Arguments:\n"
        "  tag_folder    Extracted Myth II map folder from extract\n"
        "  output path   Output path (default: <tag_folder>/displacement.obj)\n"
        "  heightscale   Vertical scale multiplier (default: 1/512)\n\n"
        "Example:\n"
        "  %s 85gy\n"
        "  %s 85gy displacement.obj 0.001953125\n",
        p, p, p);
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        usage(argv[0]);
        return 1;
    }

    std::string folder = argv[1];
    float heightScale = argc >= 4 ? (float)atof(argv[3]) : (1.0f / 512.0f);

    std::string outPath = argc >= 3 ? argv[2] : (folder + "/models/displacement.obj");
    std::error_code ec;
    fs::create_directories(fs::path(outPath).parent_path(), ec);
    printf("Myth II Terrain Mesh Exporter\n");
    printf("=============================\n");
    printf("Folder:        %s\n", folder.c_str());
    printf("Output path:   %s\n\n", outPath.c_str());

    MeshInfo mesh;
    if (!readMesh(folder, mesh)) return 1;
    if (!writeOBJ(outPath, mesh, heightScale, folder)) return 1;

    printf("\nDone.\n");
    return 0;
}
