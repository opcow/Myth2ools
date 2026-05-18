// export_map_actions.cpp
// Decode Myth II map action/script data from an extracted mesh tag.
//
// Usage:
//   export_map_actions <map_folder>
//
// Outputs:
//   <map_folder>/assets/actions/actions.json
//   <map_folder>/assets/actions/actions.txt

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static uint16_t swap16(uint16_t n) { return (uint16_t)((n >> 8) | (n << 8)); }
static uint32_t swap32(uint32_t n) {
    return ((n & 0xFF000000u) >> 24) | ((n & 0x00FF0000u) >> 8) |
           ((n & 0x0000FF00u) << 8)  | ((n & 0x000000FFu) << 24);
}
static uint16_t readBE16u(const uint8_t* b, size_t o) {
    uint16_t v;
    memcpy(&v, b + o, 2);
    return swap16(v);
}
static int16_t readBE16s(const uint8_t* b, size_t o) { return (int16_t)readBE16u(b, o); }
static uint32_t readBE32u(const uint8_t* b, size_t o) {
    uint32_t v;
    memcpy(&v, b + o, 4);
    return swap32(v);
}
static int32_t readBE32s(const uint8_t* b, size_t o) { return (int32_t)readBE32u(b, o); }

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
    std::vector<uint8_t> data((size_t)sz);
    if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    fclose(f);
    return data;
}

static bool writeText(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "Cannot write: %s\n", path.c_str());
        return false;
    }
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

static std::string fourCC(const uint8_t* p) {
    if ((p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF) ||
        (p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x00)) {
        return "";
    }
    char s[5] = {(char)p[0], (char)p[1], (char)p[2], (char)p[3], 0};
    return std::string(s, 4);
}

static std::string cleanString(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i]; i++) s.push_back((char)p[i]);
    return s;
}

static void appendJsonString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c >= 0x7F) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                out += buf;
            } else {
                out.push_back((char)c);
            }
            break;
        }
    }
    out.push_back('"');
}

enum ParamType {
    PARAM_FLAG = 0,
    PARAM_STRING = 1,
    PARAM_MONSTER_IDENTIFIER = 2,
    PARAM_ACTION_IDENTIFIER = 3,
    PARAM_ANGLE = 4,
    PARAM_INTEGER = 5,
    PARAM_WORLD_DISTANCE = 6,
    PARAM_FIELD_NAME = 7,
    PARAM_FIXED = 8,
    PARAM_PROJECTILE = 9,
    PARAM_STRING_LIST = 10,
    PARAM_SOUND = 11,
    PARAM_PROJECTILE_OR_WORLD_POINT_2D = 12,
    PARAM_WORLD_POINT_2D = 13,
    PARAM_WORLD_RECTANGLE_2D = 14,
    PARAM_OBJECT_IDENTIFIER = 15,
    PARAM_MODEL_IDENTIFIER = 16,
    PARAM_SOUND_SOURCE_IDENTIFIER = 17,
    PARAM_WORLD_POINT_3D = 18,
    PARAM_LOCAL_PROJECTILE_GROUP_IDENTIFIER = 19,
    PARAM_MODEL_ANIMATION_IDENTIFIER = 20
};

static const char* paramTypeName(uint16_t t) {
    switch ((ParamType)t) {
    case PARAM_FLAG: return "flag";
    case PARAM_STRING: return "string";
    case PARAM_MONSTER_IDENTIFIER: return "monster_identifier";
    case PARAM_ACTION_IDENTIFIER: return "action_identifier";
    case PARAM_ANGLE: return "angle";
    case PARAM_INTEGER: return "integer";
    case PARAM_WORLD_DISTANCE: return "world_distance";
    case PARAM_FIELD_NAME: return "field_name";
    case PARAM_FIXED: return "fixed";
    case PARAM_PROJECTILE: return "projectile";
    case PARAM_STRING_LIST: return "string_list";
    case PARAM_SOUND: return "sound";
    case PARAM_PROJECTILE_OR_WORLD_POINT_2D: return "projectile";
    case PARAM_WORLD_POINT_2D: return "world_point_2d";
    case PARAM_WORLD_RECTANGLE_2D: return "world_rectangle_2d";
    case PARAM_OBJECT_IDENTIFIER: return "object_identifier";
    case PARAM_MODEL_IDENTIFIER: return "model_identifier";
    case PARAM_SOUND_SOURCE_IDENTIFIER: return "sound_source_identifier";
    case PARAM_WORLD_POINT_3D: return "world_point_3d";
    case PARAM_LOCAL_PROJECTILE_GROUP_IDENTIFIER: return "local_projectile_group_identifier";
    case PARAM_MODEL_ANIMATION_IDENTIFIER: return "model_animation_identifier";
    default: return "unknown";
    }
}

static const char* expirationName(uint16_t v) {
    switch (v) {
    case 0: return "trigger";
    case 1: return "execution";
    case 2: return "successful_execution";
    case 3: return "never";
    case 4: return "failed_execution";
    default: return "unknown";
    }
}

static std::vector<std::string> actionFlagNames(uint32_t flags) {
    const char* names[] = {
        "initially_active",
        "activates_only_once",
        "no_initial_delay",
        "only_initial_delay",
        "deleted_on_deactivation"
    };
    std::vector<std::string> out;
    for (int i = 0; i < 5; i++) {
        if (flags & (1u << i)) out.push_back(names[i]);
    }
    return out;
}

static size_t alignTo(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

static void appendNumberArray(std::string& json, const std::vector<double>& vals) {
    json += "[";
    for (size_t i = 0; i < vals.size(); i++) {
        if (i) json += ", ";
        char buf[64];
        snprintf(buf, sizeof(buf), "%.4f", vals[i]);
        json += buf;
    }
    json += "]";
}

static void appendIntArray(std::string& json, const std::vector<int32_t>& vals) {
    json += "[";
    for (size_t i = 0; i < vals.size(); i++) {
        if (i) json += ", ";
        json += std::to_string(vals[i]);
    }
    json += "]";
}

static void appendHexString(std::string& out, const uint8_t* data, size_t size) {
    static const char* hex = "0123456789abcdef";
    out.push_back('"');
    for (size_t i = 0; i < size; i++) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0x0F]);
    }
    out.push_back('"');
}

struct ActionHead {
    uint16_t id = 0;
    uint16_t expirationMode = 0;
    std::string type;
    uint32_t flags = 0;
    double triggerTimeLower = 0.0;
    double triggerTimeDelta = 0.0;
    uint16_t numParams = 0;
    uint16_t size = 0;
    uint32_t offset = 0;
    uint16_t indent = 0;
};

static bool parseActions(const std::vector<uint8_t>& mesh, std::string& json, std::string& text) {
    constexpr size_t MESH_HEADER_SIZE = 1024;
    constexpr size_t ACTION_HEAD_SIZE = 64;
    constexpr double WORLD_POINT_SF = 512.0;
    constexpr double FIXED_SF = 65536.0;
    constexpr double ANGLE_SF = 65536.0 / 360.0;
    constexpr double TIME_SF = 30.0;

    if (mesh.size() < MESH_HEADER_SIZE) {
        fprintf(stderr, "mesh_tag.bin is too small\n");
        return false;
    }

    uint32_t actionCount = readBE32u(mesh.data(), 128);
    uint32_t actionOffset = readBE32u(mesh.data(), 132);
    uint32_t actionBufferSize = readBE32u(mesh.data(), 136);
    size_t actionStart = MESH_HEADER_SIZE + (size_t)actionOffset;
    size_t actionEnd = actionStart + (size_t)actionBufferSize;

    if (actionCount == 0 || actionBufferSize == 0) {
        json = "{\n  \"action_count\": 0,\n  \"actions\": []\n}\n";
        text = "No map actions.\n";
        return true;
    }
    if (actionEnd > mesh.size() || actionCount * ACTION_HEAD_SIZE > actionBufferSize) {
        fprintf(stderr, "Invalid map action buffer: count=%u offset=%u size=%u mesh=%zu\n",
                actionCount, actionOffset, actionBufferSize, mesh.size());
        return false;
    }

    const uint8_t* buf = mesh.data() + actionStart;
    std::vector<ActionHead> heads;
    heads.reserve(actionCount);
    for (uint32_t i = 0; i < actionCount; i++) {
        size_t o = (size_t)i * ACTION_HEAD_SIZE;
        ActionHead h;
        h.id = readBE16u(buf, o + 0);
        h.expirationMode = readBE16u(buf, o + 2);
        h.type = fourCC(buf + o + 4);
        h.flags = readBE32u(buf, o + 8);
        h.triggerTimeLower = (double)readBE32u(buf, o + 12) / TIME_SF;
        h.triggerTimeDelta = (double)readBE32u(buf, o + 16) / TIME_SF;
        h.numParams = readBE16u(buf, o + 20);
        h.size = readBE16u(buf, o + 22);
        h.offset = readBE32u(buf, o + 24);
        h.indent = readBE16u(buf, o + 28);
        heads.push_back(h);
    }

    json = "{\n";
    json += "  \"source\": \"raw/mesh_tag.bin\",\n";
    json += "  \"action_count\": " + std::to_string(actionCount) + ",\n";
    json += "  \"action_buffer_offset\": " + std::to_string(actionOffset) + ",\n";
    json += "  \"action_buffer_size\": " + std::to_string(actionBufferSize) + ",\n";
    json += "  \"actions\": [\n";

    std::ostringstream txt;
    txt << "Map actions: " << actionCount << "\n\n";

    size_t actionHeadEnd = (size_t)actionCount * ACTION_HEAD_SIZE;
    for (size_t ai = 0; ai < heads.size(); ai++) {
        const ActionHead& h = heads[ai];
        size_t paramsStart = actionHeadEnd + h.offset;
        size_t paramsEnd = paramsStart + h.size;
        if (paramsEnd > actionBufferSize) {
            fprintf(stderr, "Action %u parameter data overruns action buffer\n", h.id);
            return false;
        }

        std::string name;
        std::string paramsJson;
        std::ostringstream paramText;
        size_t p = paramsStart;
        for (uint16_t pi = 0; pi < h.numParams; pi++) {
            if (p + 8 > paramsEnd) break;
            uint16_t type = readBE16u(buf, p + 0);
            uint16_t count = readBE16u(buf, p + 2);
            std::string paramName = fourCC(buf + p + 4);
            p += 8;

            std::string valueJson;
            std::string valueText;
            size_t bytes = 0;

            auto failShort = [&]() {
                valueJson = "null";
                valueText = "<truncated>";
                p = paramsEnd;
            };

            if (type == PARAM_STRING) {
                bytes = alignTo(count, 4);
                if (p + bytes > paramsEnd) { failShort(); }
                else {
                    std::string s = cleanString(buf + p, count);
                    appendJsonString(valueJson, s);
                    valueText = s;
                    if (paramName == "name") name = s;
                    p += bytes;
                }
            } else if (type == PARAM_SOUND || type == PARAM_FIELD_NAME ||
                       type == PARAM_PROJECTILE || type == PARAM_STRING_LIST ||
                       type == PARAM_PROJECTILE_OR_WORLD_POINT_2D) {
                // FourCC arrays — verified via Loathing FUN_00479220 cases 7/9/10/11/12.
                // (Doc previously claimed string_list was u16 × count; Loathing's formatter
                //  reads a uint32 and looks it up in the 'stli' tag group.)
                bytes = (size_t)count * 4;
                if (p + bytes > paramsEnd) { failShort(); }
                else {
                    valueJson = "[";
                    for (uint16_t i = 0; i < count; i++) {
                        std::string s = fourCC(buf + p + (size_t)i * 4);
                        if (i) {
                            valueJson += ", ";
                            valueText += ", ";
                        }
                        appendJsonString(valueJson, s);
                        valueText += s;
                    }
                    valueJson += "]";
                    p += bytes;
                }
            } else if (type == PARAM_WORLD_RECTANGLE_2D) {
                // 4 × s32 fixed-point world distance per rectangle. Verified via
                // Loathing case 0xe in FUN_00479220 (4 floats printed per entry,
                // local_11c advances by 4 uints per iteration).
                bytes = (size_t)count * 16;
                if (p + bytes > paramsEnd) { failShort(); }
                else {
                    valueJson = "[";
                    for (uint16_t i = 0; i < count; i++) {
                        size_t base = p + (size_t)i * 16;
                        double x0 = (double)(int32_t)readBE32u(buf, base + 0)  / WORLD_POINT_SF;
                        double y0 = (double)(int32_t)readBE32u(buf, base + 4)  / WORLD_POINT_SF;
                        double x1 = (double)(int32_t)readBE32u(buf, base + 8)  / WORLD_POINT_SF;
                        double y1 = (double)(int32_t)readBE32u(buf, base + 12) / WORLD_POINT_SF;
                        char item[192];
                        snprintf(item, sizeof(item),
                                 "%s{\"x0\": %.4f, \"y0\": %.4f, \"x1\": %.4f, \"y1\": %.4f}",
                                 i ? ", " : "", x0, y0, x1, y1);
                        valueJson += item;
                        snprintf(item, sizeof(item),
                                 "%s(%.4f, %.4f, %.4f, %.4f)",
                                 i ? ", " : "", x0, y0, x1, y1);
                        valueText += item;
                    }
                    valueJson += "]";
                    p += bytes;
                }
            } else if (type == PARAM_WORLD_POINT_2D) {
                bytes = (size_t)count * 8;
                if (p + bytes > paramsEnd) { failShort(); }
                else {
                    valueJson = "[";
                    for (uint16_t i = 0; i < count; i++) {
                        double x = (double)readBE32u(buf, p + (size_t)i * 8 + 0) / WORLD_POINT_SF;
                        double y = (double)readBE32u(buf, p + (size_t)i * 8 + 4) / WORLD_POINT_SF;
                        char item[96];
                        snprintf(item, sizeof(item), "%s{\"x\": %.4f, \"y\": %.4f}", i ? ", " : "", x, y);
                        valueJson += item;
                        snprintf(item, sizeof(item), "%s(%.4f, %.4f)", i ? ", " : "", x, y);
                        valueText += item;
                    }
                    valueJson += "]";
                    p += bytes;
                }
            } else if (type == PARAM_WORLD_POINT_3D) {
                bytes = (size_t)count * 12;
                if (p + bytes > paramsEnd) { failShort(); }
                else {
                    valueJson = "[";
                    for (uint16_t i = 0; i < count; i++) {
                        double x = (double)readBE32u(buf, p + (size_t)i * 12 + 0) / WORLD_POINT_SF;
                        double y = (double)readBE32u(buf, p + (size_t)i * 12 + 4) / WORLD_POINT_SF;
                        double z = (double)readBE32u(buf, p + (size_t)i * 12 + 8) / WORLD_POINT_SF;
                        char item[128];
                        snprintf(item, sizeof(item), "%s{\"x\": %.4f, \"y\": %.4f, \"z\": %.4f}", i ? ", " : "", x, y, z);
                        valueJson += item;
                        snprintf(item, sizeof(item), "%s(%.4f, %.4f, %.4f)", i ? ", " : "", x, y, z);
                        valueText += item;
                    }
                    valueJson += "]";
                    p += bytes;
                }
            } else if (type == PARAM_FIXED || type == PARAM_WORLD_DISTANCE) {
                bytes = (size_t)count * 4;
                if (p + bytes > paramsEnd) { failShort(); }
                else {
                    std::vector<double> vals;
                    double scale = type == PARAM_FIXED ? FIXED_SF : WORLD_POINT_SF;
                    for (uint16_t i = 0; i < count; i++) vals.push_back((double)readBE32u(buf, p + (size_t)i * 4) / scale);
                    appendNumberArray(valueJson, vals);
                    valueText = valueJson;
                    p += bytes;
                }
            } else if (type == PARAM_INTEGER) {
                bytes = (size_t)count * 4;
                if (p + bytes > paramsEnd) { failShort(); }
                else {
                    std::vector<int32_t> vals;
                    for (uint16_t i = 0; i < count; i++) vals.push_back(readBE32s(buf, p + (size_t)i * 4));
                    appendIntArray(valueJson, vals);
                    valueText = valueJson;
                    p += bytes;
                }
            } else if (type == PARAM_FLAG) {
                bytes = alignTo(count, 4);
                if (p + bytes > paramsEnd) { failShort(); }
                else {
                    bool v = count == 0 ? true : buf[p] != 0;
                    valueJson = v ? "true" : "false";
                    valueText = valueJson;
                    p += bytes;
                }
            } else {
                size_t aligned = alignTo(count, 2);
                bytes = aligned * 2;
                if (p + bytes > paramsEnd) { failShort(); }
                else {
                    if (type == PARAM_ANGLE) {
                        std::vector<double> vals;
                        for (uint16_t i = 0; i < count; i++) vals.push_back((double)readBE16u(buf, p + (size_t)i * 2) / ANGLE_SF);
                        appendNumberArray(valueJson, vals);
                    } else {
                        std::vector<int32_t> vals;
                        for (uint16_t i = 0; i < count; i++) vals.push_back((int32_t)readBE16u(buf, p + (size_t)i * 2));
                        appendIntArray(valueJson, vals);
                    }
                    valueText = valueJson;
                    p += bytes;
                }
            }

            if (paramName != "name") {
                if (!paramsJson.empty()) paramsJson += ",\n";
                paramsJson += "        {\"name\": ";
                appendJsonString(paramsJson, paramName);
                paramsJson += ", \"type\": ";
                appendJsonString(paramsJson, paramTypeName(type));
                paramsJson += ", \"type_id\": " + std::to_string(type) + ", \"values\": " + valueJson + "}";

                paramText << std::string(h.indent * 2, ' ') << "    - " << paramName << " "
                          << paramTypeName(type) << "=" << valueText << "\n";
            }
        }

        std::vector<std::string> flags = actionFlagNames(h.flags);
        if (ai) json += ",\n";
        json += "    {\n";
        json += "      \"id\": " + std::to_string(h.id) + ",\n";
        json += "      \"type\": ";
        appendJsonString(json, h.type);
        json += ",\n      \"name\": ";
        appendJsonString(json, name);
        json += ",\n      \"expiration_mode\": ";
        appendJsonString(json, expirationName(h.expirationMode));
        json += ",\n      \"expiration_mode_id\": " + std::to_string(h.expirationMode) + ",\n";
        json += "      \"flags_raw\": " + std::to_string(h.flags) + ",\n";
        json += "      \"flags\": [";
        for (size_t fi = 0; fi < flags.size(); fi++) {
            if (fi) json += ", ";
            appendJsonString(json, flags[fi]);
        }
        json += "],\n";
        char timing[192];
        snprintf(timing, sizeof(timing),
                 "      \"trigger_time_lower_bound_seconds\": %.4f,\n"
                 "      \"trigger_time_delta_seconds\": %.4f,\n",
                 h.triggerTimeLower, h.triggerTimeDelta);
        json += timing;
        json += "      \"indent\": " + std::to_string(h.indent) + ",\n";
        json += "      \"parameter_data_offset\": " + std::to_string(h.offset) + ",\n";
        json += "      \"parameter_data_size\": " + std::to_string(h.size) + ",\n";
        json += "      \"parameter_data_hex\": ";
        appendHexString(json, buf + paramsStart, h.size);
        json += ",\n";
        json += "      \"parameters\": [\n";
        if (!paramsJson.empty()) {
            json += paramsJson;
            json += "\n";
        }
        json += "      ]\n";
        json += "    }";

        txt << "[" << h.id << "] " << std::string(h.indent * 2, ' ');
        if (!h.type.empty()) txt << h.type << ".";
        txt << (name.empty() ? "<unnamed>" : name);
        if (!flags.empty() || h.expirationMode || h.triggerTimeLower || h.triggerTimeDelta) {
            txt << " [";
            bool first = true;
            for (const auto& flag : flags) {
                if (!first) txt << " ";
                first = false;
                txt << flag;
            }
            if (h.expirationMode) {
                if (!first) txt << " ";
                first = false;
                txt << "expiry=" << expirationName(h.expirationMode);
            }
            if (h.triggerTimeLower) {
                if (!first) txt << " ";
                first = false;
                txt << "delay=" << h.triggerTimeLower << "s";
            }
            if (h.triggerTimeDelta) {
                if (!first) txt << " ";
                txt << "dur=" << h.triggerTimeDelta << "s";
            }
            txt << "]";
        }
        txt << "\n" << paramText.str() << "\n";
    }

    json += "\n  ]\n}\n";
    text = txt.str();
    return true;
}

static void usage(const char* p) {
    fprintf(stderr,
            "Usage: %s <map_folder>\n\n"
            "Reads <map_folder>/raw/mesh_tag.bin and writes:\n"
            "  <map_folder>/assets/actions/actions.json\n"
            "  <map_folder>/assets/actions/actions.txt\n",
            p);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }

    std::string folder = argv[1];
    std::string meshPath = folder + "/raw/mesh_tag.bin";
    std::vector<uint8_t> mesh = readFile(meshPath);
    if (mesh.empty()) {
        fprintf(stderr, "Cannot read: %s\n", meshPath.c_str());
        return 1;
    }

    std::string json;
    std::string text;
    if (!parseActions(mesh, json, text)) return 1;

    std::string outDir = folder + "/assets/actions";
    std::error_code ec;
    fs::create_directories(outDir, ec);
    std::string jsonPath = outDir + "/actions.json";
    std::string textPath = outDir + "/actions.txt";
    if (!writeText(jsonPath, json)) return 1;
    if (!writeText(textPath, text)) return 1;

    printf("Map actions exported:\n");
    printf("  %s\n", jsonPath.c_str());
    printf("  %s\n", textPath.c_str());
    return 0;
}
