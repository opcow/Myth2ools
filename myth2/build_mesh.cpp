// build_mesh.cpp
// Create a minimal but structurally valid mesh_tag.bin that build_plugin
// can patch into via --edit mode. All data comes from JSON exports.
// build_plugin handles terrain, normals, passability, water, etc.

#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

static uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t swap32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}
static void w16(uint8_t* b, size_t o, uint16_t v) { uint16_t s = swap16(v); memcpy(b+o, &s, 2); }
static void w32(uint8_t* b, size_t o, uint32_t v) { uint32_t s = swap32(v); memcpy(b+o, &s, 4); }
static uint16_t r16(const uint8_t* b, size_t o) { uint16_t v; memcpy(&v, b+o, 2); return swap16(v); }
static int16_t r16s(const uint8_t* b, size_t o) { return (int16_t)r16(b, o); }
static uint32_t r32(const uint8_t* b, size_t o) { uint32_t v; memcpy(&v, b+o, 4); return swap32(v); }

static std::vector<uint8_t> readFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return {};
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    std::vector<uint8_t> buf((size_t)sz);
    if (sz > 0 && fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return {}; }
    fclose(f); return buf;
}
static bool writeFile(const std::string& path, const std::vector<uint8_t>& d) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    fclose(f);
    return ok;
}

// ---- JSON helpers ----
static std::string readText(const std::string& path) {
    auto d = readFile(path); return std::string(d.begin(), d.end());
}
static uint32_t tag4(const std::string& s);
static std::string jsonStr(const std::string& j, const std::string& key) {
    size_t p = j.find("\"" + key + "\"");
    if (p == std::string::npos) return "";
    p = j.find(':', p); if (p == std::string::npos) return "";
    p = j.find('"', p); if (p == std::string::npos) return "";
    p++; std::string r;
    while (p < j.size()) { char c = j[p++]; if (c == '"') return r; r.push_back(c); }
    return "";
}
static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}
static std::vector<uint8_t> decodeHex(const std::string& s) {
    std::vector<uint8_t> out;
    if ((s.size() & 1) != 0) return out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = hexNibble(s[i]);
        int lo = hexNibble(s[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return out;
}
static int jsonInt(const std::string& j, const std::string& key, int d = 0) {
    size_t p = j.find("\"" + key + "\":"); if (p == std::string::npos) return d;
    p = j.find(':', p); if (p == std::string::npos) return d; p++;
    while (p < j.size() && (j[p]==' '||j[p]=='\t')) p++;
    bool neg = (p < j.size() && j[p]=='-'); if (neg) p++;
    int v = 0; while (p < j.size() && j[p]>='0' && j[p]<='9') { v=v*10+(j[p]-'0'); p++; }
    return neg ? -v : v;
}
static double jsonDbl(const std::string& j, const std::string& key, double d = 0.0) {
    size_t p = j.find("\"" + key + "\":"); if (p == std::string::npos) return d;
    p = j.find(':', p); if (p == std::string::npos) return d; p++;
    while (p < j.size() && (j[p]==' '||j[p]=='\t')) p++;
    bool neg = (p < j.size() && j[p]=='-'); if (neg) p++;
    double v = 0; bool dot = false; double frac = 1.0;
    while (p < j.size()) {
        char c = j[p];
        if (c>='0'&&c<='9') { if(dot){frac/=10;v+=(double)(c-'0')*frac;}else v=v*10+(c-'0'); p++; }
        else if (c=='.'&&!dot){dot=true;p++;} else break;
    }
    return neg ? -v : v;
}
static bool jsonTagRef(const std::string& j, const std::string& key, uint32_t& out) {
    size_t p = j.find("\"" + key + "\"");
    if (p == std::string::npos) return false;
    p = j.find(':', p);
    if (p == std::string::npos) return false;
    p++;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) p++;
    if (p >= j.size()) return false;
    if (j.compare(p, 4, "null") == 0) {
        out = 0xFFFFFFFFu;
        return true;
    }
    if (j[p] == '0') {
        out = 0u;
        return true;
    }
    if (j[p] == '"') {
        p++;
        if (p + 4 <= j.size() && p + 4 < j.size() && j[p + 4] == '"') {
            out = tag4(j.substr(p, 4));
            return true;
        }
    }
    return false;
}
static bool jsonArrayTagRef(const std::string& j, const std::string& key, int index, uint32_t& out) {
    size_t p = j.find("\"" + key + "\"");
    if (p == std::string::npos) return false;
    p = j.find('[', p);
    if (p == std::string::npos) return false;
    p++;
    for (int i = 0; i <= index; i++) {
        while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) p++;
        if (p >= j.size()) return false;
        if (i == index) {
            if (j.compare(p, 4, "null") == 0) {
                out = 0xFFFFFFFFu;
                return true;
            }
            if (j[p] == '0') {
                out = 0u;
                return true;
            }
            if (j[p] == '"') {
                p++;
                if (p + 4 <= j.size() && p + 4 < j.size() && j[p + 4] == '"') {
                    out = tag4(j.substr(p, 4));
                    return true;
                }
            }
            return false;
        }
        while (p < j.size() && j[p] != ',' && j[p] != ']') p++;
        if (p >= j.size() || j[p] == ']') return false;
        p++;
    }
    return false;
}
static uint32_t tag4(const std::string& s) {
    if (s.size() != 4) return 0;
    return ((uint32_t)(uint8_t)s[0]<<24)|((uint32_t)(uint8_t)s[1]<<16)
         |((uint32_t)(uint8_t)s[2]<<8)|(uint32_t)(uint8_t)s[3];
}

// ---- Instance from JSON ----
struct JsonInst {
    std::string tag; double x, y, z; double facingDeg; int palIdx, markerIdx;
    int identifier = -1;
    int permutation = 0;
    int pitchRaw = -1;
    int rollRaw = -1;
};

// ---- Unit type definition ----
struct UnitTypeDef {
    uint16_t markerType = 0;
    uint32_t typeTag = 0;
    int paletteIndex = 0;
    int instanceCount = 0;
    uint16_t flags = 0;
    uint16_t teamIndex = 0xFFFFu;
    uint32_t netgameFlags = 0x0000FFFFu;
};

struct PreservedMarkerRecord {
    std::vector<uint8_t> raw;
};

struct SourceInstanceRecord {
    std::vector<uint8_t> raw;
};

struct SourceUnitTypeRecord {
    std::vector<uint8_t> raw;
};

struct SourceCellRecord {
    std::vector<uint8_t> raw;
};

static void loadFixedRecords(const std::vector<uint8_t>& bytes, size_t stride, std::vector<SourceInstanceRecord>& out) {
    out.clear();
    if (stride == 0 || bytes.empty() || (bytes.size() % stride) != 0) return;
    out.resize(bytes.size() / stride);
    for (size_t i = 0; i < out.size(); i++) {
        size_t off = i * stride;
        out[i].raw.assign(bytes.begin() + (ptrdiff_t)off,
                          bytes.begin() + (ptrdiff_t)off + (ptrdiff_t)stride);
    }
}

static void loadFixedRecords(const std::vector<uint8_t>& bytes, size_t stride, std::vector<SourceUnitTypeRecord>& out) {
    out.clear();
    if (stride == 0 || bytes.empty() || (bytes.size() % stride) != 0) return;
    out.resize(bytes.size() / stride);
    for (size_t i = 0; i < out.size(); i++) {
        size_t off = i * stride;
        out[i].raw.assign(bytes.begin() + (ptrdiff_t)off,
                          bytes.begin() + (ptrdiff_t)off + (ptrdiff_t)stride);
    }
}

static void loadUnitTypeRecordsFromJson(const std::string& json, std::vector<SourceUnitTypeRecord>& out) {
    out.clear();
    if (json.empty()) return;
    size_t arrStart = json.find("\"unit_types\"");
    if (arrStart != std::string::npos) arrStart = json.find('[', arrStart);
    if (arrStart == std::string::npos) return;
    int arrDepth = 1;
    size_t arrEnd = arrStart + 1;
    while (arrEnd < json.size() && arrDepth > 0) {
        if (json[arrEnd] == '[') arrDepth++;
        else if (json[arrEnd] == ']') arrDepth--;
        if (arrDepth > 0) arrEnd++;
    }
    size_t pos = arrStart;
    while (true) {
        size_t ob = json.find('{', pos);
        if (ob == std::string::npos || ob > arrEnd) break;
        int objDepth = 1;
        size_t cb = ob + 1;
        while (cb < json.size() && objDepth > 0) {
            if (json[cb] == '{') objDepth++;
            else if (json[cb] == '}') objDepth--;
            if (objDepth > 0) cb++;
        }
        if (objDepth != 0) break;
        std::string obj = json.substr(ob, cb - ob + 1);
        std::vector<uint8_t> raw = decodeHex(jsonStr(obj, "raw_hex"));
        if (raw.size() == 32) {
            SourceUnitTypeRecord rec;
            rec.raw = std::move(raw);
            out.push_back(std::move(rec));
        }
        pos = cb + 1;
    }
}

static void loadSourceInstancesFromJson(const std::string& json, std::vector<SourceInstanceRecord>& out) {
    out.clear();
    if (json.empty()) return;
    size_t arrStart = json.find("\"source_instances\"");
    if (arrStart != std::string::npos) arrStart = json.find('[', arrStart);
    if (arrStart == std::string::npos) return;
    int arrDepth = 1;
    size_t arrEnd = arrStart + 1;
    while (arrEnd < json.size() && arrDepth > 0) {
        if (json[arrEnd] == '[') arrDepth++;
        else if (json[arrEnd] == ']') arrDepth--;
        if (arrDepth > 0) arrEnd++;
    }
    size_t pos = arrStart;
    while (true) {
        size_t ob = json.find('{', pos);
        if (ob == std::string::npos || ob > arrEnd) break;
        int objDepth = 1;
        size_t cb = ob + 1;
        while (cb < json.size() && objDepth > 0) {
            if (json[cb] == '{') objDepth++;
            else if (json[cb] == '}') objDepth--;
            if (objDepth > 0) cb++;
        }
        if (objDepth != 0) break;
        std::string obj = json.substr(ob, cb - ob + 1);
        std::vector<uint8_t> raw = decodeHex(jsonStr(obj, "raw_hex"));
        if (raw.size() == 64) {
            SourceInstanceRecord rec;
            rec.raw = std::move(raw);
            out.push_back(std::move(rec));
        }
        pos = cb + 1;
    }
}

static void loadCellGridFromJson(const std::string& json, std::vector<SourceCellRecord>& out) {
    out.clear();
    if (json.empty()) return;
    size_t arrStart = json.find("\"cells\"");
    if (arrStart != std::string::npos) arrStart = json.find('[', arrStart);
    if (arrStart == std::string::npos) return;
    int arrDepth = 1;
    size_t arrEnd = arrStart + 1;
    while (arrEnd < json.size() && arrDepth > 0) {
        if (json[arrEnd] == '[') arrDepth++;
        else if (json[arrEnd] == ']') arrDepth--;
        if (arrDepth > 0) arrEnd++;
    }
    size_t pos = arrStart;
    while (true) {
        size_t ob = json.find('{', pos);
        if (ob == std::string::npos || ob > arrEnd) break;
        int objDepth = 1;
        size_t cb = ob + 1;
        while (cb < json.size() && objDepth > 0) {
            if (json[cb] == '{') objDepth++;
            else if (json[cb] == '}') objDepth--;
            if (objDepth > 0) cb++;
        }
        if (objDepth != 0) break;
        std::string obj = json.substr(ob, cb - ob + 1);
        std::vector<uint8_t> raw = decodeHex(jsonStr(obj, "raw_hex"));
        if (raw.size() == 12) {
            SourceCellRecord rec;
            rec.raw = std::move(raw);
            out.push_back(std::move(rec));
        }
        pos = cb + 1;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: build_mesh <folder> [--output <path>]\n");
        return 1;
    }
    std::string folder = argv[1];
    while (!folder.empty() && (folder.back()=='/'||folder.back()=='\\')) folder.pop_back();
    std::string outPath = folder + "/raw/mesh_tag.bin";
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--output" && i+1 < argc) outPath = argv[++i];
    }

    std::string manifest = readText(folder + "/manifest.json");
    if (manifest.empty()) { fprintf(stderr, "Cannot read manifest.json\n"); return 1; }

    // Parse manifest
    int subW = jsonInt(manifest, "width", 0), subH = jsonInt(manifest, "height", 0);
    if (subW == 0 || subH == 0) {
        size_t p = manifest.find("\"submesh_dimensions\"");
        if (p != std::string::npos) {
            std::string tail = manifest.substr(p);
            subW = jsonInt(tail, "width", 0); subH = jsonInt(tail, "height", 0);
        }
    }
    if (subW == 0 || subH == 0) { fprintf(stderr, "No submesh dimensions\n"); return 1; }

    std::string meshTag = jsonStr(manifest, "mesh_tag");
    if (meshTag.size() != 4) { fprintf(stderr, "No mesh_tag\n"); return 1; }
    std::string landTag = jsonStr(manifest, "landscape_256");
    if (landTag.empty()) landTag = "85gi";
    std::string lightingTag = jsonStr(manifest, "mesh_lighting_tag");
    std::string connectorTag = jsonStr(manifest, "connector_tag");
    std::string nextMeshTag = jsonStr(manifest, "next_mesh");
    std::string nextMeshAlternateTag = jsonStr(manifest, "next_mesh_alternate");
    int edgeNorth = jsonInt(manifest, "north", 32);
    int edgeEast = jsonInt(manifest, "east", 32);
    int edgeSouth = jsonInt(manifest, "south", 32);
    int edgeWest = jsonInt(manifest, "west", 32);
    uint32_t windTagRef = 0u;
    uint32_t screenTagRefs[3] = { 0u, 0u, 0u };
    uint32_t winAmbientRef = 0xFFFFFFFFu;
    uint32_t lossAmbientRef = 0xFFFFFFFFu;
    uint32_t hintsRef = 0xFFFFFFFFu;
    uint8_t fogColorRaw[8] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    uint32_t fogDensity = (uint32_t)jsonInt(manifest, "fog_density", 0);
    bool haveWindTag = jsonTagRef(manifest, "wind_tag", windTagRef);
    bool haveScreenTagRefs[3] = {
        jsonArrayTagRef(manifest, "screen_collection_tags", 0, screenTagRefs[0]),
        jsonArrayTagRef(manifest, "screen_collection_tags", 1, screenTagRefs[1]),
        jsonArrayTagRef(manifest, "screen_collection_tags", 2, screenTagRefs[2])
    };
    bool haveWinAmbientRef = jsonTagRef(manifest, "win_ambient_sound", winAmbientRef);
    bool haveLossAmbientRef = jsonTagRef(manifest, "loss_ambient_sound", lossAmbientRef);
    bool haveHintsRef = jsonTagRef(manifest, "hints_string_list_tag", hintsRef);
    {
        std::vector<uint8_t> fogHex = decodeHex(jsonStr(manifest, "fog_color_raw_hex"));
        if (fogHex.size() == 8) memcpy(fogColorRaw, fogHex.data(), 8);
    }

    std::vector<PreservedMarkerRecord> preservedMarkers;
    std::string teamMarkersJson = readText(folder + "/team_markers.json");
    if (!teamMarkersJson.empty()) {
        size_t arrStart = teamMarkersJson.find("\"team_markers\"");
        if (arrStart != std::string::npos) arrStart = teamMarkersJson.find('[', arrStart);
        if (arrStart != std::string::npos) {
            int arrDepth = 1;
            size_t arrEnd = arrStart + 1;
            while (arrEnd < teamMarkersJson.size() && arrDepth > 0) {
                if (teamMarkersJson[arrEnd] == '[') arrDepth++;
                else if (teamMarkersJson[arrEnd] == ']') arrDepth--;
                if (arrDepth > 0) arrEnd++;
            }
            size_t pos = arrStart;
            while (true) {
                size_t ob = teamMarkersJson.find('{', pos);
                if (ob == std::string::npos || ob > arrEnd) break;
                int objDepth = 1;
                size_t cb = ob + 1;
                while (cb < teamMarkersJson.size() && objDepth > 0) {
                    if (teamMarkersJson[cb] == '{') objDepth++;
                    else if (teamMarkersJson[cb] == '}') objDepth--;
                    if (objDepth > 0) cb++;
                }
                if (objDepth != 0) break;
                std::string obj = teamMarkersJson.substr(ob, cb - ob + 1);
                std::vector<uint8_t> raw = decodeHex(jsonStr(obj, "raw_hex"));
                if (raw.size() == 64) {
                    PreservedMarkerRecord rec;
                    rec.raw = std::move(raw);
                    preservedMarkers.push_back(std::move(rec));
                }
                pos = cb + 1;
            }
        }
    }
    std::string meshMetadataJson = readText(folder + "/mesh_metadata.json");
    uint32_t supportUnitTypeCount = (uint32_t)jsonInt(meshMetadataJson, "unit_type_count", 0);
    uint32_t supportUnitTypeSize = (uint32_t)jsonInt(meshMetadataJson, "unit_type_size", 0);
    uint32_t supportMeshSize = (uint32_t)jsonInt(meshMetadataJson, "mesh_size", 0);
    uint32_t supportMarkerCount = (uint32_t)jsonInt(meshMetadataJson, "marker_count", 0);
    uint32_t supportMarkerSize = (uint32_t)jsonInt(meshMetadataJson, "marker_size", 0);
    uint32_t supportActionOffset = (uint32_t)jsonInt(meshMetadataJson, "action_offset", 0);
    uint32_t supportActionSize = (uint32_t)jsonInt(meshMetadataJson, "action_size", 0);
    uint32_t supportMediaCoverageOffset = (uint32_t)jsonInt(meshMetadataJson, "media_coverage_offset", 0);
    uint32_t supportMediaCoverageSize = (uint32_t)jsonInt(meshMetadataJson, "media_coverage_size", 0);
    uint32_t supportMeshLodOffset = (uint32_t)jsonInt(meshMetadataJson, "mesh_lod_offset", 0);
    uint32_t supportMeshLodSize = (uint32_t)jsonInt(meshMetadataJson, "mesh_lod_size", 0);
    uint32_t supportTrailingOffsetA = (uint32_t)jsonInt(meshMetadataJson, "trailing_offset_a", 0);
    uint32_t supportTrailingSizeA = (uint32_t)jsonInt(meshMetadataJson, "trailing_size_a", 0);
    uint32_t supportTrailingOffsetB = (uint32_t)jsonInt(meshMetadataJson, "trailing_offset_b", 0);
    uint32_t supportTrailingSizeB = (uint32_t)jsonInt(meshMetadataJson, "trailing_size_b", 0);
    uint32_t supportTailSize = (uint32_t)jsonInt(meshMetadataJson, "post_action_tail_size", 0);
    uint32_t supportAppendixSize = (uint32_t)jsonInt(meshMetadataJson, "post_data_appendix_size", 0);
    std::vector<uint8_t> connectorDescriptorRaw =
        decodeHex(jsonStr(meshMetadataJson, "connector_trailing_descriptor_raw_hex"));
    bool haveConnectorDescriptor = connectorDescriptorRaw.size() == 16;
    std::vector<uint8_t> supportHeader = readFile(folder + "/mesh_support/header.bin");
    std::string supportCellGridJson = readText(folder + "/mesh_support/cell_grid.json");
    std::vector<uint8_t> supportCellGrid = readFile(folder + "/mesh_support/cell_grid.bin");
    std::string supportUnitTypesJson = readText(folder + "/mesh_support/unit_types.json");
    std::vector<uint8_t> supportUnitTypes = readFile(folder + "/mesh_support/unit_types.bin");
    std::string supportInstancesJson = readText(folder + "/mesh_support/source_instances.json");
    std::vector<uint8_t> supportInstances = readFile(folder + "/mesh_support/source_instances.bin");
    std::vector<uint8_t> supportTail = readFile(folder + "/mesh_support/post_action_tail.bin");
    std::vector<uint8_t> supportAppendix = readFile(folder + "/mesh_support/post_data_appendix.bin");
    bool haveSupportUnitTypesBin = supportUnitTypeSize != 0 && supportUnitTypeSize == supportUnitTypes.size() &&
                                   supportUnitTypeCount != 0 && (supportUnitTypeSize % 32) == 0 &&
                                   supportUnitTypeCount == (supportUnitTypeSize / 32);
    bool haveSupportUnitTypes = false;
    bool haveSupportInstancesBin = supportMarkerSize != 0 && supportMarkerSize == supportInstances.size() &&
                                   supportMarkerCount != 0 && (supportMarkerSize % 64) == 0 &&
                                   supportMarkerCount == (supportMarkerSize / 64);
    bool haveSupportInstances = false;
    bool haveSupportTail = supportTail.size() == supportTailSize;
    bool haveSupportAppendix = supportAppendix.size() == supportAppendixSize;
    bool haveSupportCellGrid = false;
    bool haveSupportCellGridBin = false;
    std::vector<uint8_t> sourceMesh;
    bool needSourceMesh = (!haveSupportUnitTypesBin && supportUnitTypesJson.empty()) ||
                          (!haveSupportInstancesBin && supportInstancesJson.empty()) ||
                          preservedMarkers.empty() || !haveSupportTail || !haveSupportAppendix;
    if (needSourceMesh) sourceMesh = readFile(folder + "/raw/mesh_tag.bin");
    const uint8_t* sourceHeader = nullptr;
    if (supportHeader.size() >= 1024) sourceHeader = supportHeader.data();
    else if (sourceMesh.size() >= 1024) sourceHeader = sourceMesh.data();
    std::vector<SourceInstanceRecord> sourceInstances;
    std::vector<SourceUnitTypeRecord> sourceUnitTypeRecords;
    loadUnitTypeRecordsFromJson(supportUnitTypesJson, sourceUnitTypeRecords);
    if (!sourceUnitTypeRecords.empty() && sourceUnitTypeRecords.size() == supportUnitTypeCount) {
        haveSupportUnitTypes = true;
    } else if (haveSupportUnitTypesBin) {
        loadFixedRecords(supportUnitTypes, 32, sourceUnitTypeRecords);
        haveSupportUnitTypes = !sourceUnitTypeRecords.empty();
    }
    loadSourceInstancesFromJson(supportInstancesJson, sourceInstances);
    if (!sourceInstances.empty() && sourceInstances.size() == supportMarkerCount) {
        haveSupportInstances = true;
    } else if (haveSupportInstancesBin) {
        loadFixedRecords(supportInstances, 64, sourceInstances);
        haveSupportInstances = !sourceInstances.empty();
    }
    if (sourceMesh.size() >= 1024) {
        uint32_t sourceMarkerCountAll = r32(sourceMesh.data(), 0x34);
        uint32_t sourceMarkerOffsetAll = r32(sourceMesh.data(), 0x38);
        uint32_t sourceUnitTypeCountAll = r32(sourceMesh.data(), 0x24);
        uint32_t sourceUnitTypeOffsetAll = r32(sourceMesh.data(), 0x28);
        size_t sourceBaseAll = 1024 + (size_t)sourceMarkerOffsetAll;
        if (sourceInstances.empty() &&
            sourceBaseAll + (size_t)sourceMarkerCountAll * 64 <= sourceMesh.size()) {
            sourceInstances.resize(sourceMarkerCountAll);
            for (uint32_t i = 0; i < sourceMarkerCountAll; i++) {
                size_t off = sourceBaseAll + (size_t)i * 64;
                sourceInstances[i].raw.assign(sourceMesh.begin() + (ptrdiff_t)off,
                                              sourceMesh.begin() + (ptrdiff_t)off + 64);
            }
        }
        size_t sourceUnitTypeBaseAll = 1024 + (size_t)sourceUnitTypeOffsetAll;
        if (sourceUnitTypeRecords.empty() &&
            sourceUnitTypeBaseAll + (size_t)sourceUnitTypeCountAll * 32 <= sourceMesh.size()) {
            sourceUnitTypeRecords.reserve(sourceUnitTypeCountAll);
            for (uint32_t i = 0; i < sourceUnitTypeCountAll; i++) {
                size_t off = sourceUnitTypeBaseAll + (size_t)i * 32;
                SourceUnitTypeRecord rec;
                rec.raw.assign(sourceMesh.begin() + (ptrdiff_t)off,
                               sourceMesh.begin() + (ptrdiff_t)off + 32);
                sourceUnitTypeRecords.push_back(std::move(rec));
            }
        }
        if (preservedMarkers.empty()) {
            uint32_t sourceMarkerCount = r32(sourceMesh.data(), 0x34);
            uint32_t sourceMarkerOffset = r32(sourceMesh.data(), 0x38);
            size_t sourceBase = 1024 + (size_t)sourceMarkerOffset;
            if (sourceBase + (size_t)sourceMarkerCount * 64 <= sourceMesh.size()) {
                for (uint32_t i = 0; i < sourceMarkerCount; i++) {
                    size_t off = sourceBase + (size_t)i * 64;
                    if (r16s(sourceMesh.data(), off + 4) != 0) continue;
                    PreservedMarkerRecord rec;
                    rec.raw.assign(sourceMesh.begin() + (ptrdiff_t)off,
                                   sourceMesh.begin() + (ptrdiff_t)off + 64);
                    preservedMarkers.push_back(std::move(rec));
                }
            }
        }
        if (lightingTag.size() != 4 && sourceHeader) {
            uint32_t rawLightingTag = r32(sourceHeader, 0x44);
            if (rawLightingTag != 0xFFFFFFFFu && rawLightingTag != 0u) {
                lightingTag.assign({
                    (char)((rawLightingTag >> 24) & 0xFF),
                    (char)((rawLightingTag >> 16) & 0xFF),
                    (char)((rawLightingTag >> 8) & 0xFF),
                    (char)(rawLightingTag & 0xFF)
                });
            }
        }
        if (connectorTag.size() != 4 && sourceHeader) {
            uint32_t rawConnectorTag = r32(sourceHeader, 0x48);
            if (rawConnectorTag != 0xFFFFFFFFu && rawConnectorTag != 0u) {
                connectorTag.assign({
                    (char)((rawConnectorTag >> 24) & 0xFF),
                    (char)((rawConnectorTag >> 16) & 0xFF),
                    (char)((rawConnectorTag >> 8) & 0xFF),
                    (char)(rawConnectorTag & 0xFF)
                });
            }
        }
        if (nextMeshTag.size() != 4 && sourceHeader) {
            uint32_t rawNextMeshTag = r32(sourceHeader, 0x9C);
            if (rawNextMeshTag != 0xFFFFFFFFu && rawNextMeshTag != 0u) {
                nextMeshTag.assign({
                    (char)((rawNextMeshTag >> 24) & 0xFF),
                    (char)((rawNextMeshTag >> 16) & 0xFF),
                    (char)((rawNextMeshTag >> 8) & 0xFF),
                    (char)(rawNextMeshTag & 0xFF)
                });
            }
        }
        if (nextMeshAlternateTag.size() != 4 && sourceHeader) {
            uint32_t rawNextMeshAlternateTag = r32(sourceHeader, 0xA0);
            if (rawNextMeshAlternateTag != 0xFFFFFFFFu && rawNextMeshAlternateTag != 0u) {
                nextMeshAlternateTag.assign({
                    (char)((rawNextMeshAlternateTag >> 24) & 0xFF),
                    (char)((rawNextMeshAlternateTag >> 16) & 0xFF),
                    (char)((rawNextMeshAlternateTag >> 8) & 0xFF),
                    (char)(rawNextMeshAlternateTag & 0xFF)
                });
            }
        }
        if (sourceHeader) {
            edgeNorth = r16(sourceHeader, 116);
            edgeEast  = r16(sourceHeader, 118);
            edgeSouth = r16(sourceHeader, 120);
            edgeWest  = r16(sourceHeader, 122);
        }
        if (!haveWindTag && sourceHeader) {
            windTagRef = r32(sourceHeader, 0xE0);
            haveWindTag = true;
        }
        for (int i = 0; i < 3; i++) {
            if (!haveScreenTagRefs[i] && sourceHeader) {
                screenTagRefs[i] = r32(sourceHeader, 0xE4 + (size_t)i * 4);
                haveScreenTagRefs[i] = true;
            }
        }
        if (!haveWinAmbientRef && sourceHeader) {
            winAmbientRef = r32(sourceHeader, 0x104);
            haveWinAmbientRef = true;
        }
        if (!haveLossAmbientRef && sourceHeader) {
            lossAmbientRef = r32(sourceHeader, 0x108);
            haveLossAmbientRef = true;
        }
        if (!haveHintsRef && sourceHeader) {
            hintsRef = r32(sourceHeader, 0x1EC);
            haveHintsRef = true;
        }
        if (jsonStr(manifest, "fog_color_raw_hex").empty() && sourceHeader) {
            memcpy(fogColorRaw, sourceHeader + 0x1F0, 8);
        }
        if (sourceHeader) fogDensity = r32(sourceHeader, 0x1F8);
    }

    uint32_t sourceDataSize = 0;
    uint32_t sourceActionOffset = 0;
    uint32_t sourceActionSize = 0;
    uint32_t sourceMediaCoverageOffset = 0;
    uint32_t sourceMediaCoverageSize = 0;
    uint32_t sourceMeshLodOffset = 0;
    uint32_t sourceMeshLodSize = 0;
    uint32_t sourceTrailingOffsetA = 0;
    uint32_t sourceTrailingSizeA = 0;
    uint32_t sourceTrailingOffsetB = 0;
    uint32_t sourceTrailingSizeB = 0;
    std::vector<uint8_t> sourceTail = supportTail;
    std::vector<uint8_t> sourceAppendix = supportAppendix;
    if (supportActionSize != 0) sourceActionSize = supportActionSize;
    if (supportActionOffset != 0) sourceActionOffset = supportActionOffset;
    if (supportMediaCoverageSize != 0 || !supportTail.empty()) {
        sourceMediaCoverageOffset = supportMediaCoverageOffset;
        sourceMediaCoverageSize = supportMediaCoverageSize;
    }
    if (supportMeshLodSize != 0 || !supportTail.empty()) {
        sourceMeshLodOffset = supportMeshLodOffset;
        sourceMeshLodSize = supportMeshLodSize;
    }
    if (supportTrailingSizeA != 0 || !supportTail.empty()) {
        sourceTrailingOffsetA = supportTrailingOffsetA;
        sourceTrailingSizeA = supportTrailingSizeA;
    }
    if (supportTrailingSizeB != 0 || !supportTail.empty()) {
        sourceTrailingOffsetB = supportTrailingOffsetB;
        sourceTrailingSizeB = supportTrailingSizeB;
    }
    if (sourceMesh.size() >= 1024) {
        sourceDataSize = r32(sourceMesh.data(), 28);
        if (sourceActionOffset == 0) sourceActionOffset = r32(sourceMesh.data(), 132);
        if (sourceActionSize == 0) sourceActionSize = r32(sourceMesh.data(), 136);
        if (sourceMediaCoverageOffset == 0 && sourceMediaCoverageSize == 0) {
            sourceMediaCoverageOffset = r32(sourceMesh.data(), 192);
            sourceMediaCoverageSize = r32(sourceMesh.data(), 196);
        }
        if (sourceMeshLodOffset == 0 && sourceMeshLodSize == 0) {
            sourceMeshLodOffset = r32(sourceMesh.data(), 204);
            sourceMeshLodSize = r32(sourceMesh.data(), 208);
        }
        if (sourceTrailingOffsetA == 0 && sourceTrailingSizeA == 0) {
            sourceTrailingOffsetA = r32(sourceMesh.data(), 276);
            sourceTrailingSizeA = r32(sourceMesh.data(), 280);
        }
        if (sourceTrailingOffsetB == 0 && sourceTrailingSizeB == 0) {
            sourceTrailingOffsetB = r32(sourceMesh.data(), 288);
            sourceTrailingSizeB = r32(sourceMesh.data(), 292);
        }

        size_t sourceEndActions = (size_t)sourceActionOffset + (size_t)sourceActionSize;
        size_t sourceDataEnd = (size_t)sourceDataSize;
        if (sourceTail.empty() &&
            sourceDataEnd >= sourceEndActions &&
            sourceMesh.size() >= 1024 + sourceDataEnd &&
            sourceDataEnd > sourceEndActions) {
            sourceTail.assign(sourceMesh.begin() + (ptrdiff_t)(1024 + sourceEndActions),
                              sourceMesh.begin() + (ptrdiff_t)(1024 + sourceDataEnd));
        }
        if (sourceAppendix.empty() && sourceMesh.size() > 1024 + sourceDataEnd) {
            sourceAppendix.assign(sourceMesh.begin() + (ptrdiff_t)(1024 + sourceDataEnd),
                                  sourceMesh.end());
        }
    }

    int cellW = subW * 32, cellH = subH * 32;
    int totalCells = cellW * cellH;
    size_t cellDataSize = (size_t)totalCells * 12;
    haveSupportCellGridBin = !supportCellGrid.empty() &&
                             supportMeshSize == supportCellGrid.size() &&
                             supportCellGrid.size() == cellDataSize;
    std::vector<SourceCellRecord> sourceCellRecords;
    loadCellGridFromJson(supportCellGridJson, sourceCellRecords);
    if (!sourceCellRecords.empty() && sourceCellRecords.size() == (size_t)totalCells) {
        haveSupportCellGrid = true;
    } else if (haveSupportCellGridBin) {
        haveSupportCellGrid = true;
    }
    if (!haveSupportCellGrid && sourceMesh.empty()) {
        sourceMesh = readFile(folder + "/raw/mesh_tag.bin");
    }

    printf("Mesh: %s, %dx%d cells\n", meshTag.c_str(), cellW, cellH);

    // ---- Parse JSON instances from all sources ----
    std::vector<JsonInst> allInsts;
    // Build marker type map: tag -> marker type based on which file it first appears in
    std::unordered_map<std::string, int> tagToMT;

    auto parseWithMT = [&](const std::string& path, const std::string& key, int mt) {
        std::string c = readText(path);
        if (c.empty()) return;
        size_t start = c.find("\"" + key + "\":"); if (start == std::string::npos) return;
        size_t arrStart = c.find('[', start); if (arrStart == std::string::npos) return;
        // Find closing bracket of this array so we don't read into sibling arrays
        int arrDepth = 1; size_t arrEnd = arrStart + 1;
        while (arrEnd < c.size() && arrDepth > 0) {
            if (c[arrEnd] == '[') arrDepth++;
            else if (c[arrEnd] == ']') arrDepth--;
            if (arrDepth > 0) arrEnd++;
        }
        size_t pos = arrStart;
        while (true) {
            size_t ob = c.find('{', pos);
            if (ob == std::string::npos || ob > arrEnd) break;
            int depth = 1; size_t cb = ob + 1;
            while (cb < c.size() && depth > 0) {
                if (c[cb] == '{') depth++; else if (c[cb] == '}') depth--;
                if (depth > 0) cb++;
            }
            if (depth != 0) break;
            std::string obj = c.substr(ob, cb - ob + 1);
            std::string tag = jsonStr(obj, "tag");
            if (tag.empty()) { pos = cb + 1; continue; }
            JsonInst ji;
            ji.tag = tag; ji.x = jsonDbl(obj, "x", 0); ji.y = jsonDbl(obj, "y", 0);
            ji.z = jsonDbl(obj, "z", 0); ji.facingDeg = jsonDbl(obj, "facing_deg", 0);
            ji.palIdx = jsonInt(obj, "pal_idx", 0); ji.markerIdx = jsonInt(obj, "marker_idx", -1);
            ji.identifier = jsonInt(obj, "identifier", -1);
            ji.permutation = jsonInt(obj, "permutation", 0);
            ji.pitchRaw = jsonInt(obj, "pitch_raw", -1);
            ji.rollRaw = jsonInt(obj, "roll_raw", -1);
            if (ji.markerIdx < 0) { pos = cb + 1; continue; }
            allInsts.push_back(ji);
            if (tagToMT.find(ji.tag) == tagToMT.end())
                tagToMT[ji.tag] = mt;
            pos = cb + 1;
        }
    };

    parseWithMT(folder + "/assets/sprites/units.json", "units", 3);
    parseWithMT(folder + "/assets/sprites/scenery.json", "scenery", 1);
    parseWithMT(folder + "/placement.json", "instances", 6);
    parseWithMT(folder + "/placement.json", "animations", 11);
    parseWithMT(folder + "/assets/sounds/sounds.json", "sounds", 5);
    parseWithMT(folder + "/assets/models/projectiles.json", "projectiles", 9);

    std::sort(allInsts.begin(), allInsts.end(), [](const JsonInst& a, const JsonInst& b) {
        if (a.markerIdx >= 0 && b.markerIdx >= 0 && a.markerIdx != b.markerIdx) return a.markerIdx < b.markerIdx;
        if (a.markerIdx >= 0 && b.markerIdx < 0) return true;
        if (a.markerIdx < 0 && b.markerIdx >= 0) return false;
        if (a.palIdx != b.palIdx) return a.palIdx < b.palIdx;
        return a.tag < b.tag;
    });

    printf("Parsed %zu instances\n", allInsts.size());

    std::vector<UnitTypeDef> unitTypes;
    std::unordered_map<uint32_t, int> paletteSlotToTypeIdx;
    auto paletteKey = [](uint16_t markerType, int palIdx) -> uint32_t {
        return ((uint32_t)markerType << 16) | (uint32_t)(uint16_t)palIdx;
    };

    bool preserveSourceUnitTypeTable = !sourceUnitTypeRecords.empty();

    if (!preserveSourceUnitTypeTable) {
        for (const auto& ji : allInsts) {
            auto mtIt = tagToMT.find(ji.tag);
            uint16_t mt = (mtIt != tagToMT.end()) ? (uint16_t)mtIt->second : 3;
            uint32_t key = paletteKey(mt, ji.palIdx);
            if (paletteSlotToTypeIdx.find(key) != paletteSlotToTypeIdx.end()) continue;

            UnitTypeDef u;
            u.markerType = mt;
            u.typeTag = tag4(ji.tag);
            u.paletteIndex = ji.palIdx;
            u.teamIndex = (mt == 3 || mt == 0) ? 0u : 0xFFFFu;
            paletteSlotToTypeIdx[key] = (int)unitTypes.size();
            unitTypes.push_back(u);
        }

        bool haveTeamStartType = false;
        for (const auto& ut : unitTypes) {
            if (ut.markerType == 0 && ut.typeTag == 0) {
                haveTeamStartType = true;
                break;
            }
        }
        if (!haveTeamStartType) {
            UnitTypeDef ts;
            ts.markerType = 0;
            ts.typeTag = 0;
            ts.paletteIndex = 0xFFFF;
            ts.teamIndex = 0;
            unitTypes.push_back(ts);
        }
    }

    printf("Built %d unit types\n", preserveSourceUnitTypeTable ? (int)sourceUnitTypeRecords.size() : (int)unitTypes.size());

    // ---- Count instances per type ----
    if (!preserveSourceUnitTypeTable) {
        for (auto& ut : unitTypes) ut.instanceCount = 0;
        for (auto& ji : allInsts) {
            auto mtIt = tagToMT.find(ji.tag);
            uint16_t mt = (mtIt != tagToMT.end()) ? (uint16_t)mtIt->second : 3;
            auto it = paletteSlotToTypeIdx.find(paletteKey(mt, ji.palIdx));
            if (it != paletteSlotToTypeIdx.end()) unitTypes[it->second].instanceCount++;
        }
        for (auto& ut : unitTypes) {
            if (ut.markerType == 0) ut.instanceCount += (int)preservedMarkers.size();
        }
    }

    // ---- Assemble sections and header ----
    size_t utOff = cellDataSize;
    size_t utSz = (preserveSourceUnitTypeTable ? sourceUnitTypeRecords.size() : unitTypes.size()) * 32;
    size_t instOff = utOff + utSz;
    size_t totalMarkerCount = allInsts.size() + preservedMarkers.size();
    size_t instSz = totalMarkerCount * 64;
    size_t actOff = instOff + instSz;
    // Preserve a source-sized placeholder action span so downstream rebuilds can
    // replace it in place without having to reflow every post-action section.
    size_t actSz = sourceActionSize;
    size_t tailOff = actOff + actSz;
    size_t tailSz = sourceTail.size();

    // Actions: build_plugin --edit handles the contents, so keep a zero-filled gap.
    std::vector<uint8_t> actionBuf(actSz, 0);
    printf("Actions: build_plugin handles actions\n");

    // ---- Total data size ----
    size_t dataSize = tailOff + tailSz;
    size_t fileSize = 1024 + dataSize + sourceAppendix.size();

    // ---- Build the file ----
    std::vector<uint8_t> out(fileSize, 0);

    // Header (1024 bytes) — fields from Python MeshHeaderFmt codec
    // [0-3]   landscape_collection_tag
    memcpy(out.data() + 0, landTag.c_str(), 4);
    // [4-7]   media_tag
    std::string mediaTag = jsonStr(manifest, "media_tag");
    if (mediaTag.empty()) mediaTag = "wate";
    memcpy(out.data() + 4, mediaTag.c_str(), 4);
    // [8-9]   submesh_width
    w16(out.data(), 8, (uint16_t)subW);
    // [10-11] submesh_height
    w16(out.data(), 10, (uint16_t)subH);
    // [12-15] mesh_offset
    w32(out.data(), 12, 0);
    // [16-19] mesh_size
    w32(out.data(), 16, (uint32_t)cellDataSize);
    // [20-23] runtime: mesh_ptr — leave 0
    // [24-27] data_offset
    w32(out.data(), 24, 1024);
    // [28-31] data_size
    w32(out.data(), 28, (uint32_t)dataSize);
    // [32-35] runtime: data_ptr — leave 0
    // [36-39] marker_palette_entries
    w32(out.data(), 36, (uint32_t)(preserveSourceUnitTypeTable ? sourceUnitTypeRecords.size() : unitTypes.size()));
    // [40-43] marker_palette_offset
    w32(out.data(), 40, (uint32_t)utOff);
    // [44-47] marker_palette_size
    w32(out.data(), 44, (uint32_t)utSz);
    // [48-51] runtime: marker_palette_ptr — leave 0
    // [52-55] marker_count
    w32(out.data(), 52, (uint32_t)totalMarkerCount);
    // [56-59] markers_offset
    w32(out.data(), 56, (uint32_t)instOff);
    // [60-63] markers_size
    w32(out.data(), 60, (uint32_t)instSz);
    // [64-67] runtime: markers_ptr — leave 0
    // [68-71] mesh_lighting_tag — preserve from manifest or source mesh when available.
    if (lightingTag.size() == 4) memcpy(out.data() + 68, lightingTag.c_str(), 4);
    else w32(out.data(), 68, 0xFFFFFFFFu);
    // [72-75] connector_tag
    if (connectorTag.size() == 4) memcpy(out.data() + 72, connectorTag.c_str(), 4);
    else w32(out.data(), 72, 0xFFFFFFFFu);
    // [76-79] flags — from manifest, default: SINGLE_PLAYER_MAP
    uint32_t mf = (uint32_t)jsonInt(manifest, "mesh_flags", 0);
    w32(out.data(), 76, mf);
    // [80-83] particle_system_tag — -1 (none)
    w32(out.data(), 80, 0xFFFFFFFFu);
    // [84-87] team_count
    w32(out.data(), 84, 2);
    // [88-89] dark_fraction
    w16(out.data(), 88, 229);
    // [90-91] light_fraction
    w16(out.data(), 90, 25);
    // [92-99] dark_color
    w32(out.data(), 92, 0x00000000u);
    w32(out.data(), 96, 0x0F5C0000u);
    // [100-107] light_color
    w32(out.data(), 100, 0xFFFFA147u);
    w32(out.data(), 104, 0x00000000u);
    // [108-111] transition_point
    w32(out.data(), 108, 62259);
    // [112-113] ceiling_height — Vengeance default: 32 world units.
    w16(out.data(), 112, (uint16_t)(32 * 512));
    // [114-115] unused — 0
    // [116-123] edge_of_mesh_buffer_zones — preserve source values when available.
    w16(out.data(), 116, (uint16_t)edgeNorth);
    w16(out.data(), 118, (uint16_t)edgeEast);
    w16(out.data(), 120, (uint16_t)edgeSouth);
    w16(out.data(), 122, (uint16_t)edgeWest);
    // [124-127] action section signature. Stock Myth II meshes use 'amds'
    // before the map action count/offset/size fields.
    memcpy(out.data() + 124, "amds", 4);

    // [128-131] map_action_count — build_plugin fills in
    w32(out.data(), 128, 0);
    // [132-135] map_actions_offset — must be valid location even with 0 actions
    w32(out.data(), 132, (uint32_t)actOff);
    // [136-139] map_action_buffer_size — preserve source-sized placeholder region
    w32(out.data(), 136, (uint32_t)actSz);
    // [140-143] map_description_string_list_tag
    std::string nameStli = jsonStr(manifest, "map_name_stli");
    if (nameStli.size() == 4) memcpy(out.data()+140, nameStli.c_str(), 4);
    // [144-147] postgame_collection_tag
    std::string postgame = jsonStr(manifest, "postgame_256");
    if (postgame.size() == 4) memcpy(out.data()+144, postgame.c_str(), 4);
    // [148-151] pregame_collection_tag
    std::string pregame = jsonStr(manifest, "pregame_256");
    if (pregame.size() == 4) memcpy(out.data()+148, pregame.c_str(), 4);
    // [152-155] overhead_map_collection_tag
    std::string overhead = jsonStr(manifest, "overhead_256");
    if (overhead.size() == 4) memcpy(out.data()+152, overhead.c_str(), 4);
    // [156-159] next_mesh
    if (nextMeshTag.size() == 4) memcpy(out.data()+156, nextMeshTag.c_str(), 4);
    else w32(out.data(), 156, 0xFFFFFFFFu);
    // [160-163] next_mesh_alternate
    if (nextMeshAlternateTag.size() == 4) memcpy(out.data()+160, nextMeshAlternateTag.c_str(), 4);
    else w32(out.data(), 160, 0xFFFFFFFFu);
    // [164-171] cutscene tags
    // [172-175] cutscene_failure
    // [176-179] pregame_storyline_tag
    std::string preStory = jsonStr(manifest, "pregame_storyline_text");
    if (preStory.size() == 4) memcpy(out.data()+176, preStory.c_str(), 4);
    // [180-191] storyline tags 2-4 — 0

    auto remapSourceTailOffset = [&](uint32_t sourceOffset) -> uint32_t {
        size_t sourceTailBase = (size_t)sourceActionOffset + (size_t)sourceActionSize;
        if (sourceOffset <= sourceTailBase) return (uint32_t)tailOff;
        return (uint32_t)(tailOff + ((size_t)sourceOffset - sourceTailBase));
    };

    // [192-195] media_coverage_region_offset — preserve source tail layout when available.
    if (!sourceTail.empty() && sourceMediaCoverageSize != 0) w32(out.data(), 192, remapSourceTailOffset(sourceMediaCoverageOffset));
    else w32(out.data(), 192, (uint32_t)dataSize);
    // [196-199] media_coverage_region_size
    if (!sourceTail.empty()) w32(out.data(), 196, sourceMediaCoverageSize);
    // [200-203] runtime — 0
    // [204-207] mesh_LOD_data_offset — preserve source tail layout when available.
    if (!sourceTail.empty() && sourceMeshLodSize != 0) w32(out.data(), 204, remapSourceTailOffset(sourceMeshLodOffset));
    else w32(out.data(), 204, (uint32_t)dataSize);
    // [208-211] mesh_LOD_data_size
    if (!sourceTail.empty()) w32(out.data(), 208, sourceMeshLodSize);
    // [212-215] runtime — 0
    // [216-223] global_tint_color — 0
    // [224-225] global_tint_fraction — 0
    // [226-227] pad — 0

    // [224-227] wind_tag
    if (haveWindTag) w32(out.data(), 224, windTagRef);
    // [228-239] screen_collection_tags
    for (int i = 0; i < 3; i++) {
        if (haveScreenTagRefs[i]) w32(out.data(), 228 + (size_t)i * 4, screenTagRefs[i]);
    }
    // [244-251] blood_color — red tint (matching original)
    w32(out.data(), 244, 0xFFFF0000u);
    w32(out.data(), 248, 0x00000000u);
    // [252-255] picture_caption_string_list_tag — 0
    // [256-259] narration_sound_tag
    std::string narr = jsonStr(manifest, "narration_sound");
    if (narr.size() == 4) memcpy(out.data()+256, narr.c_str(), 4);
    // [260-263] win_ambient_sound_tag
    if (haveWinAmbientRef) w32(out.data(), 260, winAmbientRef);
    else w32(out.data(), 260, 0xFFFFFFFFu);
    // [264-267] loss_ambient_sound_tag
    if (haveLossAmbientRef) w32(out.data(), 264, lossAmbientRef);
    else w32(out.data(), 264, 0xFFFFFFFFu);

    // [268-271] reverb_environment — 0
    // [272-283] preserve Myth II trailing post-action section when available.
    if (!sourceTail.empty()) {
        if (sourceTrailingSizeA != 0) w32(out.data(), 276, remapSourceTailOffset(sourceTrailingOffsetA));
        else w32(out.data(), 276, (uint32_t)dataSize);
        w32(out.data(), 280, sourceTrailingSizeA);
    }
    // [284-299] Myth II connector/trailing descriptor block.
    // We do not understand this block well enough yet, so preserve it from the
    // exported source header when available. This is needed for fence connectors.
    if (sourceHeader) {
        memcpy(out.data() + 284, sourceHeader + 284, 16);
    } else if (haveConnectorDescriptor) {
        memcpy(out.data() + 284, connectorDescriptorRaw.data(), 16);
    }

    // [300-491] cutscene file paths — 0
    // [492-495] hints_string_list_tag
    if (haveHintsRef) w32(out.data(), 492, hintsRef);
    // [496-503] fog_color — preserve raw source bytes when available
    memcpy(out.data() + 496, fogColorRaw, 8);
    // [504-507] fog_density
    w32(out.data(), 504, fogDensity);
    // [508-511] difficulty_level_override_string_list_tag — -1 below if unset
    // [512-515] team_names_override_string_list_tag — 0 is valid/unused
    // [516-547] plugin_name — empty
    // [548-551] extra_flags — 0
    // [552-583] win/loss narration collection/storyline/caption/sound tags — 0 is valid/unused
    // [584-587] minimum_zoom_factor — 0
    // [588-993] padding — 0

    // Convert unset tag-reference fields to -1 (game crashes on null 0x00000000 tags)
    int tagOffs[] = {
        140,144,148,152,156,160,164,168,172,176,180,184,188,
        252,256,492,508,
        -1
    };
    for (int* to = tagOffs; *to >= 0; to++) {
        if (r32(out.data(), (size_t)*to) == 0)
            w32(out.data(), (size_t)*to, 0xFFFFFFFFu);
    }
    // ---- Write cell grid (build_plugin fills with OBJ heights) ----
    if (!sourceCellRecords.empty() && sourceCellRecords.size() == (size_t)totalCells) {
        uint8_t* dst = out.data() + 1024;
        for (size_t i = 0; i < sourceCellRecords.size(); i++) {
            memcpy(dst + i * 12, sourceCellRecords[i].raw.data(), 12);
        }
    } else if (haveSupportCellGridBin) {
        memcpy(out.data() + 1024, supportCellGrid.data(), cellDataSize);
    } else if (sourceMesh.size() >= 1024 + cellDataSize) {
        memcpy(out.data() + 1024, sourceMesh.data() + 1024, cellDataSize);
    } else {
        // Cell grid is already zero from the out.assign() call
        // Set non-zero initial normal word so build_plugin's preserveLegacyNormalWord
        // doesn't keep it as 0 (which prevents normal regeneration from OBJ heights)
        for (int i = 0; i < totalCells; i++) {
            size_t off = 1024 + (size_t)i * 12;
            w16(out.data(), off + 2, 0x0101); // normal word (non-zero so OBJ import regenerates)
            w16(out.data(), off + 4, 0x0066); // flags = grass/grass
            w16(out.data(), off + 6, 0xFFFF); // firstObjectIndex = -1
            w16(out.data(), off + 10, 0xFFFF); // render flags = -1 (matching original)
        }
    }

    // Water/media flags are handled by build_plugin --edit mesh which reads
    // terrain/water.bmp when assets/terrain/water.obj is not present.
    // Workflow: temporarily move water.obj aside so BMP import runs.

    // ---- Write unit type records ----
    // Format per MarkerPaletteEntryFmt:
    //  [0-1] type (uint16)
    //  [2-3] flags (uint16)
    //  [4-7] marker_tag (char[4])
    //  [8-9] team_index (int16)
    //  [10-11] padding
    //  [12-15] netgame_flags (uint32)
    //  [16-25] padding (10 bytes)
    //  [26-31] padding (6 bytes)
    size_t utFileOff = 1024 + utOff;
    std::unordered_map<uint16_t, uint16_t> typeRelIndex;
    if (preserveSourceUnitTypeTable) {
        for (size_t i = 0; i < sourceUnitTypeRecords.size(); i++) {
            size_t o = utFileOff + i * 32;
            memcpy(out.data() + o, sourceUnitTypeRecords[i].raw.data(), 32);
        }
    } else {
        for (size_t i = 0; i < unitTypes.size(); i++) {
            size_t o = utFileOff + i * 32;
            uint16_t relIndex = typeRelIndex[unitTypes[i].markerType]++;
            w16(out.data(), o + 0, unitTypes[i].markerType);
            w16(out.data(), o + 2, unitTypes[i].flags);
            w32(out.data(), o + 4, unitTypes[i].typeTag);
            w16(out.data(), o + 8, unitTypes[i].teamIndex);
            w32(out.data(), o + 12, unitTypes[i].netgameFlags);
            w16(out.data(), o + 28, (uint16_t)unitTypes[i].instanceCount);
            w16(out.data(), o + 30, relIndex);
        }
    }

    // ---- Write instances ----
    size_t instFileOff = 1024 + instOff;
    for (size_t i = 0; i < allInsts.size(); i++) {
        size_t o = instFileOff + i * 64;
        const auto& ji = allInsts[i];
        bool hasSourceRecord = ji.markerIdx >= 0 && (size_t)ji.markerIdx < sourceInstances.size()
                            && sourceInstances[(size_t)ji.markerIdx].raw.size() == 64;
        if (hasSourceRecord) {
            memcpy(out.data() + o, sourceInstances[(size_t)ji.markerIdx].raw.data(), 64);
        }

        // Determine marker type for this instance
        int markerType = 3; // default
        auto mtit = tagToMT.find(ji.tag);
        if (mtit != tagToMT.end()) markerType = mtit->second;

        if (!hasSourceRecord) w32(out.data(), o + 0, 0);          // flags
        w16(out.data(), o + 4, (uint16_t)markerType); // type
        w16(out.data(), o + 6, (uint16_t)ji.palIdx);  // palette_index (type-relative)
        int identifier = ji.identifier >= 0 ? ji.identifier : (10000 + ji.markerIdx);
        w16(out.data(), o + 8, (uint16_t)identifier); // marker identifier
        if (!hasSourceRecord) w16(out.data(), o + 10, 0);          // min_difficulty
        w32(out.data(), o + 12, (uint32_t)lround(ji.x * 512.0));  // pos_x
        w32(out.data(), o + 16, (uint32_t)lround(ji.y * 512.0));  // pos_y
        w32(out.data(), o + 20, (uint32_t)lround(ji.z * 512.0));  // pos_z
        // velocity at 24-31: 0
        w16(out.data(), o + 32, (uint16_t)((uint32_t)lround(ji.facingDeg * (65536.0/360.0)) & 0xFFFF)); // yaw
        uint16_t pitch = (ji.pitchRaw >= 0) ? (uint16_t)ji.pitchRaw : ((markerType == 6 || markerType == 11) ? 0xC000u : 0u);
        uint16_t roll = (ji.rollRaw >= 0) ? (uint16_t)ji.rollRaw : 0u;
        w16(out.data(), o + 34, pitch);
        if (markerType == 6) out[o + 37] = (uint8_t)ji.permutation; // user_data[1]
        // Model-animation markers need data_index 0 here; -1 crashes when the
        // engine drops the first frame.
        w16(out.data(), o + 52, roll);
        if (!hasSourceRecord) w16(out.data(), o + 60, markerType == 11 ? 0 : 0xFFFF);
    }
    for (size_t i = 0; i < preservedMarkers.size(); i++) {
        size_t o = instFileOff + (allInsts.size() + i) * 64;
        memcpy(out.data() + o, preservedMarkers[i].raw.data(), 64);
    }

    // ---- Write actions ----
    if (!actionBuf.empty()) {
        size_t actFileOff = 1024 + actOff;
        memcpy(out.data() + actFileOff, actionBuf.data(), actionBuf.size());
    }
    if (!sourceTail.empty()) {
        size_t tailFileOff = 1024 + tailOff;
        memcpy(out.data() + tailFileOff, sourceTail.data(), sourceTail.size());
    }
    if (!sourceAppendix.empty()) {
        size_t appendixFileOff = 1024 + dataSize;
        memcpy(out.data() + appendixFileOff, sourceAppendix.data(), sourceAppendix.size());
    }

    // ---- Write output ----
    if (!writeFile(outPath, out)) {
        fprintf(stderr, "Cannot write: %s\n", outPath.c_str());
        return 1;
    }
    printf("Wrote: %s (%zu bytes)\n", outPath.c_str(), out.size());
    printf("  Cell grid: %zu bytes (%d cells)\n", cellDataSize, totalCells);
    printf("  Unit types: %zu bytes (%d types)\n", utSz,
           preserveSourceUnitTypeTable ? (int)sourceUnitTypeRecords.size() : (int)unitTypes.size());
    printf("  Instances: %zu bytes (%d instances)\n", instSz, (int)totalMarkerCount);
    printf("  Actions: %zu bytes\n", actSz);
    printf("  Preserved tail: %zu bytes\n", tailSz);
    printf("  Preserved appendix: %zu bytes\n", sourceAppendix.size());

    return 0;
}
