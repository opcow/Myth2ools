#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl2.h"

#include "fonts/inter_regular.inl"
#include "fonts/jetbrains_mono.inl"

#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <set>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

struct ShellRunner {
    std::atomic<bool> running{false};
    std::mutex logMutex;
    std::deque<std::string> logLines;
    std::thread worker;

    ~ShellRunner() {
        if (worker.joinable()) worker.join();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(logMutex);
        logLines.clear();
    }

    void append(const std::string& line) {
        std::lock_guard<std::mutex> lock(logMutex);
        logLines.push_back(line);
        while (logLines.size() > 4000) logLines.pop_front();
    }

    bool start(const std::string& command) {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true)) return false;
        if (worker.joinable()) worker.join();
        append("$ " + command);
        worker = std::thread([this, command]() {
#if defined(_WIN32)
            FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
#else
            FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
            if (!pipe) {
                append("Failed to start command.");
                running = false;
                return;
            }
            std::array<char, 1024> buffer{};
            while (fgets(buffer.data(), (int)buffer.size(), pipe)) {
                std::string line(buffer.data());
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                append(line);
            }
#if defined(_WIN32)
            int code = _pclose(pipe);
#else
            int code = pclose(pipe);
#endif
            append("Exit code: " + std::to_string(code));
            running = false;
        });
        return true;
    }
};

struct MapUnitMarker {
    std::string tag;
    uint16_t identifier = 0;
    float cellX = 0.0f;
    float cellY = 0.0f;
};

struct MapPreviewPickTarget {
    int actionIndex = -1;
    int paramIndex = -1;
    int valueIndex = -1;
    int typeId = -1;
};

static std::string quoteArg(const fs::path& path) {
    std::string s = path.string();
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

static std::string quoteArg(const std::string& s) {
    return quoteArg(fs::path(s));
}

static std::string quoteArg(const char* s) {
    return quoteArg(std::string(s ? s : ""));
}

static std::string wrapWindowsCommand(const std::string& command) {
#if defined(_WIN32)
    return "cmd /c \"" + command + "\"";
#else
    return command;
#endif
}

static fs::path resolveUserPath(const fs::path& baseDir, const std::string& raw) {
    fs::path p(raw);
    if (p.empty()) return p;
    if (p.is_absolute()) return p;
    return baseDir / p;
}

template <size_t N>
static void copyToBuffer(std::array<char, N>& dst, const std::string& src);

static bool normalizeActionsDocForSave(json& doc, const std::set<int>& dirtyActionIndices, std::string& error);
static bool recomputeActionDocLayout(json& doc, std::string& error);
static void setActionsStatus(struct AppState& state, const std::string& message, bool writeToLog = true);
static void logActionSummary(struct AppState& state, const json& action, const char* phase);
static bool guiParseHexBytes(const std::string& s, std::vector<uint8_t>& out);
static void pushPrimaryButtonStyle();
static void popPrimaryButtonStyle();
static void drawStatusChip(const char* text, const ImVec4& bgColor, const ImVec4& textColor = ImVec4(1, 1, 1, 1));
static void applyLockedDockWindowClass();
static ImGuiWindowFlags lockedDockPanelFlags();
static const char* guiActionTypeLabel(const std::string& fourcc);
extern ImFont* g_monoFont;

#if defined(_WIN32)
static std::wstring widen(const char* text) {
    std::string s = text ? text : "";
    return std::wstring(s.begin(), s.end());
}

static bool pickDialogPath(fs::path& outPath,
                           const fs::path& initialPath,
                           bool saveMode,
                           bool folderMode,
                           const char* title) {
    IFileDialog* dialog = nullptr;
    HRESULT hr = saveMode
        ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))
        : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return false;

    std::wstring titleWide = widen(title);
    dialog->SetTitle(titleWide.c_str());

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM;
        if (folderMode) {
            options |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST;
        } else if (!saveMode) {
            options |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        }
        dialog->SetOptions(options);
    }

    fs::path seed = initialPath;
    fs::path folder = folderMode ? seed : seed.parent_path();
    if (folder.empty() && !seed.empty()) folder = seed;
    if (!folder.empty()) {
        IShellItem* folderItem = nullptr;
        std::wstring folderWide = folder.wstring();
        if (SUCCEEDED(SHCreateItemFromParsingName(folderWide.c_str(), nullptr, IID_PPV_ARGS(&folderItem)))) {
            dialog->SetFolder(folderItem);
            folderItem->Release();
        }
    }

    if (saveMode && !folderMode && !seed.filename().empty()) {
        std::wstring fileName = seed.filename().wstring();
        dialog->SetFileName(fileName.c_str());
    }

    bool ok = false;
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result)) && result) {
            PWSTR widePath = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &widePath)) && widePath) {
                outPath = fs::path(widePath);
                CoTaskMemFree(widePath);
                ok = true;
            }
            result->Release();
        }
    }

    dialog->Release();
    return ok;
}
#else
static std::string trimTrailingWhitespace(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

static bool captureDialogCommand(const std::string& command, std::string& output) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return false;
    std::array<char, 1024> buffer{};
    output.clear();
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    int code = pclose(pipe);
    output = trimTrailingWhitespace(output);
    return code == 0 && !output.empty();
}

static bool commandExists(const char* command) {
    std::string probe = std::string("command -v ") + command + " >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;
}

static bool pickDialogPath(fs::path& outPath,
                           const fs::path& initialPath,
                           bool saveMode,
                           bool folderMode,
                           const char* title) {
    std::string selected;
    const std::string initial = initialPath.empty() ? std::string() : quoteArg(initialPath);

#if defined(__APPLE__)
    std::string script;
    if (folderMode) {
        script = "osascript -e " + quoteArg(std::string("POSIX path of (choose folder with prompt \"") + title + "\")");
    } else if (saveMode) {
        script = "osascript -e " + quoteArg(std::string("POSIX path of (choose file name with prompt \"") + title + "\")");
    } else {
        script = "osascript -e " + quoteArg(std::string("POSIX path of (choose file with prompt \"") + title + "\")");
    }
    if (captureDialogCommand(script, selected)) {
        outPath = fs::path(selected);
        return true;
    }
#endif

    if (commandExists("zenity")) {
        std::string cmd = "zenity --file-selection";
        if (folderMode) cmd += " --directory";
        if (saveMode) cmd += " --save --confirm-overwrite";
        if (!initial.empty()) cmd += " --filename=" + initial;
        cmd += " --title=" + quoteArg(title);
        if (captureDialogCommand(cmd, selected)) {
            outPath = fs::path(selected);
            return true;
        }
    }

    if (commandExists("kdialog")) {
        std::string cmd;
        if (folderMode) {
            cmd = "kdialog --getexistingdirectory " + (initial.empty() ? quoteArg(fs::current_path()) : initial);
        } else if (saveMode) {
            cmd = "kdialog --getsavefilename " + (initial.empty() ? quoteArg(fs::current_path()) : initial);
        } else {
            cmd = "kdialog --getopenfilename " + (initial.empty() ? quoteArg(fs::current_path()) : initial);
        }
        cmd += " --title " + quoteArg(title);
        if (captureDialogCommand(cmd, selected)) {
            outPath = fs::path(selected);
            return true;
        }
    }

    return false;
}
#endif

template <size_t N>
static void pickFolderIntoBuffer(std::array<char, N>& buffer, const fs::path& baseDir) {
    fs::path chosen;
    if (pickDialogPath(chosen, resolveUserPath(baseDir, buffer.data()), false, true, "Select Folder")) {
        copyToBuffer(buffer, chosen.string());
    }
}

template <size_t N>
static void pickFileIntoBuffer(std::array<char, N>& buffer, const fs::path& baseDir, const char* title) {
    fs::path chosen;
    if (pickDialogPath(chosen, resolveUserPath(baseDir, buffer.data()), false, false, title)) {
        copyToBuffer(buffer, chosen.string());
    }
}

template <size_t N>
static void saveFileIntoBuffer(std::array<char, N>& buffer, const fs::path& baseDir, const char* title) {
    fs::path chosen;
    if (pickDialogPath(chosen, resolveUserPath(baseDir, buffer.data()), true, false, title)) {
        copyToBuffer(buffer, chosen.string());
    }
}

template <size_t N>
static void copyToBuffer(std::array<char, N>& dst, const std::string& src) {
    std::snprintf(dst.data(), dst.size(), "%s", src.c_str());
}

static uint16_t readBE16(FILE* f) {
    int a = fgetc(f), b = fgetc(f);
    if (a == EOF || b == EOF) return 0;
    return static_cast<uint16_t>((static_cast<uint16_t>(a) << 8) | static_cast<uint16_t>(b));
}

static uint32_t readBE32(FILE* f) {
    int a = fgetc(f), b = fgetc(f), c = fgetc(f), d = fgetc(f);
    if (a == EOF || b == EOF || c == EOF || d == EOF) return 0;
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) |
           static_cast<uint32_t>(d);
}

static std::string readFixedString(FILE* f, size_t len) {
    std::vector<char> buf(len);
    if (len && fread(buf.data(), 1, len, f) != len) return "";
    size_t n = 0;
    while (n < len && buf[n] != '\0') n++;
    return std::string(buf.data(), n);
}

static std::string tagToString(uint32_t tag) {
    char s[5];
    s[0] = static_cast<char>((tag >> 24) & 0xFF);
    s[1] = static_cast<char>((tag >> 16) & 0xFF);
    s[2] = static_cast<char>((tag >> 8) & 0xFF);
    s[3] = static_cast<char>(tag & 0xFF);
    s[4] = 0;
    return std::string(s, 4);
}

struct GuiTagEntry {
    uint32_t groupTag = 0;
    uint32_t subgroupTag = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    fs::path sourceFile;
    bool isLocal = false;
};

struct MeshChoice {
    std::string tag;
    std::string name;
    std::string campaignLabel;
};

static bool isPrintableTag(const std::string& tag) {
    if (tag.size() != 4) return false;
    for (char c : tag) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc > 126) return false;
    }
    return true;
}

static std::string stliToText(const std::vector<uint8_t>& raw) {
    std::string s;
    s.reserve(raw.size() + 8);
    for (uint8_t c : raw) {
        if (c == '\r') s.push_back('\n');
        else if (c >= 0x20 || c == '\t') s.push_back(static_cast<char>(c));
    }
    return s;
}

static std::string firstLine(const std::string& text) {
    size_t pos = text.find('\n');
    std::string line = pos == std::string::npos ? text : text.substr(0, pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

static bool lookupCampaignMapInfo(const std::string& tag, std::string& outLabel, std::string& outName) {
    struct CampaignEntry { const char* tag; const char* label; const char* name; };
    static const CampaignEntry campaign[] = {
        {"00tm", "00",  "Training Map"},
        {"l1",   "01",  "Village"},
        {"cr01", "02",  "Graveyard"},
        {"le3e", "03",  "Town Gates"},
        {"thkm", "04",  "Outside Keep"},
        {"03hp", "04a", "Hunting Party"},
        {"ink3", "05",  "Inside Keep"},
        {"neme", "06",  "Destroy Bridge"},
        {"07rt", "07",  "Repair the World Knot"},
        {"08li", "08",  "Library"},
        {"shma", "09",  "Escape Madrigal"},
        {"10la", "10",  "Landing"},
        {"11an", "11",  "Ambushed Night"},
        {"12mp", "12",  "Mountain Pass"},
        {"13td", "13",  "Deceiver"},
        {"14tr", "14",  "Trow"},
        {"15cm", "15",  "Capture Muirthemne"},
        {"16ca", "16",  "Catacombs"},
        {"17dm", "17",  "Defend Muirthemne"},
        {"18ts", "18",  "Tain Shard"},
        {"19it", "19",  "Inside the Tain"},
        {"20sc", "20",  "Soulblighter's Camp"},
        {"21md", "21",  "Munitions Dump"},
        {"22da", "22",  "Dam"},
        {"23b1", "23",  "Battle 1"},
        {"24b2", "24",  "Battle 2"},
        {"25so", "25",  "Soulblighter"},
    };
    for (const CampaignEntry& entry : campaign) {
        if (tag == entry.tag) {
            outLabel = entry.label;
            outName = entry.name;
            return true;
        }
    }
    return false;
}

static std::string meshChoiceDisplay(const MeshChoice& choice) {
    std::string display;
    if (!choice.campaignLabel.empty()) {
        display += choice.campaignLabel + " ";
    }
    display += choice.tag;
    if (!choice.name.empty()) {
        display += " - " + choice.name;
    }
    return display;
}

static bool readTagPayload(const GuiTagEntry& entry, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(entry.sourceFile.string().c_str(), "rb");
    if (!f) return false;
    if (std::fseek(f, static_cast<long>(entry.offset), SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize(entry.size);
    bool ok = (entry.size == 0) || (std::fread(out.data(), 1, entry.size, f) == entry.size);
    std::fclose(f);
    return ok;
}

static bool collectTagsFromFile(const fs::path& filePath, std::vector<GuiTagEntry>& outTags) {
    FILE* f = std::fopen(filePath.string().c_str(), "rb");
    if (!f) return false;

    auto closeFile = [&]() { std::fclose(f); };

    std::rewind(f);
    uint16_t type = readBE16(f);
    (void)type;
    uint16_t version = readBE16(f);
    (void)version;
    std::string name = readFixedString(f, 32);
    (void)name;
    std::string url = readFixedString(f, 64);
    (void)url;
    uint16_t entryPointCount = readBE16(f);
    uint16_t tagCount = readBE16(f);
    uint32_t checksum = readBE32(f);
    (void)checksum;
    uint32_t flags = readBE32(f);
    (void)flags;
    uint32_t size = readBE32(f);
    (void)size;
    uint32_t headerChecksum = readBE32(f);
    (void)headerChecksum;
    uint32_t unused0 = readBE32(f);
    (void)unused0;
    uint32_t signature = readBE32(f);

    bool isLocal = false;
    if (signature == 0x646E6732u) {
        isLocal = false;
    } else {
        std::fseek(f, 0x3C, SEEK_SET);
        uint32_t sig = readBE32(f);
        if (sig == 0x6D746832u) {
            isLocal = true;
            entryPointCount = 0;
            tagCount = 1;
        } else {
            closeFile();
            return false;
        }
    }

    long fileLength = 0;
    if (isLocal) {
        std::fseek(f, 0, SEEK_END);
        fileLength = std::ftell(f);
        std::rewind(f);
    } else {
        std::fseek(f, 128L + static_cast<long>(entryPointCount) * 112L, SEEK_SET);
    }

    for (uint16_t i = 0; i < tagCount; ++i) {
        uint16_t identifier = readBE16(f);
        (void)identifier;
        int c = fgetc(f);
        if (c == EOF) break;
        uint8_t flagsByte = static_cast<uint8_t>(c);
        (void)flagsByte;
        c = fgetc(f);
        if (c == EOF) break;
        uint8_t typeByte = static_cast<uint8_t>(c);
        (void)typeByte;
        std::string tagName = readFixedString(f, 32);
        (void)tagName;
        uint32_t groupTag = readBE32(f);
        uint32_t subgroupTag = readBE32(f);
        uint32_t offset = readBE32(f);
        (void)offset;
        uint32_t tagSize = readBE32(f);
        (void)tagSize;
        uint32_t userData = readBE32(f);
        (void)userData;
        uint16_t tagVersion = readBE16(f);
        (void)tagVersion;
        c = fgetc(f);
        if (c == EOF) break;
        c = fgetc(f);
        if (c == EOF) break;
        uint32_t tagSignature = readBE32(f);
        (void)tagSignature;

        GuiTagEntry entry;
        entry.groupTag = groupTag;
        entry.subgroupTag = subgroupTag;
        entry.offset = offset;
        entry.size = tagSize;
        entry.sourceFile = filePath;
        entry.isLocal = isLocal;
        if (isLocal) {
            long payloadSize = (fileLength > 64L) ? (fileLength - 64L) : 0L;
            entry.offset = 64;
            entry.size = static_cast<uint32_t>(payloadSize);
        }
        outTags.push_back(entry);
    }

    closeFile();
    return true;
}

struct AppState {
    fs::path exeDir;
    fs::path scriptDir;
    fs::path toolDir;
    std::array<char, 512> tagsSource{};
    std::array<char, 64> meshTag{};
    std::array<char, 128> meshTagFilter{};
    std::array<char, 512> outputFolder{};
    std::array<char, 512> pluginOutput{};
    std::array<char, 512> blenderPath{};
    bool overwrite = true;
    bool writeOra = false;
    bool exportNoAnimationSnapshots = false;
    bool editOnBuild = true;
    bool autoAppendMeshTag = true;
    bool followLog = true;
    bool lockDockLayout = true;
    int  windowX = INT_MIN;       // INT_MIN = "no saved position; let the OS place the window"
    int  windowY = INT_MIN;
    int  windowW = 1440;
    int  windowH = 900;
    bool windowMaximized = false;
    std::vector<MeshChoice> availableMeshTags;
    std::string lastScannedTagsSource;
    std::string mapScanStatus;
    std::string logSaveStatus;
    std::string settingsStatus;
    std::string actionsStatus;
    std::array<char, 128> actionFilter{};
    std::array<char, 128> logFilter{};
    int selectedActionIndex = -1;
    enum PendingDiscard { PENDING_NONE = 0, PENDING_RELOAD = 1, PENDING_QUIT = 2, PENDING_RUN = 3 };
    PendingDiscard pendingDiscard = PENDING_NONE;
    bool openDiscardPopup = false;
    std::string pendingRunCommand;
    std::string pendingRunLabel;
    bool actionsLoaded = false;
    bool actionsDirty = false;
    bool actionsStructureDirty = false;
    uint64_t actionsBaselineFingerprint = 0;
    std::vector<uint64_t> actionBaselineFingerprints;
    fs::path mapPreviewPath;
    fs::path mapPreviewUnitsPath;
    unsigned int mapPreviewTextureId = 0;
    int mapPreviewWidth = 0;
    int mapPreviewHeight = 0;
    bool mapPreviewNeedsReload = true;
    std::string mapPreviewStatus;
    float mapPreviewZoom = 1.0f;
    ImVec2 mapPreviewPan = ImVec2(0.0f, 0.0f);
    std::vector<MapUnitMarker> mapPreviewUnitMarkers;
    std::set<uint16_t> selectedMapUnitIdentifiers;
    bool mapPreviewDragSelecting = false;
    ImVec2 mapPreviewDragStart = ImVec2(0.0f, 0.0f);
    ImVec2 mapPreviewDragCurrent = ImVec2(0.0f, 0.0f);
    std::optional<MapPreviewPickTarget> mapPreviewPickTarget;
    std::set<int> dirtyActionIndices;
    json actionsDoc;
    ShellRunner runner;
};

static std::string selectedMeshChoiceDisplay(const AppState& state) {
    std::string currentTag = state.meshTag.data();
    for (const MeshChoice& choice : state.availableMeshTags) {
        if (choice.tag == currentTag) return meshChoiceDisplay(choice);
    }
    return currentTag.empty() ? std::string("(choose map)") : currentTag;
}

static uint64_t hashBytesFNV1a64(const char* data, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t hashJsonValue(const json& value) {
    const std::string dumped = value.dump();
    return hashBytesFNV1a64(dumped.data(), dumped.size());
}

static int countDockedWindowsRecursive(const ImGuiDockNode* node) {
    if (node == nullptr) return 0;
    int total = node->Windows.Size;
    total += countDockedWindowsRecursive(node->ChildNodes[0]);
    total += countDockedWindowsRecursive(node->ChildNodes[1]);
    return total;
}

static uint16_t readLE16u(const uint8_t* b, size_t o) {
    return static_cast<uint16_t>(static_cast<uint16_t>(b[o]) |
                                 (static_cast<uint16_t>(b[o + 1]) << 8));
}

static uint32_t readLE32u(const uint8_t* b, size_t o) {
    return static_cast<uint32_t>(b[o]) |
           (static_cast<uint32_t>(b[o + 1]) << 8) |
           (static_cast<uint32_t>(b[o + 2]) << 16) |
           (static_cast<uint32_t>(b[o + 3]) << 24);
}

static int32_t readLE32s(const uint8_t* b, size_t o) {
    return static_cast<int32_t>(readLE32u(b, o));
}

static void captureActionsBaseline(AppState& state) {
    state.actionBaselineFingerprints.clear();
    state.actionsBaselineFingerprint = 0;
    if (!state.actionsLoaded || !state.actionsDoc.contains("actions") || !state.actionsDoc["actions"].is_array()) {
        return;
    }
    state.actionsBaselineFingerprint = hashJsonValue(state.actionsDoc);
    const json& actions = state.actionsDoc["actions"];
    state.actionBaselineFingerprints.reserve(actions.size());
    for (const json& action : actions) {
        state.actionBaselineFingerprints.push_back(hashJsonValue(action));
    }
}

static void refreshAllActionsDirtyState(AppState& state) {
    if (!state.actionsLoaded || !state.actionsDoc.contains("actions") || !state.actionsDoc["actions"].is_array()) {
        state.dirtyActionIndices.clear();
        state.actionsDirty = false;
        state.actionsStructureDirty = false;
        return;
    }

    const json& actions = state.actionsDoc["actions"];
    state.actionsDirty = hashJsonValue(state.actionsDoc) != state.actionsBaselineFingerprint;
    if (!state.actionsDirty) {
        state.dirtyActionIndices.clear();
        state.actionsStructureDirty = false;
    } else if (actions.size() != state.actionBaselineFingerprints.size()) {
        state.actionsStructureDirty = true;
    }
}

static bool isPrintableAscii(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return uc >= 32 && uc <= 126;
}

static std::optional<std::string> validateMeshTag(const std::string& meshTag) {
    if (meshTag.empty()) return std::string("Mesh Tag is required.");
    if (meshTag.size() != 4) return std::string("Mesh Tag must be exactly 4 characters.");
    for (char c : meshTag) {
        if (!isPrintableAscii(c)) return std::string("Mesh Tag contains non-printable characters.");
    }
    return std::nullopt;
}

static bool isWindowsReservedName(const std::string& nameUpper) {
    static const char* reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    for (const char* r : reserved) {
        if (nameUpper == r) return true;
    }
    return false;
}

static std::optional<std::string> validatePathText(const std::string& raw, const char* label, bool allowEmpty = false) {
    if (raw.empty()) {
        if (allowEmpty) return std::nullopt;
        return std::string(label) + " is required.";
    }

    for (char c : raw) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32) return std::string(label) + " contains control characters.";
    }

    fs::path path(raw);
    for (const fs::path& partPath : path) {
        std::string part = partPath.string();
        if (part.empty()) continue;
        if (part == "/" || part == "\\" || part == path.root_name().string()) continue;
        if (part == "." || part == "..") {
            return std::string(label) + " may not contain '.' or '..' path segments.";
        }

#if defined(_WIN32)
        for (char c : part) {
            if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
                return std::string(label) + " contains invalid Windows filename characters.";
            }
            if (c == ':') {
                return std::string(label) + " contains an unexpected ':' in a path segment.";
            }
        }
        if (!part.empty() && (part.back() == ' ' || part.back() == '.')) {
            return std::string(label) + " has a path segment ending in space or dot.";
        }
        std::string stem = fs::path(part).stem().string();
        std::string upper;
        upper.reserve(stem.size());
        for (char c : stem) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (isWindowsReservedName(upper)) {
            return std::string(label) + " uses a reserved Windows filename.";
        }
#endif
    }

    return std::nullopt;
}

static std::vector<std::string> collectWorkflowIssues(const AppState& state) {
    std::vector<std::string> issues;
    auto addIssue = [&](std::optional<std::string> issue) {
        if (issue) issues.push_back(*issue);
    };

    addIssue(validatePathText(state.tagsSource.data(), "Tags Source or Plugin File"));
    addIssue(validateMeshTag(state.meshTag.data()));
    addIssue(validatePathText(state.outputFolder.data(), "Extracted Map Folder"));
    addIssue(validatePathText(state.pluginOutput.data(), "Plugin Output File or Folder"));
    if (std::strlen(state.blenderPath.data()) > 0) {
        addIssue(validatePathText(state.blenderPath.data(), "Blender Executable", true));
    }
    return issues;
}

static std::string scriptName(const char* winName, const char* unixName) {
#if defined(_WIN32)
    return winName;
#else
    return unixName;
#endif
}

static bool hasRequiredLayout(const AppState& s) {
    std::error_code ec;
    const fs::path extractScript = s.scriptDir / scriptName("extract_assets.bat", "extract_assets.sh");
    const fs::path blendScript = s.scriptDir / scriptName("create_blend.bat", "create_blend.sh");
    const fs::path buildPluginExe = s.toolDir / scriptName("build_plugin.exe", "build_plugin");
    return fs::exists(extractScript, ec) &&
           fs::exists(blendScript, ec) &&
           fs::exists(buildPluginExe, ec);
}

static std::string makeTimestampedLogName() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &tt);
#else
    localtime_r(&tt, &localTm);
#endif
    char buf[64]{};
    std::strftime(buf, sizeof(buf), "myth2ools_log_%Y%m%d_%H%M%S.txt", &localTm);
    return std::string(buf);
}

static bool saveLogToFile(AppState& state, fs::path& savedPath) {
    std::error_code ec;
    fs::path logDir = state.exeDir / "logs";
    fs::create_directories(logDir, ec);
    if (ec) return false;

    savedPath = logDir / makeTimestampedLogName();
    std::ofstream out(savedPath, std::ios::binary);
    if (!out) return false;

    std::lock_guard<std::mutex> lock(state.runner.logMutex);
    for (const std::string& line : state.runner.logLines) {
        out << line << "\r\n";
    }
    return out.good();
}

static fs::path settingsPath(const AppState& state) {
    return state.exeDir / "settings.ini";
}

static fs::path blenderPathFile(const AppState& state) {
    return state.exeDir / "blender_path.txt";
}

static std::string escapeSetting(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

static std::string unescapeSetting(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '\\' && i + 1 < value.size()) {
            char n = value[++i];
            switch (n) {
            case '\\': out.push_back('\\'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default:
                out.push_back('\\');
                out.push_back(n);
                break;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static bool saveSettings(AppState& state) {
    std::ofstream out(settingsPath(state), std::ios::binary);
    if (!out) return false;
    out << "tagsSource=" << escapeSetting(state.tagsSource.data()) << "\n";
    out << "meshTag=" << escapeSetting(state.meshTag.data()) << "\n";
    out << "outputFolder=" << escapeSetting(state.outputFolder.data()) << "\n";
    out << "pluginOutput=" << escapeSetting(state.pluginOutput.data()) << "\n";
    out << "blenderPath=" << escapeSetting(state.blenderPath.data()) << "\n";
    out << "overwrite=" << (state.overwrite ? "1" : "0") << "\n";
    out << "writeOra=" << (state.writeOra ? "1" : "0") << "\n";
    out << "exportNoAnimationSnapshots=" << (state.exportNoAnimationSnapshots ? "1" : "0") << "\n";
    out << "editOnBuild=" << (state.editOnBuild ? "1" : "0") << "\n";
    out << "autoAppendMeshTag=" << (state.autoAppendMeshTag ? "1" : "0") << "\n";
    out << "followLog=" << (state.followLog ? "1" : "0") << "\n";
    out << "lockDockLayout=" << (state.lockDockLayout ? "1" : "0") << "\n";
    out << "windowX=" << state.windowX << "\n";
    out << "windowY=" << state.windowY << "\n";
    out << "windowW=" << state.windowW << "\n";
    out << "windowH=" << state.windowH << "\n";
    out << "windowMaximized=" << (state.windowMaximized ? "1" : "0") << "\n";
    if (!out.good()) return false;

    const std::string blender = state.blenderPath.data();
    std::error_code ec;
    if (blender.empty()) {
        fs::remove(blenderPathFile(state), ec);
        return true;
    }

    std::ofstream blenderOut(blenderPathFile(state), std::ios::binary);
    if (!blenderOut) return false;
    blenderOut << blender << "\n";
    return blenderOut.good();
}

static void loadSettings(AppState& state) {
    std::ifstream in(settingsPath(state), std::ios::binary);
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string value = unescapeSetting(line.substr(eq + 1));
        if (key == "tagsSource") copyToBuffer(state.tagsSource, value);
        else if (key == "meshTag") copyToBuffer(state.meshTag, value);
        else if (key == "outputFolder") copyToBuffer(state.outputFolder, value);
        else if (key == "pluginOutput") copyToBuffer(state.pluginOutput, value);
        else if (key == "blenderPath") copyToBuffer(state.blenderPath, value);
        else if (key == "overwrite") state.overwrite = (value == "1");
        else if (key == "writeOra") state.writeOra = (value == "1");
        else if (key == "exportNoAnimationSnapshots") state.exportNoAnimationSnapshots = (value == "1");
        else if (key == "editOnBuild") state.editOnBuild = (value == "1");
        else if (key == "autoAppendMeshTag") state.autoAppendMeshTag = (value == "1");
        else if (key == "followLog") state.followLog = (value == "1");
        else if (key == "lockDockLayout") state.lockDockLayout = (value == "1");
        else if (key == "windowX") { try { state.windowX = std::stoi(value); } catch (...) {} }
        else if (key == "windowY") { try { state.windowY = std::stoi(value); } catch (...) {} }
        else if (key == "windowW") { try { int v = std::stoi(value); if (v > 100) state.windowW = v; } catch (...) {} }
        else if (key == "windowH") { try { int v = std::stoi(value); if (v > 100) state.windowH = v; } catch (...) {} }
        else if (key == "windowMaximized") state.windowMaximized = (value == "1");
    }

    if (std::string(state.blenderPath.data()).empty()) {
        std::ifstream blenderIn(blenderPathFile(state), std::ios::binary);
        if (blenderIn) {
            std::string blender;
            std::getline(blenderIn, blender);
            copyToBuffer(state.blenderPath, blender);
        }
    }
}

static void discoverPaths(AppState& s) {
    s.scriptDir = s.exeDir / "scripts";
    s.toolDir = s.exeDir / "bin";
}

static std::string buildExtractAssetsCommand(const AppState& s) {
    fs::path repoDir = s.exeDir.parent_path();
    fs::path script = s.scriptDir / scriptName("extract_assets.bat", "extract_assets.sh");
    fs::path tagsSource = resolveUserPath(repoDir, s.tagsSource.data());
    fs::path outFolder = resolveUserPath(repoDir, s.outputFolder.data());
    std::string command = quoteArg(script) + " " + quoteArg(tagsSource) + " " + quoteArg(s.meshTag.data()) + " " + quoteArg(outFolder);
    if (s.writeOra) command += " --ora";
    if (s.overwrite) command += " --overwrite";
    if (s.exportNoAnimationSnapshots) command += " --animation-frame none";
    return wrapWindowsCommand(command);
}

static std::string buildPluginCommand(const AppState& s) {
    fs::path repoDir = s.exeDir.parent_path();
    fs::path exe = s.toolDir / scriptName("build_plugin.exe", "build_plugin");
    fs::path outFolder = resolveUserPath(repoDir, s.outputFolder.data());
    fs::path pluginOutput = resolveUserPath(repoDir, s.pluginOutput.data());
    std::error_code ec;
    if (fs::exists(pluginOutput, ec) && fs::is_directory(pluginOutput, ec)) {
        pluginOutput /= std::string(s.meshTag.data()) + "_plugin";
    }
    std::string command = quoteArg(exe) + " " + quoteArg(outFolder) + " " + quoteArg(pluginOutput);
    if (s.editOnBuild) command += " --edit";
    return wrapWindowsCommand(command);
}

static std::string buildCreateBlendCommand(const AppState& s) {
    fs::path repoDir = s.exeDir.parent_path();
    fs::path script = s.scriptDir / scriptName("create_blend.bat", "create_blend.sh");
    fs::path outFolder = resolveUserPath(repoDir, s.outputFolder.data());
    std::string command = quoteArg(script) + " " + quoteArg(outFolder);
    return wrapWindowsCommand(command);
}

static fs::path actionsJsonPath(const AppState& state) {
    fs::path repoDir = state.exeDir.parent_path();
    fs::path outFolder = resolveUserPath(repoDir, state.outputFolder.data());
    return outFolder / "assets" / "actions" / "actions.json";
}

static fs::path terrainPreviewPath(const AppState& state) {
    fs::path repoDir = state.exeDir.parent_path();
    fs::path outFolder = resolveUserPath(repoDir, state.outputFolder.data());
    return outFolder / "terrain" / "terrain.bmp";
}

static fs::path unitsPreviewPath(const AppState& state) {
    fs::path repoDir = state.exeDir.parent_path();
    fs::path outFolder = resolveUserPath(repoDir, state.outputFolder.data());
    return outFolder / "assets" / "sprites" / "units.json";
}

static std::string jsonStringOrEmpty(const json& j, const char* key);
static int jsonIntOrDefault(const json& j, const char* key, int fallback);

static void releaseMapPreviewTexture(AppState& state) {
    if (state.mapPreviewTextureId != 0) {
        glDeleteTextures(1, &state.mapPreviewTextureId);
        state.mapPreviewTextureId = 0;
    }
    state.mapPreviewWidth = 0;
    state.mapPreviewHeight = 0;
}

static bool decodeBmpToRgba(const fs::path& path, std::vector<uint8_t>& rgba, int& width, int& height, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Could not open preview image.";
        return false;
    }

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 54) {
        error = "Preview BMP is too small.";
        return false;
    }
    if (bytes[0] != 'B' || bytes[1] != 'M') {
        error = "Preview image is not a BMP file.";
        return false;
    }

    const uint32_t pixelOffset = readLE32u(bytes.data(), 10);
    const uint32_t dibSize = readLE32u(bytes.data(), 14);
    if (dibSize < 40 || bytes.size() < 14u + dibSize) {
        error = "Preview BMP uses an unsupported DIB header.";
        return false;
    }

    const int32_t bmpWidth = readLE32s(bytes.data(), 18);
    const int32_t bmpHeightSigned = readLE32s(bytes.data(), 22);
    const uint16_t planes = readLE16u(bytes.data(), 26);
    const uint16_t bpp = readLE16u(bytes.data(), 28);
    const uint32_t compression = readLE32u(bytes.data(), 30);
    const uint32_t colorsUsed = readLE32u(bytes.data(), 46);
    if (planes != 1) {
        error = "Preview BMP has invalid plane count.";
        return false;
    }
    if (bmpWidth <= 0 || bmpHeightSigned == 0) {
        error = "Preview BMP has invalid dimensions.";
        return false;
    }
    if (!(bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32) || compression != 0) {
        error = "Preview BMP format is unsupported.";
        return false;
    }

    width = bmpWidth;
    height = bmpHeightSigned < 0 ? -bmpHeightSigned : bmpHeightSigned;
    const bool topDown = bmpHeightSigned < 0;
    const size_t rowStride = ((static_cast<size_t>(width) * bpp + 31u) / 32u) * 4u;
    if (pixelOffset + rowStride * static_cast<size_t>(height) > bytes.size()) {
        error = "Preview BMP pixel data is truncated.";
        return false;
    }

    size_t paletteCount = 0;
    size_t paletteOffset = 14u + dibSize;
    if (bpp <= 8) {
        paletteCount = colorsUsed ? colorsUsed : (1u << bpp);
        if (paletteOffset + paletteCount * 4u > bytes.size() || paletteOffset > pixelOffset) {
            error = "Preview BMP palette is truncated.";
            return false;
        }
    }

    rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
    auto paletteColor = [&](size_t index) -> const uint8_t* {
        return bytes.data() + paletteOffset + index * 4u;
    };

    for (int y = 0; y < height; ++y) {
        const int srcY = topDown ? y : (height - 1 - y);
        const uint8_t* row = bytes.data() + pixelOffset + rowStride * static_cast<size_t>(srcY);
        for (int x = 0; x < width; ++x) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (bpp == 4) {
                const uint8_t packed = row[x / 2];
                const uint8_t index = static_cast<uint8_t>((x & 1) == 0 ? (packed >> 4) : (packed & 0x0F));
                if (index >= paletteCount) {
                    error = "Preview BMP palette index is out of range.";
                    return false;
                }
                const uint8_t* c = paletteColor(index);
                b = c[0]; g = c[1]; r = c[2];
            } else if (bpp == 8) {
                const uint8_t index = row[x];
                if (index >= paletteCount) {
                    error = "Preview BMP palette index is out of range.";
                    return false;
                }
                const uint8_t* c = paletteColor(index);
                b = c[0]; g = c[1]; r = c[2];
            } else if (bpp == 24) {
                const uint8_t* p = row + static_cast<size_t>(x) * 3u;
                b = p[0]; g = p[1]; r = p[2];
            } else {
                const uint8_t* p = row + static_cast<size_t>(x) * 4u;
                b = p[0]; g = p[1]; r = p[2]; a = p[3];
            }

            const size_t dst = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
            rgba[dst + 0] = r;
            rgba[dst + 1] = g;
            rgba[dst + 2] = b;
            rgba[dst + 3] = a;
        }
    }
    return true;
}

static void reloadMapPreviewUnitMarkers(AppState& state) {
    state.mapPreviewUnitMarkers.clear();

    std::error_code ec;
    if (state.mapPreviewUnitsPath.empty()) return;
    if (!fs::exists(state.mapPreviewUnitsPath, ec) || ec) return;

    std::ifstream in(state.mapPreviewUnitsPath, std::ios::binary);
    if (!in) return;

    try {
        json doc = json::parse(in);
        if (!doc.contains("units") || !doc["units"].is_array()) return;

        for (const json& unit : doc["units"]) {
            if (!unit.is_object()) continue;
            if (!unit.contains("tag") || !unit["tag"].is_string()) continue;
            if (!unit.contains("identifier") || !unit["identifier"].is_number()) continue;
            if (!unit.contains("x") || !unit["x"].is_number()) continue;
            if (!unit.contains("y") || !unit["y"].is_number()) continue;

            MapUnitMarker marker;
            marker.tag = unit["tag"].get<std::string>();
            marker.identifier = static_cast<uint16_t>(unit["identifier"].get<unsigned int>());
            marker.cellX = unit["x"].get<float>();
            marker.cellY = unit["y"].get<float>();
            state.mapPreviewUnitMarkers.push_back(marker);
        }
    } catch (...) {
        state.mapPreviewUnitMarkers.clear();
    }
}

static ImU32 previewMarkerColorForUnitTag(const std::string& tag) {
    if (tag == "ghol" || tag == "soul" || tag == "thmy") {
        return IM_COL32(224, 74, 58, 235);
    }
    if (tag == "hawk" || tag == "deer" || tag == "chic" || tag == "frog") {
        return IM_COL32(232, 194, 88, 235);
    }
    return IM_COL32(78, 148, 255, 235);
}

static int findActionIndexById(const json& actions, int actionId) {
    for (size_t i = 0; i < actions.size(); ++i) {
        if (!actions[i].is_object()) continue;
        if (jsonIntOrDefault(actions[i], "id", -1) == actionId) return static_cast<int>(i);
    }
    return -1;
}

static bool actionParameterLinksToUnitGroup(const std::string& name) {
    return name == "link" || name == "subj" || name == "obje" || name == "enem" || name == "frie";
}

static bool actionDirectlyReferencesUnitIdentifier(const json& action, uint16_t identifier) {
    if (!action.is_object()) return false;
    if (!action.contains("parameters") || !action["parameters"].is_array()) return false;

    for (const json& param : action["parameters"]) {
        if (!param.is_object()) continue;
        if (jsonStringOrEmpty(param, "type") != "monster_identifier") continue;
        if (!param.contains("values") || !param["values"].is_array()) continue;
        for (const json& value : param["values"]) {
            if (!value.is_number()) continue;
            const int current = value.get<int>();
            if (current == static_cast<int>(identifier)) return true;
        }
    }
    return false;
}

static void collectHighlightedUnitIdentifiersFromAction(const json& actions,
                                                        int actionIndex,
                                                        std::set<uint16_t>& identifiers,
                                                        std::set<int>& visitedActionIndices) {
    if (actionIndex < 0 || actionIndex >= static_cast<int>(actions.size())) return;
    if (!visitedActionIndices.insert(actionIndex).second) return;

    const json& action = actions[static_cast<size_t>(actionIndex)];
    if (!action.is_object()) return;
    if (!action.contains("parameters") || !action["parameters"].is_array()) return;

    for (const json& param : action["parameters"]) {
        if (!param.is_object()) continue;
        const std::string paramType = jsonStringOrEmpty(param, "type");
        const std::string paramName = jsonStringOrEmpty(param, "name");

        if (paramType == "monster_identifier" && param.contains("values") && param["values"].is_array()) {
            for (const json& value : param["values"]) {
                if (!value.is_number()) continue;
                const int identifier = value.get<int>();
                if (identifier < 0 || identifier > 0xFFFF) continue;
                identifiers.insert(static_cast<uint16_t>(identifier));
            }
            continue;
        }

        if (paramType == "action_identifier" &&
            actionParameterLinksToUnitGroup(paramName) &&
            param.contains("values") &&
            param["values"].is_array()) {
            for (const json& value : param["values"]) {
                if (!value.is_number()) continue;
                const int linkedActionId = value.get<int>();
                const int linkedActionIndex = findActionIndexById(actions, linkedActionId);
                if (linkedActionIndex >= 0) {
                    collectHighlightedUnitIdentifiersFromAction(actions,
                                                                linkedActionIndex,
                                                                identifiers,
                                                                visitedActionIndices);
                }
            }
        }
    }
}

static bool actionReferencesUnitIdentifierRecursive(const json& actions,
                                                    int actionIndex,
                                                    uint16_t identifier,
                                                    std::vector<int>& memo,
                                                    std::set<int>& activeStack) {
    if (actionIndex < 0 || actionIndex >= static_cast<int>(actions.size())) return false;
    if (memo[static_cast<size_t>(actionIndex)] != -1) {
        return memo[static_cast<size_t>(actionIndex)] == 1;
    }
    if (!activeStack.insert(actionIndex).second) return false;

    bool found = false;
    const json& action = actions[static_cast<size_t>(actionIndex)];
    if (actionDirectlyReferencesUnitIdentifier(action, identifier)) {
        found = true;
    } else if (action.is_object() && action.contains("parameters") && action["parameters"].is_array()) {
        for (const json& param : action["parameters"]) {
            if (!param.is_object()) continue;
            if (jsonStringOrEmpty(param, "type") != "action_identifier") continue;
            const std::string paramName = jsonStringOrEmpty(param, "name");
            if (!actionParameterLinksToUnitGroup(paramName)) continue;
            if (!param.contains("values") || !param["values"].is_array()) continue;

            for (const json& value : param["values"]) {
                if (!value.is_number()) continue;
                const int linkedActionIndex = findActionIndexById(actions, value.get<int>());
                if (linkedActionIndex >= 0 &&
                    actionReferencesUnitIdentifierRecursive(actions,
                                                            linkedActionIndex,
                                                            identifier,
                                                            memo,
                                                            activeStack)) {
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }

    activeStack.erase(actionIndex);
    memo[static_cast<size_t>(actionIndex)] = found ? 1 : 0;
    return found;
}

static std::set<int> relatedActionIndicesForUnitIdentifier(const AppState& state, uint16_t identifier) {
    std::set<int> matches;
    if (!state.actionsLoaded) return matches;
    if (!state.actionsDoc.contains("actions") || !state.actionsDoc["actions"].is_array()) return matches;

    const json& actions = state.actionsDoc["actions"];
    std::vector<int> memo(actions.size(), -1);
    for (size_t i = 0; i < actions.size(); ++i) {
        std::set<int> activeStack;
        if (actionReferencesUnitIdentifierRecursive(actions,
                                                    static_cast<int>(i),
                                                    identifier,
                                                    memo,
                                                    activeStack)) {
            matches.insert(static_cast<int>(i));
        }
    }
    return matches;
}

static std::set<int> relatedActionIndicesForSelectedMapUnit(const AppState& state) {
    std::set<int> matches;
    for (uint16_t identifier : state.selectedMapUnitIdentifiers) {
        const std::set<int> unitMatches = relatedActionIndicesForUnitIdentifier(state, identifier);
        matches.insert(unitMatches.begin(), unitMatches.end());
    }
    return matches;
}

static const MapUnitMarker* findMapUnitMarkerByIdentifier(const AppState& state, uint16_t identifier) {
    for (const MapUnitMarker& marker : state.mapPreviewUnitMarkers) {
        if (marker.identifier == identifier) return &marker;
    }
    return nullptr;
}

static std::string selectedMapUnitLabel(const AppState& state) {
    if (state.selectedMapUnitIdentifiers.empty()) return "No units selected.";
    if (state.selectedMapUnitIdentifiers.size() == 1) {
        const uint16_t identifier = *state.selectedMapUnitIdentifiers.begin();
        const MapUnitMarker* selectedMarker = findMapUnitMarkerByIdentifier(state, identifier);
        if (selectedMarker != nullptr) {
            return selectedMarker->tag + " #" + std::to_string(selectedMarker->identifier);
        }
        return std::string("Unit #") + std::to_string(identifier);
    }
    return std::to_string(state.selectedMapUnitIdentifiers.size()) + " units selected";
}

static void clearMapUnitSelection(AppState& state) {
    state.selectedMapUnitIdentifiers.clear();
    state.mapPreviewDragSelecting = false;
    state.mapPreviewPickTarget.reset();
}

static void resetMapPreviewView(AppState& state) {
    state.mapPreviewZoom = 1.0f;
    state.mapPreviewPan = ImVec2(0.0f, 0.0f);
}

static void setSelectedActionIndex(AppState& state, int actionIndex) {
    state.selectedActionIndex = actionIndex;
    state.mapPreviewPickTarget.reset();
}

static bool selectReferencedAction(AppState& state, int actionId) {
    if (!state.actionsLoaded || !state.actionsDoc.contains("actions") || !state.actionsDoc["actions"].is_array()) {
        state.actionsStatus = "Actions are not loaded.";
        return false;
    }
    const int actionIndex = findActionIndexById(state.actionsDoc["actions"], actionId);
    if (actionIndex < 0) {
        state.actionsStatus = "Could not find referenced action #" + std::to_string(actionId) + ".";
        return false;
    }
    setSelectedActionIndex(state, actionIndex);
    state.actionsStatus = "Selected action #" + std::to_string(actionId) + ".";
    return true;
}

static bool selectReferencedMapUnit(AppState& state, int identifier) {
    if (identifier < 0 || identifier > 0xFFFF) {
        state.actionsStatus = "Referenced identifier is out of range.";
        return false;
    }
    const uint16_t unitIdentifier = static_cast<uint16_t>(identifier);
    if (findMapUnitMarkerByIdentifier(state, unitIdentifier) == nullptr) {
        state.actionsStatus = "Could not find preview marker #" + std::to_string(identifier) + ".";
        return false;
    }
    state.selectedMapUnitIdentifiers.clear();
    state.selectedMapUnitIdentifiers.insert(unitIdentifier);
    state.actionsStatus = "Selected unit #" + std::to_string(identifier) + ".";
    return true;
}

static bool selectAllReferencedMapUnits(AppState& state, const json& values) {
    if (!values.is_array()) {
        state.actionsStatus = "This parameter does not contain a unit list.";
        return false;
    }

    std::set<uint16_t> selectedIdentifiers;
    for (const json& value : values) {
        if (!value.is_number_integer()) continue;
        const int identifier = value.get<int>();
        if (identifier < 0 || identifier > 0xFFFF) continue;
        const uint16_t unitIdentifier = static_cast<uint16_t>(identifier);
        if (findMapUnitMarkerByIdentifier(state, unitIdentifier) != nullptr) {
            selectedIdentifiers.insert(unitIdentifier);
        }
    }

    if (selectedIdentifiers.empty()) {
        state.actionsStatus = "Could not find any referenced preview markers for this parameter.";
        return false;
    }

    state.selectedMapUnitIdentifiers.swap(selectedIdentifiers);
    state.actionsStatus = "Selected " + std::to_string(state.selectedMapUnitIdentifiers.size()) + " referenced unit(s).";
    return true;
}

static bool canSelectParameterReference(int typeId) {
    switch (typeId) {
    case 2:
    case 3:
    case 15:
    case 16:
    case 17:
    case 19:
    case 20:
        return true;
    default:
        return false;
    }
}

static bool canFillParameterReferenceFromMap(int typeId) {
    switch (typeId) {
    case 2:
    case 15:
    case 16:
    case 17:
    case 19:
    case 20:
        return true;
    default:
        return false;
    }
}

static std::string actionReferenceDisplayLabel(const AppState& state, int actionId) {
    if (!state.actionsLoaded || !state.actionsDoc.contains("actions") || !state.actionsDoc["actions"].is_array()) {
        return std::string("#") + std::to_string(actionId);
    }
    const int actionIndex = findActionIndexById(state.actionsDoc["actions"], actionId);
    if (actionIndex < 0) {
        return std::string("#") + std::to_string(actionId) + " (missing)";
    }
    const json& action = state.actionsDoc["actions"][static_cast<size_t>(actionIndex)];
    const std::string name = jsonStringOrEmpty(action, "name");
    const std::string type = jsonStringOrEmpty(action, "type");
    const char* typeLabel = guiActionTypeLabel(type);
    std::string label = "#" + std::to_string(actionId) + "  ";
    label += name.empty() ? std::string("(unnamed)") : name;
    label += "  [";
    if (type.empty()) {
        label += "container";
    } else if (typeLabel != nullptr) {
        label += typeLabel;
    } else {
        label += type;
    }
    label += "]";
    return label;
}

static std::vector<int> selectedUnitIdsToAppend(const AppState& state, const json& values) {
    std::set<int> existingIds;
    if (values.is_array()) {
        for (const json& value : values) {
            if (value.is_number_integer()) existingIds.insert(value.get<int>());
        }
    }

    std::vector<int> toAppend;
    toAppend.reserve(state.selectedMapUnitIdentifiers.size());
    for (uint16_t identifier : state.selectedMapUnitIdentifiers) {
        const int id = static_cast<int>(identifier);
        if (existingIds.insert(id).second) {
            toAppend.push_back(id);
        }
    }
    return toAppend;
}

static bool selectParameterReference(AppState& state, int typeId, int value) {
    switch (typeId) {
    case 3:
        return selectReferencedAction(state, value);
    case 2:
    case 15:
    case 16:
    case 17:
    case 19:
    case 20:
        return selectReferencedMapUnit(state, value);
    default:
        state.actionsStatus = "Selection is not supported for this parameter type.";
        return false;
    }
}

static void markActionIndexDirty(AppState& state, int actionIndex) {
    if (!state.actionsLoaded || !state.actionsDoc.contains("actions") || !state.actionsDoc["actions"].is_array()) return;
    if (actionIndex < 0 || actionIndex >= static_cast<int>(state.actionsDoc["actions"].size())) return;

    const size_t index = static_cast<size_t>(actionIndex);
    const bool changed =
        index >= state.actionBaselineFingerprints.size() ||
        hashJsonValue(state.actionsDoc["actions"][index]) != state.actionBaselineFingerprints[index];
    if (changed) state.dirtyActionIndices.insert(actionIndex);
    else state.dirtyActionIndices.erase(actionIndex);

    state.actionsDirty = hashJsonValue(state.actionsDoc) != state.actionsBaselineFingerprint;
    if (!state.actionsDirty) {
        state.dirtyActionIndices.clear();
        state.actionsStructureDirty = false;
    } else if (state.actionsDoc["actions"].size() != state.actionBaselineFingerprints.size()) {
        state.actionsStructureDirty = true;
    }
}

static bool applyMapPickedUnitToTarget(AppState& state, uint16_t identifier) {
    if (!state.mapPreviewPickTarget.has_value()) return false;
    if (!state.actionsLoaded || !state.actionsDoc.contains("actions") || !state.actionsDoc["actions"].is_array()) return false;

    const MapPreviewPickTarget target = *state.mapPreviewPickTarget;
    if (target.actionIndex < 0 || target.actionIndex >= static_cast<int>(state.actionsDoc["actions"].size())) return false;
    json& action = state.actionsDoc["actions"][static_cast<size_t>(target.actionIndex)];
    if (!action.contains("parameters") || !action["parameters"].is_array()) return false;
    if (target.paramIndex < 0 || target.paramIndex >= static_cast<int>(action["parameters"].size())) return false;

    json& param = action["parameters"][static_cast<size_t>(target.paramIndex)];
    if (!param.contains("values") || !param["values"].is_array()) return false;
    if (target.valueIndex < 0 || target.valueIndex >= static_cast<int>(param["values"].size())) return false;
    if (!canFillParameterReferenceFromMap(target.typeId)) return false;

    param["values"][static_cast<size_t>(target.valueIndex)] = static_cast<int>(identifier);
    markActionIndexDirty(state, target.actionIndex);
    state.actionsStatus = "Filled parameter with unit #" + std::to_string(identifier) + ".";
    return true;
}

static void reloadMapPreviewTexture(AppState& state) {
    releaseMapPreviewTexture(state);
    state.mapPreviewUnitMarkers.clear();
    state.mapPreviewStatus.clear();

    std::error_code ec;
    if (state.mapPreviewPath.empty()) {
        state.mapPreviewStatus = "Choose an output folder to preview its terrain image.";
        state.mapPreviewNeedsReload = false;
        return;
    }
    if (!fs::exists(state.mapPreviewPath, ec) || ec) {
        state.mapPreviewStatus = "No terrain preview found at " + state.mapPreviewPath.string();
        state.mapPreviewNeedsReload = false;
        return;
    }

    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    std::string error;
    if (!decodeBmpToRgba(state.mapPreviewPath, rgba, width, height, error)) {
        state.mapPreviewStatus = error + " Path: " + state.mapPreviewPath.string();
        state.mapPreviewNeedsReload = false;
        return;
    }

    glGenTextures(1, &state.mapPreviewTextureId);
    glBindTexture(GL_TEXTURE_2D, state.mapPreviewTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    state.mapPreviewWidth = width;
    state.mapPreviewHeight = height;
    reloadMapPreviewUnitMarkers(state);
    state.mapPreviewNeedsReload = false;
}

static void syncMapPreviewSource(AppState& state) {
    const fs::path desiredPath = terrainPreviewPath(state);
    const fs::path desiredUnitsPath = unitsPreviewPath(state);
    if (desiredPath != state.mapPreviewPath || desiredUnitsPath != state.mapPreviewUnitsPath) {
        state.mapPreviewPath = desiredPath;
        state.mapPreviewUnitsPath = desiredUnitsPath;
        state.mapPreviewNeedsReload = true;
        state.mapPreviewStatus.clear();
        state.mapPreviewUnitMarkers.clear();
        state.selectedMapUnitIdentifiers.clear();
        state.mapPreviewDragSelecting = false;
        resetMapPreviewView(state);
        releaseMapPreviewTexture(state);
    }
}

static std::string normalizeLineEndingsForPlatform(const std::string& text) {
#if defined(_WIN32)
    std::string normalized;
    normalized.reserve(text.size() + text.size() / 32);
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            normalized += "\r\n";
        } else {
            normalized.push_back(c);
        }
    }
    return normalized;
#else
    return text;
#endif
}

static bool loadActionsDoc(AppState& state) {
    fs::path path = actionsJsonPath(state);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        setActionsStatus(state, "Could not open " + path.string());
        state.actionsLoaded = false;
        state.actionsDirty = false;
        state.actionsStructureDirty = false;
        state.actionsBaselineFingerprint = 0;
        state.actionBaselineFingerprints.clear();
        state.dirtyActionIndices.clear();
        setSelectedActionIndex(state, -1);
        state.actionsDoc = json{};
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        state.actionsDoc = json::parse(text);
        if (!state.actionsDoc.contains("actions") || !state.actionsDoc["actions"].is_array()) {
            setActionsStatus(state, "actions.json is missing an actions array.");
            state.actionsLoaded = false;
            state.actionsDirty = false;
            state.actionsStructureDirty = false;
            state.actionsBaselineFingerprint = 0;
            state.actionBaselineFingerprints.clear();
            state.dirtyActionIndices.clear();
            setSelectedActionIndex(state, -1);
            state.actionsDoc = json{};
            return false;
        }
    } catch (const std::exception& e) {
        setActionsStatus(state, std::string("Failed to parse actions.json: ") + e.what());
        state.actionsLoaded = false;
        state.actionsDirty = false;
        state.actionsStructureDirty = false;
        state.actionsBaselineFingerprint = 0;
        state.actionBaselineFingerprints.clear();
        state.dirtyActionIndices.clear();
        setSelectedActionIndex(state, -1);
        state.actionsDoc = json{};
        return false;
    }

    state.actionsLoaded = true;
    state.actionsDirty = false;
    state.actionsStructureDirty = false;
    setSelectedActionIndex(state, state.actionsDoc["actions"].empty() ? -1 : 0);
    captureActionsBaseline(state);
    refreshAllActionsDirtyState(state);
    setActionsStatus(state, "Loaded " + std::to_string(state.actionsDoc["actions"].size()) + " actions.");
    return true;
}

static bool saveActionsDoc(AppState& state) {
    if (!state.actionsLoaded) return false;
    std::vector<int> dirtyIndices(state.dirtyActionIndices.begin(), state.dirtyActionIndices.end());
    for (int dirtyIndex : dirtyIndices) {
        if (dirtyIndex < 0 || dirtyIndex >= static_cast<int>(state.actionsDoc["actions"].size())) continue;
        logActionSummary(state, state.actionsDoc["actions"][static_cast<size_t>(dirtyIndex)], "before-save");
    }
    std::string normalizeError;
    if (!normalizeActionsDocForSave(state.actionsDoc, state.dirtyActionIndices, normalizeError)) {
        setActionsStatus(state, "Could not rebuild action parameter data: " + normalizeError);
        return false;
    }
    if (state.actionsStructureDirty) {
        std::string layoutError;
        if (!recomputeActionDocLayout(state.actionsDoc, layoutError)) {
            setActionsStatus(state, "Could not rebuild action layout: " + layoutError);
            return false;
        }
    }
    for (int dirtyIndex : dirtyIndices) {
        if (dirtyIndex < 0 || dirtyIndex >= static_cast<int>(state.actionsDoc["actions"].size())) continue;
        logActionSummary(state, state.actionsDoc["actions"][static_cast<size_t>(dirtyIndex)], "after-normalize");
    }
    fs::path path = actionsJsonPath(state);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        setActionsStatus(state, "Could not write " + path.string());
        return false;
    }
    std::string serialized = state.actionsDoc.dump(2) + "\n";
    serialized = normalizeLineEndingsForPlatform(serialized);
    out << serialized;
    if (!out.good()) {
        setActionsStatus(state, "Failed while writing " + path.string());
        return false;
    }
    captureActionsBaseline(state);
    refreshAllActionsDirtyState(state);
    setActionsStatus(state, "Saved " + path.string());
    for (int dirtyIndex : dirtyIndices) {
        if (dirtyIndex < 0 || dirtyIndex >= static_cast<int>(state.actionsDoc["actions"].size())) continue;
        logActionSummary(state, state.actionsDoc["actions"][static_cast<size_t>(dirtyIndex)], "saved");
    }
    return true;
}

static std::string jsonStringOrEmpty(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string()) return "";
    return j[key].get<std::string>();
}

static int jsonIntOrDefault(const json& j, const char* key, int fallback = 0) {
    if (!j.contains(key) || !j[key].is_number_integer()) return fallback;
    return j[key].get<int>();
}

static double jsonDoubleOrDefault(const json& j, const char* key, double fallback = 0.0) {
    if (!j.contains(key) || !j[key].is_number()) return fallback;
    return j[key].get<double>();
}

enum ActionParamType {
    GUI_PARAM_FLAG = 0,
    GUI_PARAM_STRING = 1,
    GUI_PARAM_MONSTER_IDENTIFIER = 2,
    GUI_PARAM_ACTION_IDENTIFIER = 3,
    GUI_PARAM_ANGLE = 4,
    GUI_PARAM_INTEGER = 5,
    GUI_PARAM_WORLD_DISTANCE = 6,
    GUI_PARAM_FIELD_NAME = 7,
    GUI_PARAM_FIXED = 8,
    GUI_PARAM_PROJECTILE = 9,
    GUI_PARAM_STRING_LIST = 10,
    GUI_PARAM_SOUND = 11,
    GUI_PARAM_PROJECTILE_OR_WORLD_POINT_2D = 12,
    GUI_PARAM_WORLD_POINT_2D = 13,
    GUI_PARAM_WORLD_RECTANGLE_2D = 14,
    GUI_PARAM_OBJECT_IDENTIFIER = 15,
    GUI_PARAM_MODEL_IDENTIFIER = 16,
    GUI_PARAM_SOUND_SOURCE_IDENTIFIER = 17,
    GUI_PARAM_WORLD_POINT_3D = 18,
    GUI_PARAM_LOCAL_PROJECTILE_GROUP_IDENTIFIER = 19,
    GUI_PARAM_MODEL_ANIMATION_IDENTIFIER = 20
};

struct GuiParamTypeOption {
    int id;
    const char* typeName;
    const char* displayLabel;
};

static const GuiParamTypeOption kGuiParamTypeOptions[] = {
    {GUI_PARAM_FLAG, "flag", "flag (0)"},
    {GUI_PARAM_STRING, "string", "string (1)"},
    {GUI_PARAM_MONSTER_IDENTIFIER, "monster_identifier", "monster_identifier (2)"},
    {GUI_PARAM_ACTION_IDENTIFIER, "action_identifier", "action_identifier (3)"},
    {GUI_PARAM_ANGLE, "angle", "angle (4)"},
    {GUI_PARAM_INTEGER, "integer", "integer (5)"},
    {GUI_PARAM_WORLD_DISTANCE, "world_distance", "world_distance (6)"},
    {GUI_PARAM_FIELD_NAME, "field_name", "field_name (7)"},
    {GUI_PARAM_FIXED, "fixed", "fixed (8)"},
    {GUI_PARAM_PROJECTILE, "projectile", "projectile (9, prgr)"},
    {GUI_PARAM_STRING_LIST, "string_list", "string_list (10)"},
    {GUI_PARAM_SOUND, "sound", "sound (11)"},
    {GUI_PARAM_PROJECTILE_OR_WORLD_POINT_2D, "projectile", "projectile (12, proj)"},
    {GUI_PARAM_WORLD_POINT_2D, "world_point_2d", "world_point_2d (13)"},
    {GUI_PARAM_WORLD_RECTANGLE_2D, "world_rectangle_2d", "world_rectangle_2d (14)"},
    {GUI_PARAM_OBJECT_IDENTIFIER, "object_identifier", "object_identifier (15)"},
    {GUI_PARAM_MODEL_IDENTIFIER, "model_identifier", "model_identifier (16)"},
    {GUI_PARAM_SOUND_SOURCE_IDENTIFIER, "sound_source_identifier", "sound_source_identifier (17)"},
    {GUI_PARAM_WORLD_POINT_3D, "world_point_3d", "world_point_3d (18)"},
    {GUI_PARAM_LOCAL_PROJECTILE_GROUP_IDENTIFIER, "local_projectile_group_identifier", "local_projectile_group_identifier (19)"},
    {GUI_PARAM_MODEL_ANIMATION_IDENTIFIER, "model_animation_identifier", "model_animation_identifier (20)"}
};

static const GuiParamTypeOption* guiParamTypeOptionById(int typeId) {
    for (const GuiParamTypeOption& option : kGuiParamTypeOptions) {
        if (option.id == typeId) return &option;
    }
    return nullptr;
}

static constexpr double GUI_WORLD_POINT_SF = 512.0;
static constexpr double GUI_FIXED_SF = 65536.0;
static constexpr double GUI_ANGLE_SF = 65536.0 / 360.0;

// Display name for an action-type FourCC, derived from Loathing 1.8.4's UI
// labels (cross-referenced against the SSR Map Action Texts corpus — see
// Doc/action_corpus.md). Returns nullptr for unknown codes so callers can
// decide how to fall back.
static const char* guiActionTypeLabel(const std::string& fourcc) {
    if (fourcc.size() != 4) return nullptr;
    struct Entry { const char* code; const char* label; };
    static const Entry kEntries[] = {
        {"acli", "Action List"},
        {"ambi", "Ambient Sound Control"},
        {"anim", "Model Animation"},
        {"atta", "Attack"},
        {"ctrl", "Unit Control"},
        {"dela", "Delay"},
        {"endg", "Endgame Condition"},
        {"gene", "General Action"},
        {"geom", "Geometry Filter"},
        {"girl", "Harass"},
        {"lead", "Leading"},
        {"legi", "Legion"},
        {"ligh", "Lightning"},
        {"lpgr", "Local Projectile Group"},
        {"mean", "Meander"},
        {"mele", "Melee"},
        {"miss", "Mission"},
        {"moef", "Model Effect"},
        {"moma", "Move Marker"},
        {"move", "Movement"},
        {"mung", "Munger"},
        {"ngty", "Netgame Type"},
        {"obmo", "Observer Movement"},
        {"part", "Particle System Control"},
        {"pick", "Pick Up Object"},
        {"plat", "Platoon"},
        {"plmo", "Platoon Movement"},
        {"plsc", "Platoon Scouting"},
        {"rout", "Rout"},
        {"snif", "Sniffer"},
        {"soun", "Sound Action"},
        {"squa", "Squad"},
        {"suic", "Suicide"},
        {"surr", "Surround"},
        {"tuni", "Test Unit"},
        {"wand", "Wandering Movement"},
    };
    for (const Entry& e : kEntries) {
        if (fourcc[0] == e.code[0] && fourcc[1] == e.code[1] &&
            fourcc[2] == e.code[2] && fourcc[3] == e.code[3]) {
            return e.label;
        }
    }
    return nullptr;
}

static const char* guiExpirationModeName(int modeId) {
    switch (modeId) {
    case 0: return "trigger";
    case 1: return "execution";
    case 2: return "successful_execution";
    case 3: return "never";
    case 4: return "failed_execution";
    default: return "trigger";
    }
}

static int guiExpirationModeIdFromName(const std::string& mode) {
    if (mode == "trigger") return 0;
    if (mode == "execution") return 1;
    if (mode == "successful_execution") return 2;
    if (mode == "never") return 3;
    if (mode == "failed_execution") return 4;
    return 0;
}

static std::vector<std::string> guiActionFlagNames(uint32_t flags) {
    const char* names[] = {
        "initially_active",
        "activates_only_once",
        "no_initial_delay",
        "only_initial_delay",
        "deleted_on_deactivation"
    };
    std::vector<std::string> out;
    for (int i = 0; i < 5; ++i) {
        if (flags & (1u << i)) out.push_back(names[i]);
    }
    return out;
}

static void syncActionFlagsJson(json& action) {
    uint32_t flags = static_cast<uint32_t>(jsonIntOrDefault(action, "flags_raw"));
    std::vector<std::string> names = guiActionFlagNames(flags);
    action["flags"] = json::array();
    for (const std::string& name : names) action["flags"].push_back(name);
}

static size_t guiAlignTo(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

static void guiAppendBE16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

static void guiAppendBE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

static void guiAppendBE32s(std::vector<uint8_t>& out, int32_t value) {
    guiAppendBE32(out, static_cast<uint32_t>(value));
}

static void guiAppendFourCC(std::vector<uint8_t>& out, const std::string& raw) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(i < static_cast<int>(raw.size()) ? static_cast<uint8_t>(raw[static_cast<size_t>(i)]) : 0);
    }
}

static void guiAppendPadding(std::vector<uint8_t>& out, size_t align) {
    size_t padded = guiAlignTo(out.size(), align);
    while (out.size() < padded) out.push_back(0);
}

static std::string guiHexEncode(const std::vector<uint8_t>& bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(hex[(b >> 4) & 0xF]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

struct GuiParamRecordLayout {
    uint16_t typeId = 0;
    uint16_t count = 0;
    std::string name;
    std::vector<uint8_t> payload;
};

static int guiHexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool guiParseHexBytes(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    if ((s.size() % 2) != 0) return false;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = guiHexValue(s[i]);
        int lo = guiHexValue(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

static uint16_t guiReadBE16(const uint8_t* b, size_t o) {
    return static_cast<uint16_t>((static_cast<uint16_t>(b[o]) << 8) | static_cast<uint16_t>(b[o + 1]));
}

static std::string guiReadFourCC(const uint8_t* p) {
    if ((p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF) ||
        (p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x00)) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(p), 4);
}

static size_t guiActionParameterValueBytes(uint16_t type, uint16_t count) {
    switch (type) {
    case 1:
    case 0:
        return guiAlignTo(count, 4);
    case 7:
    case 9:
    case 10:
    case 11:
    case 12:
        return static_cast<size_t>(count) * 4;
    case 13:
        return static_cast<size_t>(count) * 8;
    case 18:
        return static_cast<size_t>(count) * 12;
    case 5:
    case 6:
    case 8:
        return static_cast<size_t>(count) * 4;
    default:
        return guiAlignTo(count, 2) * 2;
    }
}

static bool guiParseParameterLayout(const std::string& hex, std::vector<GuiParamRecordLayout>& layout, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!guiParseHexBytes(hex, bytes)) {
        error = "parameter_data_hex is not valid hex";
        return false;
    }
    layout.clear();
    size_t p = 0;
    while (p < bytes.size()) {
        if (p + 8 > bytes.size()) {
            error = "parameter layout has a truncated header";
            return false;
        }
        GuiParamRecordLayout rec;
        rec.typeId = guiReadBE16(bytes.data(), p + 0);
        rec.count = guiReadBE16(bytes.data(), p + 2);
        rec.name = guiReadFourCC(bytes.data() + p + 4);
        p += 8;
        size_t payloadBytes = guiActionParameterValueBytes(rec.typeId, rec.count);
        if (p + payloadBytes > bytes.size()) {
            error = "parameter layout has a truncated payload";
            return false;
        }
        rec.payload.assign(bytes.begin() + static_cast<ptrdiff_t>(p),
                           bytes.begin() + static_cast<ptrdiff_t>(p + payloadBytes));
        p += payloadBytes;
        layout.push_back(std::move(rec));
    }
    return true;
}

static bool guiJsonNumberArray(const json& values, std::vector<double>& out) {
    if (!values.is_array()) return false;
    out.clear();
    for (const json& item : values) {
        if (!item.is_number()) return false;
        out.push_back(item.get<double>());
    }
    return true;
}

static bool guiJsonIntArray(const json& values, std::vector<int>& out) {
    if (!values.is_array()) return false;
    out.clear();
    for (const json& item : values) {
        if (!item.is_number_integer()) return false;
        out.push_back(item.get<int>());
    }
    return true;
}

static bool guiJsonStringArray(const json& values, std::vector<std::string>& out) {
    if (!values.is_array()) return false;
    out.clear();
    for (const json& item : values) {
        if (!item.is_string()) return false;
        out.push_back(item.get<std::string>());
    }
    return true;
}

static bool guiAppendSerializedParameter(std::vector<uint8_t>& out, const std::string& paramName, uint16_t typeId, const json& values, std::string& error) {
    std::vector<uint8_t> payload;
    uint16_t count = 0;

    switch (typeId) {
    case GUI_PARAM_FLAG: {
        if (!values.is_boolean()) {
            error = "flag parameter '" + paramName + "' must be true/false";
            return false;
        }
        count = 1;
        payload.push_back(values.get<bool>() ? 1 : 0);
        while (payload.size() < 4) payload.push_back(0);
        break;
    }
    case GUI_PARAM_STRING: {
        if (!values.is_string()) {
            error = "string parameter '" + paramName + "' must be a string";
            return false;
        }
        std::string s = values.get<std::string>();
        if (s.size() > 0xFFFFu) {
            error = "string parameter '" + paramName + "' is too long";
            return false;
        }
        count = static_cast<uint16_t>(s.size());
        payload.insert(payload.end(), s.begin(), s.end());
        while (payload.size() < guiAlignTo(payload.size(), 4)) payload.push_back(0);
        break;
    }
    case GUI_PARAM_SOUND:
    case GUI_PARAM_FIELD_NAME:
    case GUI_PARAM_PROJECTILE:
    case GUI_PARAM_PROJECTILE_OR_WORLD_POINT_2D:
    case GUI_PARAM_STRING_LIST: {
        std::vector<std::string> vals;
        if (!guiJsonStringArray(values, vals)) {
            error = "FourCC parameter '" + paramName + "' must be a string array";
            return false;
        }
        count = static_cast<uint16_t>(vals.size());
        for (const std::string& s : vals) guiAppendFourCC(payload, s);
        break;
    }
    case GUI_PARAM_WORLD_POINT_2D: {
        if (!values.is_array()) {
            error = "world_point_2d parameter '" + paramName + "' must be an array";
            return false;
        }
        count = static_cast<uint16_t>(values.size());
        for (const json& point : values) {
            if (!point.is_object() || !point.contains("x") || !point.contains("y") ||
                !point["x"].is_number() || !point["y"].is_number()) {
                error = "world_point_2d parameter '" + paramName + "' has an invalid point";
                return false;
            }
            uint32_t x = static_cast<uint32_t>(std::lround(point["x"].get<double>() * GUI_WORLD_POINT_SF));
            uint32_t y = static_cast<uint32_t>(std::lround(point["y"].get<double>() * GUI_WORLD_POINT_SF));
            guiAppendBE32(payload, x);
            guiAppendBE32(payload, y);
        }
        break;
    }
    case GUI_PARAM_WORLD_POINT_3D: {
        if (!values.is_array()) {
            error = "world_point_3d parameter '" + paramName + "' must be an array";
            return false;
        }
        count = static_cast<uint16_t>(values.size());
        for (const json& point : values) {
            if (!point.is_object() || !point.contains("x") || !point.contains("y") || !point.contains("z") ||
                !point["x"].is_number() || !point["y"].is_number() || !point["z"].is_number()) {
                error = "world_point_3d parameter '" + paramName + "' has an invalid point";
                return false;
            }
            guiAppendBE32(payload, static_cast<uint32_t>(std::lround(point["x"].get<double>() * GUI_WORLD_POINT_SF)));
            guiAppendBE32(payload, static_cast<uint32_t>(std::lround(point["y"].get<double>() * GUI_WORLD_POINT_SF)));
            guiAppendBE32(payload, static_cast<uint32_t>(std::lround(point["z"].get<double>() * GUI_WORLD_POINT_SF)));
        }
        break;
    }
    case GUI_PARAM_FIXED:
    case GUI_PARAM_WORLD_DISTANCE: {
        std::vector<double> vals;
        if (!guiJsonNumberArray(values, vals)) {
            error = "numeric parameter '" + paramName + "' must be a number array";
            return false;
        }
        count = static_cast<uint16_t>(vals.size());
        double scale = (typeId == GUI_PARAM_FIXED) ? GUI_FIXED_SF : GUI_WORLD_POINT_SF;
        for (double value : vals) {
            guiAppendBE32s(payload, static_cast<int32_t>(std::lround(value * scale)));
        }
        break;
    }
    case GUI_PARAM_INTEGER: {
        std::vector<int> vals;
        if (!guiJsonIntArray(values, vals)) {
            error = "integer parameter '" + paramName + "' must be an integer array";
            return false;
        }
        count = static_cast<uint16_t>(vals.size());
        for (int value : vals) guiAppendBE32s(payload, value);
        break;
    }
    default: {
        std::vector<int> vals;
        if (!guiJsonIntArray(values, vals) && typeId != GUI_PARAM_ANGLE) {
            error = "parameter '" + paramName + "' uses an unsupported value shape";
            return false;
        }
        if (typeId == GUI_PARAM_ANGLE) {
            std::vector<double> angleVals;
            if (!guiJsonNumberArray(values, angleVals)) {
                error = "angle parameter '" + paramName + "' must be a number array";
                return false;
            }
            count = static_cast<uint16_t>(angleVals.size());
            for (double value : angleVals) {
                guiAppendBE16(payload, static_cast<uint16_t>(std::lround(value * GUI_ANGLE_SF)));
            }
            if ((count & 1u) != 0) guiAppendBE16(payload, 0);
        } else {
            count = static_cast<uint16_t>(vals.size());
            for (int value : vals) guiAppendBE16(payload, static_cast<uint16_t>(value));
            if ((count & 1u) != 0) guiAppendBE16(payload, 0);
        }
        break;
    }
    }

    guiAppendBE16(out, typeId);
    guiAppendBE16(out, count);
    guiAppendFourCC(out, paramName);
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

static bool guiBuildSerializedParameter(std::vector<uint8_t>& out,
                                        const std::string& paramName,
                                        uint16_t typeId,
                                        const json& values,
                                        std::string& error,
                                        bool stringUsesNullTerminator) {
    if (typeId == GUI_PARAM_STRING) {
        if (!values.is_string()) {
            error = "string parameter '" + paramName + "' must be a string";
            return false;
        }
        std::vector<uint8_t> payload;
        std::string s = values.get<std::string>();
        if (s.size() > 0xFFFEu) {
            error = "string parameter '" + paramName + "' is too long";
            return false;
        }
        uint16_t count = static_cast<uint16_t>(s.size() + (stringUsesNullTerminator ? 1 : 0));
        payload.insert(payload.end(), s.begin(), s.end());
        if (stringUsesNullTerminator) payload.push_back(0);
        while (payload.size() < guiAlignTo(payload.size(), 4)) payload.push_back(0);
        guiAppendBE16(out, typeId);
        guiAppendBE16(out, count);
        guiAppendFourCC(out, paramName);
        out.insert(out.end(), payload.begin(), payload.end());
        return true;
    }
    return guiAppendSerializedParameter(out, paramName, typeId, values, error);
}

static bool guiBuildSerializedParameterUsingLayout(std::vector<uint8_t>& out,
                                                   const GuiParamRecordLayout& layout,
                                                   const json& values,
                                                   std::string& error) {
    if (layout.typeId == GUI_PARAM_FLAG) {
        if (!values.is_boolean()) {
            error = "flag parameter '" + layout.name + "' must be true/false";
            return false;
        }
        bool enabled = values.get<bool>();
        if (layout.count == 0) {
            if (!enabled) {
                error = "flag parameter '" + layout.name + "' cannot be set false without changing record size";
                return false;
            }
            guiAppendBE16(out, layout.typeId);
            guiAppendBE16(out, 0);
            guiAppendFourCC(out, layout.name);
            return true;
        }
    }

    if (layout.typeId == GUI_PARAM_STRING) {
        if (!values.is_string()) {
            error = "string parameter '" + layout.name + "' must be a string";
            return false;
        }
        const std::string s = values.get<std::string>();
        const bool hadNullTerminator = layout.count > 0 && layout.payload.size() >= layout.count &&
                                       layout.payload[static_cast<size_t>(layout.count - 1)] == 0;
        const uint16_t desiredCount = static_cast<uint16_t>(s.size() + (hadNullTerminator ? 1 : 0));
        if (desiredCount != layout.count) {
            error = "string parameter '" + layout.name + "' cannot change encoded length yet";
            return false;
        }
        std::vector<uint8_t> payload;
        payload.insert(payload.end(), s.begin(), s.end());
        if (hadNullTerminator) payload.push_back(0);
        while (payload.size() < layout.payload.size()) payload.push_back(0);
        if (payload.size() != layout.payload.size()) {
            error = "string parameter '" + layout.name + "' would change payload size";
            return false;
        }
        guiAppendBE16(out, layout.typeId);
        guiAppendBE16(out, layout.count);
        guiAppendFourCC(out, layout.name);
        out.insert(out.end(), payload.begin(), payload.end());
        return true;
    }

    std::vector<uint8_t> payload = layout.payload;
    auto writeBE16At = [&](size_t offset, uint16_t value) {
        payload[offset + 0] = static_cast<uint8_t>((value >> 8) & 0xFF);
        payload[offset + 1] = static_cast<uint8_t>(value & 0xFF);
    };
    auto writeBE32At = [&](size_t offset, uint32_t value) {
        payload[offset + 0] = static_cast<uint8_t>((value >> 24) & 0xFF);
        payload[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
        payload[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        payload[offset + 3] = static_cast<uint8_t>(value & 0xFF);
    };

    switch (layout.typeId) {
    case GUI_PARAM_SOUND:
    case GUI_PARAM_FIELD_NAME:
    case GUI_PARAM_PROJECTILE:
    case GUI_PARAM_PROJECTILE_OR_WORLD_POINT_2D:
    case GUI_PARAM_STRING_LIST: {
        std::vector<std::string> vals;
        if (!guiJsonStringArray(values, vals)) {
            error = "FourCC parameter '" + layout.name + "' must be a string array";
            return false;
        }
        if (vals.size() != layout.count) {
            error = "parameter '" + layout.name + "' cannot change item count yet";
            return false;
        }
        for (size_t i = 0; i < vals.size(); ++i) {
            for (int j = 0; j < 4; ++j) {
                payload[i * 4 + static_cast<size_t>(j)] = (j < static_cast<int>(vals[i].size()))
                    ? static_cast<uint8_t>(vals[i][static_cast<size_t>(j)])
                    : 0;
            }
        }
        break;
    }
    case GUI_PARAM_WORLD_POINT_2D: {
        if (!values.is_array() || values.size() != layout.count) {
            error = "parameter '" + layout.name + "' cannot change point count yet";
            return false;
        }
        for (size_t i = 0; i < values.size(); ++i) {
            const json& point = values[i];
            if (!point.is_object() || !point.contains("x") || !point.contains("y") ||
                !point["x"].is_number() || !point["y"].is_number()) {
                error = "world_point_2d parameter '" + layout.name + "' has an invalid point";
                return false;
            }
            writeBE32At(i * 8 + 0, static_cast<uint32_t>(std::lround(point["x"].get<double>() * GUI_WORLD_POINT_SF)));
            writeBE32At(i * 8 + 4, static_cast<uint32_t>(std::lround(point["y"].get<double>() * GUI_WORLD_POINT_SF)));
        }
        break;
    }
    case GUI_PARAM_WORLD_POINT_3D: {
        if (!values.is_array() || values.size() != layout.count) {
            error = "parameter '" + layout.name + "' cannot change point count yet";
            return false;
        }
        for (size_t i = 0; i < values.size(); ++i) {
            const json& point = values[i];
            if (!point.is_object() || !point.contains("x") || !point.contains("y") || !point.contains("z") ||
                !point["x"].is_number() || !point["y"].is_number() || !point["z"].is_number()) {
                error = "world_point_3d parameter '" + layout.name + "' has an invalid point";
                return false;
            }
            writeBE32At(i * 12 + 0, static_cast<uint32_t>(std::lround(point["x"].get<double>() * GUI_WORLD_POINT_SF)));
            writeBE32At(i * 12 + 4, static_cast<uint32_t>(std::lround(point["y"].get<double>() * GUI_WORLD_POINT_SF)));
            writeBE32At(i * 12 + 8, static_cast<uint32_t>(std::lround(point["z"].get<double>() * GUI_WORLD_POINT_SF)));
        }
        break;
    }
    case GUI_PARAM_INTEGER: {
        std::vector<int> vals;
        if (!guiJsonIntArray(values, vals)) {
            error = "integer parameter '" + layout.name + "' must be an integer array";
            return false;
        }
        if (vals.size() != layout.count) {
            error = "parameter '" + layout.name + "' cannot change item count yet";
            return false;
        }
        for (size_t i = 0; i < vals.size(); ++i) {
            writeBE32At(i * 4, static_cast<uint32_t>(vals[i]));
        }
        break;
    }
    case GUI_PARAM_FIXED:
    case GUI_PARAM_WORLD_DISTANCE: {
        std::vector<double> vals;
        if (!guiJsonNumberArray(values, vals)) {
            error = "numeric parameter '" + layout.name + "' must be a number array";
            return false;
        }
        if (vals.size() != layout.count) {
            error = "parameter '" + layout.name + "' cannot change item count yet";
            return false;
        }
        double scale = (layout.typeId == GUI_PARAM_FIXED) ? GUI_FIXED_SF : GUI_WORLD_POINT_SF;
        for (size_t i = 0; i < vals.size(); ++i) {
            writeBE32At(i * 4, static_cast<uint32_t>(std::lround(vals[i] * scale)));
        }
        break;
    }
    case GUI_PARAM_ANGLE: {
        std::vector<double> vals;
        if (!guiJsonNumberArray(values, vals)) {
            error = "angle parameter '" + layout.name + "' must be a number array";
            return false;
        }
        if (vals.size() != layout.count) {
            error = "parameter '" + layout.name + "' cannot change item count yet";
            return false;
        }
        for (size_t i = 0; i < vals.size(); ++i) {
            writeBE16At(i * 2, static_cast<uint16_t>(std::lround(vals[i] * GUI_ANGLE_SF)));
        }
        break;
    }
    default: {
        std::vector<int> vals;
        if (!guiJsonIntArray(values, vals)) {
            error = "parameter '" + layout.name + "' uses an unsupported value shape";
            return false;
        }
        if (vals.size() != layout.count) {
            error = "parameter '" + layout.name + "' cannot change item count yet";
            return false;
        }
        for (size_t i = 0; i < vals.size(); ++i) {
            writeBE16At(i * 2, static_cast<uint16_t>(vals[i]));
        }
        break;
    }
    }

    guiAppendBE16(out, layout.typeId);
    guiAppendBE16(out, layout.count);
    guiAppendFourCC(out, layout.name);
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

static bool guiRepackActionParameters(json& action, std::string& error) {
    std::vector<GuiParamRecordLayout> layout;
    if (!guiParseParameterLayout(jsonStringOrEmpty(action, "parameter_data_hex"), layout, error)) return false;

    std::vector<bool> matched;
    if (action.contains("parameters") && action["parameters"].is_array()) {
        matched.assign(action["parameters"].size(), false);
    }

    std::vector<uint8_t> bytes;
    for (const GuiParamRecordLayout& rec : layout) {
        if (rec.name == "name") {
            if (!guiBuildSerializedParameter(bytes, "name", GUI_PARAM_STRING, json(jsonStringOrEmpty(action, "name")), error, true)) {
                return false;
            }
            continue;
        }

        bool found = false;
        if (action.contains("parameters") && action["parameters"].is_array()) {
            for (size_t i = 0; i < action["parameters"].size(); ++i) {
                json& param = action["parameters"][i];
                if (matched[i]) continue;
                if (jsonStringOrEmpty(param, "name") == rec.name &&
                    jsonIntOrDefault(param, "type_id", -1) == rec.typeId) {
                    if (!param.contains("values")) {
                        error = "parameter '" + rec.name + "' is missing values";
                        return false;
                    }
                    if (!guiBuildSerializedParameterUsingLayout(bytes, rec, param["values"], error)) {
                        return false;
                    }
                    matched[i] = true;
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            guiAppendBE16(bytes, rec.typeId);
            guiAppendBE16(bytes, rec.count);
            guiAppendFourCC(bytes, rec.name);
            bytes.insert(bytes.end(), rec.payload.begin(), rec.payload.end());
        }
    }

    if (action.contains("parameters") && action["parameters"].is_array()) {
        for (size_t i = 0; i < action["parameters"].size(); ++i) {
            if (matched[i]) continue;
            json& param = action["parameters"][i];
            std::string paramName = jsonStringOrEmpty(param, "name");
            int typeId = jsonIntOrDefault(param, "type_id", -1);
            if (paramName.size() != 4) {
                error = "parameter name '" + paramName + "' must be exactly four characters";
                return false;
            }
            if (typeId < 0 || typeId > 20) {
                error = "parameter '" + paramName + "' has unsupported type_id " + std::to_string(typeId);
                return false;
            }
            if (!param.contains("values")) {
                error = "parameter '" + paramName + "' is missing values";
                return false;
            }
            if (!guiBuildSerializedParameter(bytes, paramName, static_cast<uint16_t>(typeId), param["values"], error, true)) {
                return false;
            }
        }
    }

    action["parameter_data_hex"] = guiHexEncode(bytes);
    action["parameter_data_size"] = static_cast<int>(bytes.size());
    return true;
}

static bool normalizeActionsDocForSave(json& doc, const std::set<int>& dirtyActionIndices, std::string& error) {
    if (!doc.contains("actions") || !doc["actions"].is_array()) {
        error = "actions document is missing the actions array";
        return false;
    }

    for (int dirtyIndex : dirtyActionIndices) {
        if (dirtyIndex < 0 || dirtyIndex >= static_cast<int>(doc["actions"].size())) continue;
        json& action = doc["actions"][static_cast<size_t>(dirtyIndex)];
        int expirationModeId = jsonIntOrDefault(action, "expiration_mode_id", guiExpirationModeIdFromName(jsonStringOrEmpty(action, "expiration_mode")));
        action["expiration_mode_id"] = expirationModeId;
        action["expiration_mode"] = std::string(guiExpirationModeName(expirationModeId));
        syncActionFlagsJson(action);
        if (!guiRepackActionParameters(action, error)) return false;
    }
    return true;
}

static bool recomputeActionDocLayout(json& doc, std::string& error) {
    if (!doc.contains("actions") || !doc["actions"].is_array()) {
        error = "actions document is missing the actions array";
        return false;
    }

    constexpr int ACTION_HEAD_SIZE = 64;
    int runningOffset = 0;
    for (json& action : doc["actions"]) {
        std::string hex = jsonStringOrEmpty(action, "parameter_data_hex");
        std::vector<uint8_t> bytes;
        if (!guiParseHexBytes(hex, bytes)) {
            error = "action " + std::to_string(jsonIntOrDefault(action, "id")) + " has invalid parameter_data_hex";
            return false;
        }
        action["parameter_data_size"] = static_cast<int>(bytes.size());
        action["parameter_data_offset"] = runningOffset;
        runningOffset += static_cast<int>(bytes.size());
    }

    doc["action_count"] = static_cast<int>(doc["actions"].size());
    doc["action_buffer_size"] = static_cast<int>(doc["actions"].size()) * ACTION_HEAD_SIZE + runningOffset;
    return true;
}

static bool drawJsonIntListEditor(json& values, const char* addLabel, int defaultValue = 0) {
    if (!values.is_array()) values = json::array();
    bool changed = false;
    for (size_t i = 0; i < values.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        int value = values[i].is_number_integer() ? values[i].get<int>() : defaultValue;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputInt("##value", &value, 0, 0)) {
            values[i] = value;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Del")) {
            values.erase(values.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button(addLabel)) {
        values.push_back(defaultValue);
        changed = true;
    }
    return changed;
}

static bool drawJsonReferenceListEditor(AppState& state, int typeId, int paramIndex, json& values, const char* addLabel, int defaultValue = 0) {
    if (!values.is_array()) values = json::array();
    bool changed = false;
    const bool canSelect = canSelectParameterReference(typeId);
    const bool canFillFromMap = canFillParameterReferenceFromMap(typeId);
    const bool isActionReference = (typeId == GUI_PARAM_ACTION_IDENTIFIER);
    static std::array<char, 128> actionPickerFilter{};
    for (size_t i = 0; i < values.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        int value = values[i].is_number_integer() ? values[i].get<int>() : defaultValue;
        const bool pickActive =
            state.mapPreviewPickTarget.has_value() &&
            state.mapPreviewPickTarget->actionIndex == state.selectedActionIndex &&
            state.mapPreviewPickTarget->paramIndex == paramIndex &&
            state.mapPreviewPickTarget->valueIndex == static_cast<int>(i) &&
            state.mapPreviewPickTarget->typeId == typeId;
        if (isActionReference) {
            const std::string preview = actionReferenceDisplayLabel(state, value);
            ImGui::SetNextItemWidth(280.0f);
            if (ImGui::BeginCombo("##actionRef", preview.c_str())) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##actionRefFilter", "Filter actions by name, type, or id",
                                         actionPickerFilter.data(), actionPickerFilter.size());
                std::string filterLower = actionPickerFilter.data();
                for (char& c : filterLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                if (state.actionsLoaded && state.actionsDoc.contains("actions") && state.actionsDoc["actions"].is_array()) {
                    for (const json& action : state.actionsDoc["actions"]) {
                        const int actionId = jsonIntOrDefault(action, "id");
                        const std::string entry = actionReferenceDisplayLabel(state, actionId);
                        std::string haystack = entry;
                        for (char& c : haystack) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (!filterLower.empty() && haystack.find(filterLower) == std::string::npos) continue;
                        const bool selected = (value == actionId);
                        if (ImGui::Selectable(entry.c_str(), selected)) {
                            values[i] = actionId;
                            value = actionId;
                            changed = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::InputInt("##value", &value, 0, 0)) {
                values[i] = value;
                changed = true;
            }
        } else {
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputInt("##value", &value, 0, 0)) {
                values[i] = value;
                changed = true;
            }
        }
        ImGui::SameLine();
        if (!canSelect) ImGui::BeginDisabled();
        if (ImGui::Button("Select")) {
            selectParameterReference(state, typeId, value);
        }
        if (!canSelect) ImGui::EndDisabled();
        if (canFillFromMap) {
            ImGui::SameLine();
            if (ImGui::Button(pickActive ? "Cancel Pick" : "Pick")) {
                if (pickActive) {
                    state.mapPreviewPickTarget.reset();
                    state.actionsStatus = "Map pick cancelled.";
                } else {
                    state.mapPreviewPickTarget = MapPreviewPickTarget{
                        state.selectedActionIndex,
                        paramIndex,
                        static_cast<int>(i),
                        typeId
                    };
                    state.actionsStatus = "Click a unit on the map preview to fill this value.";
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Del")) {
            values.erase(values.begin() + static_cast<ptrdiff_t>(i));
            if (pickActive) state.mapPreviewPickTarget.reset();
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    std::string addButtonLabel = addLabel;
    std::vector<int> selectedIds;
    if (canFillFromMap) {
        selectedIds = selectedUnitIdsToAppend(state, values);
        if (state.selectedMapUnitIdentifiers.size() > 1) {
            addButtonLabel = "Add Values";
        } else if (state.selectedMapUnitIdentifiers.size() == 1) {
            addButtonLabel = "Add Value (Selected Unit)";
        }
    }

    const bool showSelectAll = (typeId == GUI_PARAM_MONSTER_IDENTIFIER);
    const bool disableAddSelectedUnits = canFillFromMap && !state.selectedMapUnitIdentifiers.empty() && selectedIds.empty();
    if (showSelectAll) {
        bool hasAnyReference = false;
        if (values.is_array()) {
            for (const json& value : values) {
                if (value.is_number_integer()) {
                    hasAnyReference = true;
                    break;
                }
            }
        }
        if (!hasAnyReference) ImGui::BeginDisabled();
        if (ImGui::Button("Select All")) {
            selectAllReferencedMapUnits(state, values);
        }
        if (!hasAnyReference) ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (disableAddSelectedUnits) ImGui::BeginDisabled();
    if (ImGui::Button(addButtonLabel.c_str())) {
        if (canFillFromMap && !state.selectedMapUnitIdentifiers.empty()) {
            for (int id : selectedIds) values.push_back(id);
        } else {
            values.push_back(defaultValue);
        }
        changed = true;
    }
    if (disableAddSelectedUnits) ImGui::EndDisabled();
    return changed;
}

static bool drawJsonDoubleListEditor(json& values, const char* addLabel, float defaultValue = 0.0f) {
    if (!values.is_array()) values = json::array();
    bool changed = false;
    for (size_t i = 0; i < values.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        float value = values[i].is_number() ? static_cast<float>(values[i].get<double>()) : defaultValue;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputFloat("##value", &value, 0.0f, 0.0f, "%.4f")) {
            values[i] = value;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("-")) {
            values.erase(values.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button(addLabel)) {
        values.push_back(defaultValue);
        changed = true;
    }
    return changed;
}

static bool drawJsonStringListEditor(json& values, const char* addLabel, size_t width = 80) {
    if (!values.is_array()) values = json::array();
    bool changed = false;
    for (size_t i = 0; i < values.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        std::array<char, 64> buf{};
        copyToBuffer(buf, values[i].is_string() ? values[i].get<std::string>() : "");
        ImGui::SetNextItemWidth(static_cast<float>(width));
        if (ImGui::InputText("##value", buf.data(), buf.size())) {
            values[i] = std::string(buf.data());
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("-")) {
            values.erase(values.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button(addLabel)) {
        values.push_back("");
        changed = true;
    }
    return changed;
}

static bool drawJsonPoint2ListEditor(json& values) {
    if (!values.is_array()) values = json::array();
    bool changed = false;
    for (size_t i = 0; i < values.size(); ++i) {
        if (!values[i].is_object()) values[i] = json{{"x", 0.0}, {"y", 0.0}};
        ImGui::PushID(static_cast<int>(i));
        float x = values[i].contains("x") && values[i]["x"].is_number() ? static_cast<float>(values[i]["x"].get<double>()) : 0.0f;
        float y = values[i].contains("y") && values[i]["y"].is_number() ? static_cast<float>(values[i]["y"].get<double>()) : 0.0f;
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputFloat("x", &x, 0.0f, 0.0f, "%.4f")) {
            values[i]["x"] = x;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputFloat("y", &y, 0.0f, 0.0f, "%.4f")) {
            values[i]["y"] = y;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("-")) {
            values.erase(values.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add Point")) {
        values.push_back(json{{"x", 0.0}, {"y", 0.0}});
        changed = true;
    }
    return changed;
}

static bool drawJsonPoint3ListEditor(json& values) {
    if (!values.is_array()) values = json::array();
    bool changed = false;
    for (size_t i = 0; i < values.size(); ++i) {
        if (!values[i].is_object()) values[i] = json{{"x", 0.0}, {"y", 0.0}, {"z", 0.0}};
        ImGui::PushID(static_cast<int>(i));
        float x = values[i].contains("x") && values[i]["x"].is_number() ? static_cast<float>(values[i]["x"].get<double>()) : 0.0f;
        float y = values[i].contains("y") && values[i]["y"].is_number() ? static_cast<float>(values[i]["y"].get<double>()) : 0.0f;
        float z = values[i].contains("z") && values[i]["z"].is_number() ? static_cast<float>(values[i]["z"].get<double>()) : 0.0f;
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputFloat("x", &x, 0.0f, 0.0f, "%.4f")) {
            values[i]["x"] = x;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputFloat("y", &y, 0.0f, 0.0f, "%.4f")) {
            values[i]["y"] = y;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputFloat("z", &z, 0.0f, 0.0f, "%.4f")) {
            values[i]["z"] = z;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("-")) {
            values.erase(values.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add Point")) {
        values.push_back(json{{"x", 0.0}, {"y", 0.0}, {"z", 0.0}});
        changed = true;
    }
    return changed;
}

static bool drawActionParameterEditor(AppState& state, json& param, size_t paramIndex) {
    (void)paramIndex;
    int typeId = jsonIntOrDefault(param, "type_id", -1);
    if (!param.contains("values")) param["values"] = json::array();

    bool changed = false;
    json& values = param["values"];
    const GuiParamTypeOption* selectedType = guiParamTypeOptionById(typeId);
    std::string previewLabel = selectedType != nullptr
        ? std::string(selectedType->displayLabel)
        : std::string("Unknown type (") + std::to_string(typeId) + ")";

    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("Parameter Type", previewLabel.c_str())) {
        for (const GuiParamTypeOption& option : kGuiParamTypeOptions) {
            const bool isSelected = (option.id == typeId);
            if (ImGui::Selectable(option.displayLabel, isSelected)) {
                typeId = option.id;
                param["type_id"] = option.id;
                param["type"] = option.typeName;
                changed = true;
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    switch (typeId) {
    case GUI_PARAM_FLAG: {
        bool value = values.is_boolean() ? values.get<bool>() : false;
        if (ImGui::Checkbox("Enabled", &value)) {
            values = value;
            changed = true;
        }
        break;
    }
    case GUI_PARAM_STRING: {
        std::array<char, 512> buf{};
        copyToBuffer(buf, values.is_string() ? values.get<std::string>() : "");
        if (ImGui::InputTextMultiline("Value", buf.data(), buf.size(), ImVec2(-FLT_MIN, 72.0f))) {
            values = std::string(buf.data());
            changed = true;
        }
        break;
    }
    case GUI_PARAM_SOUND:
    case GUI_PARAM_FIELD_NAME:
    case GUI_PARAM_PROJECTILE:
    case GUI_PARAM_PROJECTILE_OR_WORLD_POINT_2D:
        changed = drawJsonStringListEditor(values, "Add FourCC", 100);
        break;
    case GUI_PARAM_MONSTER_IDENTIFIER:
    case GUI_PARAM_ACTION_IDENTIFIER:
    case GUI_PARAM_OBJECT_IDENTIFIER:
    case GUI_PARAM_MODEL_IDENTIFIER:
    case GUI_PARAM_SOUND_SOURCE_IDENTIFIER:
    case GUI_PARAM_LOCAL_PROJECTILE_GROUP_IDENTIFIER:
    case GUI_PARAM_MODEL_ANIMATION_IDENTIFIER:
        changed = drawJsonReferenceListEditor(state, typeId, static_cast<int>(paramIndex), values, "Add Value");
        break;
    case GUI_PARAM_WORLD_POINT_2D:
        changed = drawJsonPoint2ListEditor(values);
        break;
    case GUI_PARAM_WORLD_POINT_3D:
        changed = drawJsonPoint3ListEditor(values);
        break;
    case GUI_PARAM_FIXED:
    case GUI_PARAM_WORLD_DISTANCE:
    case GUI_PARAM_ANGLE:
        changed = drawJsonDoubleListEditor(values, "Add Value");
        break;
    default:
        changed = drawJsonIntListEditor(values, "Add Value");
        break;
    }

    return changed;
}

static void markCurrentActionDirty(AppState& state) {
    markActionIndexDirty(state, state.selectedActionIndex);
}

static void remapDirtyIndicesAfterInsert(AppState& state, int insertIndex) {
    std::set<int> remapped;
    for (int idx : state.dirtyActionIndices) {
        remapped.insert(idx >= insertIndex ? idx + 1 : idx);
    }
    state.dirtyActionIndices.swap(remapped);
}

static void remapDirtyIndicesAfterDelete(AppState& state, int deleteIndex) {
    std::set<int> remapped;
    for (int idx : state.dirtyActionIndices) {
        if (idx == deleteIndex) continue;
        remapped.insert(idx > deleteIndex ? idx - 1 : idx);
    }
    state.dirtyActionIndices.swap(remapped);
}

static int nextAvailableActionId(const json& actions) {
    int maxId = 0;
    if (actions.is_array()) {
        for (const json& action : actions) {
            maxId = (std::max)(maxId, jsonIntOrDefault(action, "id"));
        }
    }
    return maxId + 1;
}

static json makeDefaultActionDoc(const json& actions) {
    json action = json::object();
    action["id"] = nextAvailableActionId(actions);
    action["type"] = "";
    action["name"] = "New Action";
    action["expiration_mode"] = "trigger";
    action["expiration_mode_id"] = 0;
    action["flags_raw"] = 0;
    action["flags"] = json::array();
    action["trigger_time_lower_bound_seconds"] = 0.0;
    action["trigger_time_delta_seconds"] = 0.0;
    action["indent"] = 0;
    action["parameter_data_offset"] = 0;
    action["parameter_data_size"] = 0;
    action["parameter_data_hex"] = "";
    action["parameters"] = json::array();
    return action;
}

static bool isKnownMeshTag(const AppState& s, const std::string& value) {
    if (value.size() != 4) return false;
    for (const MeshChoice& tag : s.availableMeshTags) {
        if (tag.tag == value) return true;
    }
    return false;
}

static void syncDerivedPaths(AppState& s) {
    std::string mesh = s.meshTag.data();
    std::string out = s.outputFolder.data();
    if (out.empty()) {
        copyToBuffer(s.outputFolder, (fs::path("out") / mesh).string());
    } else if (s.autoAppendMeshTag && !mesh.empty()) {
        fs::path outPath(out);
        std::string tail = outPath.filename().string();
        if (isKnownMeshTag(s, tail)) {
            outPath = outPath.parent_path() / mesh;
            copyToBuffer(s.outputFolder, outPath.string());
            out = s.outputFolder.data();
        }
    }

    std::string plugin = s.pluginOutput.data();
    std::string desiredPlugin = out.empty() ? (fs::path("out") / (mesh + "_plugin")).string()
                                            : (fs::path(out).string() + "_plugin");
    if (plugin.empty()) {
        copyToBuffer(s.pluginOutput, desiredPlugin);
    } else if (s.autoAppendMeshTag && !mesh.empty()) {
        fs::path pluginPath(plugin);
        std::string tail = pluginPath.filename().string();
        bool replaced = false;
        if (tail.size() > 7 && tail.substr(tail.size() - 7) == "_plugin") {
            std::string prefix = tail.substr(0, tail.size() - 7);
            if (isKnownMeshTag(s, prefix)) {
                pluginPath = pluginPath.parent_path() / (mesh + "_plugin");
                replaced = true;
            }
        } else if (isKnownMeshTag(s, tail)) {
            pluginPath = pluginPath.parent_path() / mesh;
            replaced = true;
        }
        if (replaced) copyToBuffer(s.pluginOutput, pluginPath.string());
    }
}

static void drawTopBar(AppState& state) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Myth2ools");
    ImGui::SameLine();
    ImGui::TextDisabled("Map extraction, plugin builds, and action editing");
    drawStatusChip(state.runner.running.load() ? "Runner Active" : "Runner Idle",
                   state.runner.running.load() ? ImVec4(0.18f, 0.54f, 0.35f, 1.0f)
                                               : ImVec4(0.26f, 0.30f, 0.36f, 1.0f));
    ImGui::SameLine();
    drawStatusChip(state.actionsDirty ? "Unsaved Actions" : "Actions Clean",
                   state.actionsDirty ? ImVec4(0.70f, 0.42f, 0.12f, 1.0f)
                                      : ImVec4(0.22f, 0.41f, 0.62f, 1.0f));
    ImGui::SameLine();
    std::string mapChip = std::string("Map ") + (std::strlen(state.meshTag.data()) ? state.meshTag.data() : "----");
    drawStatusChip(mapChip.c_str(), ImVec4(0.32f, 0.23f, 0.14f, 1.0f));
    ImGui::SameLine();
    bool lockLayout = state.lockDockLayout;
    if (ImGui::Checkbox("Lock Layout", &lockLayout)) {
        state.lockDockLayout = lockLayout;
        state.settingsStatus = saveSettings(state) ? "Settings saved." : "Failed to save settings.";
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Freeze the current dock arrangement while still allowing splitter resizing.");
    }
    ImGui::Separator();
}

static void applyLockedDockWindowClass(bool locked) {
    (void)locked;
}

static ImGuiWindowFlags lockedDockPanelFlags(bool locked) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (locked) flags |= ImGuiWindowFlags_NoMove;
    return flags;
}

template <size_t N>
static void labeledInputText(const char* label, const char* id, std::array<char, N>& buffer) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText(id, buffer.data(), buffer.size());
}

static void refreshAvailableMeshTags(AppState& state) {
    state.availableMeshTags.clear();
    state.mapScanStatus.clear();
    state.lastScannedTagsSource = state.tagsSource.data();

    fs::path repoDir = state.exeDir.parent_path();
    fs::path sourcePath = resolveUserPath(repoDir, state.tagsSource.data());
    std::error_code ec;
    if (sourcePath.empty()) {
        state.mapScanStatus = "Choose a tags folder or plugin file first.";
        return;
    }
    if (!fs::exists(sourcePath, ec)) {
        state.mapScanStatus = "Tags source not found.";
        return;
    }

    std::vector<GuiTagEntry> tags;
    size_t fileCount = 0;
    if (fs::is_regular_file(sourcePath, ec)) {
        fileCount = 1;
        collectTagsFromFile(sourcePath, tags);
    } else if (fs::is_directory(sourcePath, ec)) {
        for (fs::recursive_directory_iterator it(sourcePath, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            ++fileCount;
            collectTagsFromFile(it->path(), tags);
        }
    }

    std::vector<GuiTagEntry> meshEntries;
    std::vector<GuiTagEntry> stliEntries;
    for (const GuiTagEntry& entry : tags) {
        if (entry.groupTag == 0x6D657368u) meshEntries.push_back(entry);
        else if (entry.groupTag == 0x73746C69u) stliEntries.push_back(entry);
    }

    std::set<std::string> seenMeshTags;
    for (const GuiTagEntry& meshEntry : meshEntries) {
        std::string meshTag = tagToString(meshEntry.subgroupTag);
        if (!isPrintableTag(meshTag)) continue;
        if (!seenMeshTags.insert(meshTag).second) continue;

        MeshChoice choice;
        choice.tag = meshTag;
        choice.name = meshTag;
        lookupCampaignMapInfo(meshTag, choice.campaignLabel, choice.name);

        std::vector<uint8_t> meshData;
        if (readTagPayload(meshEntry, meshData) && meshData.size() >= 144) {
            uint32_t mapNameStli = (static_cast<uint32_t>(meshData[140]) << 24) |
                                   (static_cast<uint32_t>(meshData[141]) << 16) |
                                   (static_cast<uint32_t>(meshData[142]) << 8) |
                                   static_cast<uint32_t>(meshData[143]);
            if (mapNameStli != 0 && mapNameStli != 0xFFFFFFFFu) {
                for (const GuiTagEntry& stliEntry : stliEntries) {
                    if (stliEntry.subgroupTag != mapNameStli) continue;
                    std::vector<uint8_t> stliRaw;
                    if (readTagPayload(stliEntry, stliRaw)) {
                        std::string name = firstLine(stliToText(stliRaw));
                        if (!name.empty()) choice.name = name;
                    }
                    break;
                }
            }
        }

        state.availableMeshTags.push_back(choice);
    }

    std::sort(state.availableMeshTags.begin(), state.availableMeshTags.end(),
              [](const MeshChoice& a, const MeshChoice& b) {
                  if (a.campaignLabel != b.campaignLabel) {
                      if (a.campaignLabel.empty()) return false;
                      if (b.campaignLabel.empty()) return true;
                      return a.campaignLabel < b.campaignLabel;
                  }
                  if (a.name != b.name) return a.name < b.name;
                  return a.tag < b.tag;
              });

    if (state.availableMeshTags.empty()) {
        state.mapScanStatus = "No mesh tags found.";
    } else {
        state.mapScanStatus = "Found " + std::to_string(state.availableMeshTags.size()) +
                              " mesh tags in " + std::to_string(fileCount) + " file(s).";
    }
}

static void drawWorkflowPanel(AppState& state) {
    fs::path repoDir = state.exeDir.parent_path();
    applyLockedDockWindowClass(state.lockDockLayout);
    static bool workflowFocusPending = true;
    if (workflowFocusPending) {
        ImGui::SetNextWindowFocus();
        workflowFocusPending = false;
    }
    ImGui::Begin("Workflow", nullptr, lockedDockPanelFlags(state.lockDockLayout));
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float halfButtonWidth = (availWidth - spacing) * 0.5f;
    const bool ready = hasRequiredLayout(state);
    const std::vector<std::string> workflowIssues = collectWorkflowIssues(state);

    const ImGuiTreeNodeFlags defaultOpen = ImGuiTreeNodeFlags_DefaultOpen;

    if (ImGui::CollapsingHeader("Source", defaultOpen)) {
        ImGui::TextDisabled("Choose a tags folder or plugin file, then pick the map tag you want to work with.");
        labeledInputText("Tags Source or Plugin File", "##tagsSource", state.tagsSource);
        if (halfButtonWidth >= 120.0f) {
            if (ImGui::Button("Tags Folder", ImVec2(halfButtonWidth, 0.0f))) {
                pickFolderIntoBuffer(state.tagsSource, repoDir);
            }
            ImGui::SameLine();
            if (ImGui::Button("Plugin File", ImVec2(halfButtonWidth, 0.0f))) {
                pickFileIntoBuffer(state.tagsSource, repoDir, "Select Plugin File");
            }
        } else {
            if (ImGui::Button("Tags Folder", ImVec2(-1.0f, 0.0f))) {
                pickFolderIntoBuffer(state.tagsSource, repoDir);
            }
            if (ImGui::Button("Plugin File", ImVec2(-1.0f, 0.0f))) {
                pickFileIntoBuffer(state.tagsSource, repoDir, "Select Plugin File");
            }
        }

        labeledInputText("Mesh Tag", "##meshTag", state.meshTag);
        if (std::strcmp(state.lastScannedTagsSource.c_str(), state.tagsSource.data()) != 0) {
            state.mapScanStatus = "Tags source changed. Refresh map list.";
        }
        if (ImGui::Button("Refresh Map List", ImVec2(-1.0f, 0.0f))) {
            refreshAvailableMeshTags(state);
            syncDerivedPaths(state);
        }
        ImGui::SetNextItemWidth(-1.0f);
        std::string selectedDisplay = selectedMeshChoiceDisplay(state);
        if (ImGui::BeginCombo("##meshTagPicker", selectedDisplay.c_str())) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##meshTagFilter", state.meshTagFilter.data(), state.meshTagFilter.size());
            ImGui::Separator();
            std::string filter = state.meshTagFilter.data();
            for (const MeshChoice& choice : state.availableMeshTags) {
                std::string display = meshChoiceDisplay(choice);
                if (!filter.empty() &&
                    choice.tag.find(filter) == std::string::npos &&
                    choice.campaignLabel.find(filter) == std::string::npos &&
                    choice.name.find(filter) == std::string::npos) {
                    continue;
                }
                const bool selected = (choice.tag == state.meshTag.data());
                if (ImGui::Selectable(display.c_str(), selected)) {
                    copyToBuffer(state.meshTag, choice.tag);
                    syncDerivedPaths(state);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (!state.mapScanStatus.empty()) {
            ImGui::TextDisabled("%s", state.mapScanStatus.c_str());
        }
    }

    if (ImGui::CollapsingHeader("Output", defaultOpen)) {
        ImGui::TextDisabled("These paths are the handoff between extraction, plugin packaging, and Blender export.");
        labeledInputText("Extracted Map Folder", "##outputFolder", state.outputFolder);
        if (ImGui::Button("Browse Output", ImVec2(-1.0f, 0.0f))) {
            pickFolderIntoBuffer(state.outputFolder, repoDir);
        }

        labeledInputText("Plugin Output File or Folder", "##pluginOutput", state.pluginOutput);
        if (ImGui::Button("Browse Plugin", ImVec2(-1.0f, 0.0f))) {
            saveFileIntoBuffer(state.pluginOutput, repoDir, "Select Plugin Output");
        }
        ImGui::TextDisabled("Build Plugin uses: extracted map folder -> plugin output path. If plugin output is a folder, Myth2ools appends <meshtag>_plugin.");

        labeledInputText("Blender Executable", "##blenderPath", state.blenderPath);
        if (ImGui::Button("Browse Blender", ImVec2(-1.0f, 0.0f))) {
            pickFileIntoBuffer(state.blenderPath, repoDir, "Select Blender Executable");
        }
    }

    if (ImGui::CollapsingHeader("Options")) {
        ImGui::TextDisabled("These toggles control how scripts write files and how output paths stay aligned.");
        ImGui::Checkbox("Overwrite exports", &state.overwrite);
        ImGui::Checkbox("Write ORA from extract_map", &state.writeOra);
        ImGui::Checkbox("Omit static animation snapshots in map_combined.obj", &state.exportNoAnimationSnapshots);
        ImGui::Checkbox("Use --edit for build_plugin", &state.editOnBuild);
        if (ImGui::Checkbox("Auto-append selected map tag to output paths", &state.autoAppendMeshTag)) {
            syncDerivedPaths(state);
        }
        if (ImGui::Button("Save Settings")) {
            state.settingsStatus = saveSettings(state) ? "Settings saved." : "Failed to save settings.";
        }
        if (!state.settingsStatus.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", state.settingsStatus.c_str());
        }
    }

    if (ImGui::CollapsingHeader("Run", defaultOpen)) {
        std::string scriptsText = state.scriptDir.string();
        std::string toolsText = state.toolDir.string();
        if (!ready) {
            scriptsText += " (missing required files)";
            toolsText += " (missing required files)";
        }
        ImGui::SeparatorText("Resolved Paths");
        ImGui::TextWrapped("Scripts: %s", scriptsText.c_str());
        ImGui::TextWrapped("Tools: %s", toolsText.c_str());

        const bool busy = state.runner.running.load();
        const bool validWorkflow = workflowIssues.empty();
        auto launchOrGuard = [&](const std::string& command, const char* label) {
            if (state.actionsLoaded && state.actionsDirty) {
                state.pendingDiscard = AppState::PENDING_RUN;
                state.pendingRunCommand = command;
                state.pendingRunLabel = label;
                state.openDiscardPopup = true;
            } else {
                state.runner.start(command);
            }
        };

        if (busy || !ready || !validWorkflow) ImGui::BeginDisabled();
        pushPrimaryButtonStyle();
        const float thirdButtonWidth = (availWidth - spacing * 2.0f) / 3.0f;
        if (thirdButtonWidth >= 110.0f) {
            if (ImGui::Button("Extract Assets", ImVec2(thirdButtonWidth, 0))) {
                launchOrGuard(buildExtractAssetsCommand(state), "Extract Assets");
            }
            ImGui::SameLine();
            if (ImGui::Button("Build Plugin", ImVec2(thirdButtonWidth, 0))) {
                launchOrGuard(buildPluginCommand(state), "Build Plugin");
            }
            ImGui::SameLine();
            if (ImGui::Button("Create Blend", ImVec2(thirdButtonWidth, 0))) {
                launchOrGuard(buildCreateBlendCommand(state), "Create Blend");
            }
        } else {
            if (ImGui::Button("Extract Assets", ImVec2(-1.0f, 0))) {
                launchOrGuard(buildExtractAssetsCommand(state), "Extract Assets");
            }
            if (ImGui::Button("Build Plugin", ImVec2(-1.0f, 0))) {
                launchOrGuard(buildPluginCommand(state), "Build Plugin");
            }
            if (ImGui::Button("Create Blend", ImVec2(-1.0f, 0))) {
                launchOrGuard(buildCreateBlendCommand(state), "Create Blend");
            }
        }
        popPrimaryButtonStyle();
        if (busy || !ready || !validWorkflow) ImGui::EndDisabled();

        if (!ready || !workflowIssues.empty()) {
            ImGui::Separator();
        }
        if (!ready) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Could not resolve the Myth2ools scripts/tools layout yet.");
        }
        for (const std::string& issue : workflowIssues) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.35f, 1.0f), "%s", issue.c_str());
        }
    }

    ImGui::End();
}

static void setActionsStatus(AppState& state, const std::string& message, bool writeToLog) {
    state.actionsStatus = message;
    if (writeToLog) {
        state.runner.append("[actions] " + message);
    }
}

static void logActionSummary(AppState& state, const json& action, const char* phase) {
    std::ostringstream oss;
    oss << "[actions] " << phase
        << " id=" << jsonIntOrDefault(action, "id")
        << " type=" << jsonStringOrEmpty(action, "type")
        << " offset=" << jsonIntOrDefault(action, "parameter_data_offset")
        << " size=" << jsonIntOrDefault(action, "parameter_data_size")
        << " hex=" << jsonStringOrEmpty(action, "parameter_data_hex");
    state.runner.append(oss.str());
}

static void drawLogPanel(AppState& state) {
    applyLockedDockWindowClass(state.lockDockLayout);
    ImGui::Begin("Command Log", nullptr, lockedDockPanelFlags(state.lockDockLayout));
    ImGui::TextDisabled("Live shell output from extraction, plugin build, and action save operations.");
    drawStatusChip(state.runner.running.load() ? "Streaming Output" : "Idle",
                   state.runner.running.load() ? ImVec4(0.18f, 0.54f, 0.35f, 1.0f)
                                               : ImVec4(0.26f, 0.30f, 0.36f, 1.0f));
    ImGui::Separator();
    if (ImGui::Button("Clear Log")) {
        state.runner.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy All")) {
        std::string fullLog;
        std::lock_guard<std::mutex> lock(state.runner.logMutex);
        for (const std::string& line : state.runner.logLines) {
            fullLog += line;
            fullLog += "\r\n";
        }
        ImGui::SetClipboardText(fullLog.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Log")) {
        fs::path savedPath;
        if (saveLogToFile(state, savedPath)) {
            state.logSaveStatus = "Saved log to " + savedPath.string();
        } else {
            state.logSaveStatus = "Failed to save log.";
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &state.followLog);
    ImGui::SameLine();
    if (state.runner.running.load()) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "Running...");
    } else {
        ImGui::TextDisabled("Idle");
    }
    if (!state.logSaveStatus.empty()) {
        ImGui::TextWrapped("%s", state.logSaveStatus.c_str());
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##logFilter", "Filter (case-insensitive substring)", state.logFilter.data(), state.logFilter.size());

    ImGui::Separator();

    static std::vector<std::string> logSnapshot;
    logSnapshot.clear();
    {
        std::lock_guard<std::mutex> lock(state.runner.logMutex);
        logSnapshot.reserve(state.runner.logLines.size());
        for (const std::string& line : state.runner.logLines) {
            logSnapshot.push_back(line);
        }
    }

    std::string filter = state.logFilter.data();
    std::string filterLower;
    filterLower.reserve(filter.size());
    for (char c : filter) filterLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    static std::vector<int> visibleLogIndices;
    visibleLogIndices.clear();
    visibleLogIndices.reserve(logSnapshot.size());
    if (filterLower.empty()) {
        for (size_t i = 0; i < logSnapshot.size(); ++i) {
            visibleLogIndices.push_back(static_cast<int>(i));
        }
    } else {
        for (size_t i = 0; i < logSnapshot.size(); ++i) {
            const std::string& line = logSnapshot[i];
            std::string lower;
            lower.reserve(line.size());
            for (char c : line) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (lower.find(filterLower) != std::string::npos) {
                visibleLogIndices.push_back(static_cast<int>(i));
            }
        }
        ImGui::TextDisabled("%zu of %zu lines", visibleLogIndices.size(), logSnapshot.size());
    }

    static std::string logBuffer;
    size_t totalSize = 0;
    for (int idx : visibleLogIndices) totalSize += logSnapshot[static_cast<size_t>(idx)].size() + 1;
    logBuffer.clear();
    logBuffer.reserve(totalSize);
    for (int idx : visibleLogIndices) {
        logBuffer.append(logSnapshot[static_cast<size_t>(idx)]);
        logBuffer.push_back('\n');
    }

    static size_t prevLogBufLen = 0;
    const bool contentGrew = logBuffer.size() > prevLogBufLen;
    prevLogBufLen = logBuffer.size();

    if (g_monoFont) ImGui::PushFont(g_monoFont);

    const ImGuiID inputId = ImGui::GetID("##logview");
    char childName[512];
    std::snprintf(childName, sizeof(childName), "%s/##logview_%08X",
                  ImGui::GetCurrentWindow()->Name, inputId);

    ImGui::InputTextMultiline(
        "##logview",
        logBuffer.empty() ? const_cast<char*>("") : &logBuffer[0],
        logBuffer.empty() ? 1u : logBuffer.size() + 1,
        ImVec2(-1.0f, -1.0f),
        ImGuiInputTextFlags_ReadOnly);

    const bool inputActive = ImGui::IsItemActive();
    if (state.followLog && contentGrew && !inputActive) {
        if (ImGuiWindow* child = ImGui::FindWindowByName(childName)) {
            ImGui::SetScrollY(child, child->ScrollMax.y);
        }
    }

    if (g_monoFont) ImGui::PopFont();
    ImGui::End();
}

static void drawActionsPanel(AppState& state) {
    const char* actionsWindowTitle = state.actionsDirty ? "Actions *###Actions" : "Actions###Actions";
    applyLockedDockWindowClass(state.lockDockLayout);
    ImGui::Begin(actionsWindowTitle, nullptr, lockedDockPanelFlags(state.lockDockLayout));

    int loadedCount = 0;
    if (state.actionsLoaded && state.actionsDoc.contains("actions") && state.actionsDoc["actions"].is_array()) {
        loadedCount = static_cast<int>(state.actionsDoc["actions"].size());
    }
    drawStatusChip(state.actionsLoaded ? "Loaded" : "Not Loaded",
                   state.actionsLoaded ? ImVec4(0.22f, 0.41f, 0.62f, 1.0f)
                                       : ImVec4(0.26f, 0.30f, 0.36f, 1.0f));
    ImGui::SameLine();
    std::string countChip = std::string("Actions ") + std::to_string(loadedCount);
    drawStatusChip(countChip.c_str(), ImVec4(0.29f, 0.24f, 0.46f, 1.0f));
    ImGui::Separator();

    if (ImGui::Button("Load Actions")) {
        if (state.actionsLoaded && state.actionsDirty) {
            state.pendingDiscard = AppState::PENDING_RELOAD;
            state.openDiscardPopup = true;
        } else {
            loadActionsDoc(state);
        }
    }
    ImGui::SameLine();
    bool canSave = state.actionsLoaded && state.actionsDirty;
    if (!canSave) ImGui::BeginDisabled();
    pushPrimaryButtonStyle();
    if (ImGui::Button("Save Actions")) {
        saveActionsDoc(state);
    }
    popPrimaryButtonStyle();
    if (!canSave) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("Discard Changes")) {
        state.pendingDiscard = AppState::PENDING_RELOAD;
        state.openDiscardPopup = true;
    }
    if (!canSave) ImGui::EndDisabled();
    ImGui::SameLine();
    if (state.actionsDirty) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Modified");
    } else {
        ImGui::TextDisabled("Clean");
    }

    if (state.openDiscardPopup) {
        ImGui::OpenPopup("Discard unsaved action changes?##discard");
        state.openDiscardPopup = false;
    }
    if (ImGui::BeginPopupModal("Discard unsaved action changes?##discard", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("You have unsaved action edits.");
        switch (state.pendingDiscard) {
        case AppState::PENDING_QUIT:
            ImGui::Text("Quitting will discard them unless you save first.");
            break;
        case AppState::PENDING_RELOAD:
            ImGui::Text("Reloading from disk will discard them.");
            break;
        case AppState::PENDING_RUN:
            ImGui::Text("Running '%s' uses files on disk and ignores your in-memory edits.", state.pendingRunLabel.c_str());
            break;
        default:
            break;
        }
        ImGui::Separator();

        const auto runPending = [&]() {
            if (!state.pendingRunCommand.empty()) {
                state.runner.start(state.pendingRunCommand);
                state.pendingRunCommand.clear();
                state.pendingRunLabel.clear();
            }
        };
        const auto clearDirty = [&]() {
            state.actionsDirty = false;
            state.actionsStructureDirty = false;
            state.dirtyActionIndices.clear();
        };

        if (state.pendingDiscard == AppState::PENDING_QUIT || state.pendingDiscard == AppState::PENDING_RUN) {
            pushPrimaryButtonStyle();
            if (ImGui::Button("Save and Continue", ImVec2(170.0f, 0.0f))) {
                AppState::PendingDiscard intent = state.pendingDiscard;
                if (saveActionsDoc(state)) {
                    state.pendingDiscard = AppState::PENDING_NONE;
                    ImGui::CloseCurrentPopup();
                    if (intent == AppState::PENDING_QUIT) {
                        glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
                    } else if (intent == AppState::PENDING_RUN) {
                        runPending();
                    }
                }
            }
            popPrimaryButtonStyle();
            ImGui::SameLine();
        }

        const char* discardLabel =
            state.pendingDiscard == AppState::PENDING_QUIT  ? "Discard and Quit" :
            state.pendingDiscard == AppState::PENDING_RUN   ? "Discard and Run"  :
                                                              "Discard and Reload";
        if (ImGui::Button(discardLabel, ImVec2(180.0f, 0.0f))) {
            AppState::PendingDiscard intent = state.pendingDiscard;
            state.pendingDiscard = AppState::PENDING_NONE;
            ImGui::CloseCurrentPopup();
            if (intent == AppState::PENDING_RELOAD) {
                loadActionsDoc(state);
            } else if (intent == AppState::PENDING_QUIT) {
                clearDirty();
                glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
            } else if (intent == AppState::PENDING_RUN) {
                clearDirty();
                runPending();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
            state.pendingDiscard = AppState::PENDING_NONE;
            state.pendingRunCommand.clear();
            state.pendingRunLabel.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (state.actionsLoaded) {
        json& actionsToolbar = state.actionsDoc["actions"];
        if (ImGui::Button("Add Action")) {
            int insertIndex = actionsToolbar.is_array() ? static_cast<int>(actionsToolbar.size()) : 0;
            remapDirtyIndicesAfterInsert(state, insertIndex);
            actionsToolbar.push_back(makeDefaultActionDoc(actionsToolbar));
            setSelectedActionIndex(state, insertIndex);
            state.actionsStructureDirty = true;
            refreshAllActionsDirtyState(state);
            setActionsStatus(state, "Added action " + std::to_string(jsonIntOrDefault(actionsToolbar[static_cast<size_t>(insertIndex)], "id")));
        }
        ImGui::SameLine();
        bool canDuplicate = state.selectedActionIndex >= 0 && state.selectedActionIndex < static_cast<int>(actionsToolbar.size());
        if (!canDuplicate) ImGui::BeginDisabled();
        if (ImGui::Button("Duplicate Action")) {
            json clone = actionsToolbar[static_cast<size_t>(state.selectedActionIndex)];
            clone["id"] = nextAvailableActionId(actionsToolbar);
            std::string name = jsonStringOrEmpty(clone, "name");
            if (!name.empty()) clone["name"] = name + " Copy";
            int insertIndex = state.selectedActionIndex + 1;
            remapDirtyIndicesAfterInsert(state, insertIndex);
            actionsToolbar.insert(actionsToolbar.begin() + static_cast<ptrdiff_t>(insertIndex), clone);
            setSelectedActionIndex(state, insertIndex);
            state.actionsStructureDirty = true;
            refreshAllActionsDirtyState(state);
            setActionsStatus(state, "Duplicated action to " + std::to_string(jsonIntOrDefault(clone, "id")));
        }
        if (!canDuplicate) ImGui::EndDisabled();
        ImGui::SameLine();
        bool canDelete = canDuplicate;
        if (!canDelete) ImGui::BeginDisabled();
        if (ImGui::Button("Delete Action")) {
            int deleteIndex = state.selectedActionIndex;
            int deletedId = jsonIntOrDefault(actionsToolbar[static_cast<size_t>(deleteIndex)], "id");
            actionsToolbar.erase(actionsToolbar.begin() + static_cast<ptrdiff_t>(deleteIndex));
            remapDirtyIndicesAfterDelete(state, deleteIndex);
            state.actionsStructureDirty = true;
            if (actionsToolbar.empty()) {
                setSelectedActionIndex(state, -1);
            } else if (deleteIndex >= static_cast<int>(actionsToolbar.size())) {
                setSelectedActionIndex(state, static_cast<int>(actionsToolbar.size()) - 1);
            } else {
                setSelectedActionIndex(state, deleteIndex);
            }
            refreshAllActionsDirtyState(state);
            setActionsStatus(state, "Deleted action " + std::to_string(deletedId));
        }
        if (!canDelete) ImGui::EndDisabled();
    }

    if (!state.actionsStatus.empty()) {
        ImGui::TextWrapped("%s", state.actionsStatus.c_str());
    }

    fs::path path = actionsJsonPath(state);
    ImGui::TextDisabled("%s", path.string().c_str());
    ImGui::Separator();

    if (!state.actionsLoaded) {
        ImGui::TextWrapped("Load the extracted map's actions.json to browse and edit action records.");
        ImGui::End();
        return;
    }

    json& actions = state.actionsDoc["actions"];
    ImGui::InputTextWithHint("##actionFilter", "Filter by name, type, or id", state.actionFilter.data(), state.actionFilter.size());
    std::string filter = state.actionFilter.data();
    std::string filterLower;
    filterLower.reserve(filter.size());
    for (char c : filter) filterLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    std::vector<int> visibleIndices;
    visibleIndices.reserve(actions.size());
    for (size_t i = 0; i < actions.size(); ++i) {
        if (!filterLower.empty()) {
            const json& a = actions[i];
            std::string haystack = jsonStringOrEmpty(a, "name");
            haystack += '\n';
            haystack += jsonStringOrEmpty(a, "type");
            haystack += '\n';
            haystack += std::to_string(jsonIntOrDefault(a, "id"));
            for (char& c : haystack) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (haystack.find(filterLower) == std::string::npos) continue;
        }
        visibleIndices.push_back(static_cast<int>(i));
    }

    float splitterWidth = 8.0f;
    const float minListWidth = 260.0f;
    const float minEditorWidth = 360.0f;
    static float actionsListWidth = 380.0f;
    float totalWidth = ImGui::GetContentRegionAvail().x;
    float maxListWidth = totalWidth - minEditorWidth - splitterWidth;
    if (maxListWidth < minListWidth) maxListWidth = minListWidth;
    if (actionsListWidth < minListWidth) actionsListWidth = minListWidth;
    if (actionsListWidth > maxListWidth) actionsListWidth = maxListWidth;

    ImGui::BeginChild("actions_list", ImVec2(actionsListWidth, 0.0f), true);
    const ImVec4 dirtyColor(1.0f, 0.75f, 0.35f, 1.0f);
    const std::set<int> relatedActionIndices = relatedActionIndicesForSelectedMapUnit(state);
    ImGui::TextDisabled("%zu of %zu actions", visibleIndices.size(), actions.size());
    ImGui::Separator();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visibleIndices.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            int i = visibleIndices[static_cast<size_t>(row)];
            const json& action = actions[static_cast<size_t>(i)];
            std::string name = jsonStringOrEmpty(action, "name");
            std::string visibleLabel = name.empty() ? "Untitled Action" : name;
            std::string typeStr = jsonStringOrEmpty(action, "type");
            int actionRowId = jsonIntOrDefault(action, "id");
            int actionIndent = (std::max)(0, jsonIntOrDefault(action, "indent"));
            const char* typeLabel = guiActionTypeLabel(typeStr);
            std::string meta = "L" + std::to_string(actionIndent) + "  #" + std::to_string(actionRowId);
            if (!typeStr.empty()) {
                meta += "  ";
                meta += typeLabel ? typeLabel : typeStr;
            } else {
                meta += "  Container";
            }
            const bool dirty = state.dirtyActionIndices.count(i) > 0;
            std::string label = (dirty ? "* " : "  ") + visibleLabel + "  [" + meta + "]##action" + std::to_string(i);
            const bool selected = (i == state.selectedActionIndex);
            const bool relatedToSelectedUnit = relatedActionIndices.count(i) > 0;
            const float baseCursorX = ImGui::GetCursorPosX();
            const float indentPixels = static_cast<float>((std::min)(actionIndent, 12)) * 14.0f;
            ImGui::SetCursorPosX(baseCursorX + indentPixels);
            if (relatedToSelectedUnit) {
                ImGui::PushStyleColor(ImGuiCol_Header, selected ? ImVec4(0.26f, 0.36f, 0.56f, 0.95f) : ImVec4(0.19f, 0.28f, 0.46f, 0.78f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.28f, 0.39f, 0.60f, 0.92f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.32f, 0.44f, 0.68f, 0.98f));
            }
            if (dirty) ImGui::PushStyleColor(ImGuiCol_Text, dirtyColor);
            if (ImGui::Selectable(label.c_str(), selected)) {
                setSelectedActionIndex(state, i);
            }
            if (dirty) ImGui::PopStyleColor();
            if (relatedToSelectedUnit) ImGui::PopStyleColor(3);
        }
    }
    clipper.End();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 0.0f);
    ImVec2 splitterPos = ImGui::GetCursorScreenPos();
    float splitterHeight = ImGui::GetContentRegionAvail().y;
    ImGui::InvisibleButton("##actions_splitter", ImVec2(splitterWidth, splitterHeight));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive()) {
        actionsListWidth += ImGui::GetIO().MouseDelta.x;
        if (actionsListWidth < minListWidth) actionsListWidth = minListWidth;
        if (actionsListWidth > maxListWidth) actionsListWidth = maxListWidth;
    }
    ImU32 splitterColor = ImGui::GetColorU32(ImGui::IsItemActive()
        ? ImVec4(0.36f, 0.64f, 0.94f, 1.0f)
        : (ImGui::IsItemHovered() ? ImVec4(0.28f, 0.48f, 0.72f, 1.0f)
                                  : ImVec4(0.19f, 0.22f, 0.27f, 1.0f)));
    ImGui::GetWindowDrawList()->AddRectFilled(splitterPos,
                                              ImVec2(splitterPos.x + splitterWidth, splitterPos.y + splitterHeight),
                                              splitterColor,
                                              4.0f);

    ImGui::SameLine(0.0f, 0.0f);

    ImGui::BeginChild("actions_editor", ImVec2(0.0f, 0.0f), true);
    if (state.selectedActionIndex < 0 || state.selectedActionIndex >= static_cast<int>(actions.size())) {
        ImGui::TextWrapped("Select an action to inspect or edit it.");
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    json& action = actions[static_cast<size_t>(state.selectedActionIndex)];

    std::array<char, 512> nameBuf{};
    copyToBuffer(nameBuf, jsonStringOrEmpty(action, "name"));
    std::array<char, 64> typeBuf{};
    copyToBuffer(typeBuf, jsonStringOrEmpty(action, "type"));
    int actionId = jsonIntOrDefault(action, "id");
    int indent = jsonIntOrDefault(action, "indent");
    int flagsRaw = jsonIntOrDefault(action, "flags_raw");
    int expirationModeId = jsonIntOrDefault(action, "expiration_mode_id", guiExpirationModeIdFromName(jsonStringOrEmpty(action, "expiration_mode")));
    float lower = static_cast<float>(jsonDoubleOrDefault(action, "trigger_time_lower_bound_seconds"));
    float delta = static_cast<float>(jsonDoubleOrDefault(action, "trigger_time_delta_seconds"));
    const char* currentTypeLabel = guiActionTypeLabel(typeBuf.data());
    const std::string actionTitle = std::strlen(nameBuf.data()) ? std::string(nameBuf.data()) : std::string("Untitled Action");
    std::string detailLine = std::string("ID ") + std::to_string(actionId) + "  |  " +
                             (std::strlen(typeBuf.data()) ? std::string(typeBuf.data()) : std::string("container"));
    if (currentTypeLabel) {
        detailLine += " - ";
        detailLine += currentTypeLabel;
    }
    detailLine += "  |  ";
    detailLine += std::to_string(action.contains("parameters") && action["parameters"].is_array() ? action["parameters"].size() : 0);
    detailLine += " parameter(s)";

    ImGui::TextUnformatted(actionTitle.c_str());
    ImGui::TextDisabled("%s", detailLine.c_str());
    ImGui::Separator();

    if (ImGui::InputText("Name", nameBuf.data(), nameBuf.size())) {
        action["name"] = std::string(nameBuf.data());
        markCurrentActionDirty(state);
    }
    {
        std::string currentType(typeBuf.data());
        const char* currentLabel = guiActionTypeLabel(currentType);
        std::string previewBuf;
        if (currentType.empty()) {
            previewBuf = "(container / no type)";
        } else if (currentLabel) {
            previewBuf = currentType + "  -  " + currentLabel;
        } else {
            previewBuf = currentType + "  -  (custom)";
        }

        static std::array<char, 64> typeFilter{};
        if (ImGui::BeginCombo("Type", previewBuf.c_str())) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##typeFilter", "Filter or type a custom 4-char FourCC",
                                     typeFilter.data(), typeFilter.size());
            std::string filterStr = typeFilter.data();
            std::string filterLower;
            filterLower.reserve(filterStr.size());
            for (char c : filterStr) filterLower.push_back((char)std::tolower((unsigned char)c));

            // Built-in entries: every known FourCC + display label.
            struct Entry { const char* code; const char* label; };
            static const Entry kEntries[] = {
                {"acli", "Action List"},          {"ambi", "Ambient Sound Control"},
                {"anim", "Model Animation"},      {"atta", "Attack"},
                {"ctrl", "Unit Control"},         {"dela", "Delay"},
                {"endg", "Endgame Condition"},    {"gene", "General Action"},
                {"geom", "Geometry Filter"},      {"girl", "Harass"},
                {"lead", "Leading"},              {"legi", "Legion"},
                {"ligh", "Lightning"},            {"lpgr", "Local Projectile Group"},
                {"mean", "Meander"},              {"mele", "Melee"},
                {"miss", "Mission"},              {"moef", "Model Effect"},
                {"moma", "Move Marker"},          {"move", "Movement"},
                {"mung", "Munger"},               {"ngty", "Netgame Type"},
                {"obmo", "Observer Movement"},   {"part", "Particle System Control"},
                {"pick", "Pick Up Object"},      {"plat", "Platoon"},
                {"plmo", "Platoon Movement"},    {"plsc", "Platoon Scouting"},
                {"rout", "Rout"},                {"snif", "Sniffer"},
                {"soun", "Sound Action"},        {"squa", "Squad"},
                {"suic", "Suicide"},             {"surr", "Surround"},
                {"tuni", "Test Unit"},           {"wand", "Wandering Movement"},
            };

            // Container (no type) is a real choice.
            {
                bool selected = currentType.empty();
                if (ImGui::Selectable("(container / no type)", selected)) {
                    action["type"] = std::string();
                    copyToBuffer(typeBuf, std::string());
                    markCurrentActionDirty(state);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();

            int shown = 0;
            for (const Entry& e : kEntries) {
                std::string code = e.code;
                std::string lbl = e.label;
                if (!filterLower.empty()) {
                    std::string hay = code + " " + lbl;
                    for (char& c : hay) c = (char)std::tolower((unsigned char)c);
                    if (hay.find(filterLower) == std::string::npos) continue;
                }
                std::string line = code + "  -  " + lbl;
                bool selected = (currentType == code);
                if (ImGui::Selectable(line.c_str(), selected)) {
                    action["type"] = code;
                    copyToBuffer(typeBuf, code);
                    markCurrentActionDirty(state);
                }
                if (selected) ImGui::SetItemDefaultFocus();
                shown++;
            }

            // Apply-custom-from-filter: if the filter is exactly 4 characters
            // and doesn't match a known code, offer to set it as a custom type.
            if (filterStr.size() == 4 && guiActionTypeLabel(filterStr) == nullptr) {
                ImGui::Separator();
                std::string apply = std::string("Apply custom type: ") + filterStr;
                if (ImGui::Selectable(apply.c_str())) {
                    action["type"] = filterStr;
                    copyToBuffer(typeBuf, filterStr);
                    markCurrentActionDirty(state);
                }
            } else if (shown == 0 && !filterStr.empty()) {
                ImGui::TextDisabled("No matches. Type a 4-character custom FourCC to apply.");
            }
            ImGui::EndCombo();
        }
    }
    const char* expirationModes[] = {
        "trigger",
        "execution",
        "successful_execution",
        "never",
        "failed_execution"
    };
    if (expirationModeId < 0 || expirationModeId > 4) expirationModeId = 0;
    if (ImGui::Combo("Expiration Mode", &expirationModeId, expirationModes, IM_ARRAYSIZE(expirationModes))) {
        action["expiration_mode_id"] = expirationModeId;
        action["expiration_mode"] = std::string(guiExpirationModeName(expirationModeId));
        markCurrentActionDirty(state);
    }
    if (ImGui::InputInt("ID", &actionId)) {
        action["id"] = actionId;
        markCurrentActionDirty(state);
    }
    if (ImGui::InputInt("Indent", &indent)) {
        action["indent"] = indent;
        markCurrentActionDirty(state);
    }
    if (ImGui::InputInt("Flags Raw", &flagsRaw)) {
        action["flags_raw"] = flagsRaw;
        syncActionFlagsJson(action);
        markCurrentActionDirty(state);
    }
    if (ImGui::InputFloat("Trigger Lower (s)", &lower, 0.5f, 5.0f, "%.4f")) {
        action["trigger_time_lower_bound_seconds"] = lower;
        markCurrentActionDirty(state);
    }
    if (ImGui::InputFloat("Trigger Delta (s)", &delta, 0.5f, 5.0f, "%.4f")) {
        action["trigger_time_delta_seconds"] = delta;
        markCurrentActionDirty(state);
    }

    ImGui::SeparatorText("Parameters");
    if (action.contains("parameters") && action["parameters"].is_array()) {
        for (size_t pi = 0; pi < action["parameters"].size(); ++pi) {
            json& param = action["parameters"][pi];
            std::string paramName = jsonStringOrEmpty(param, "name");
            std::string paramType = jsonStringOrEmpty(param, "type");
            if (ImGui::TreeNode((paramName + " (" + paramType + ")##param" + std::to_string(pi)).c_str())) {
                if (drawActionParameterEditor(state, param, pi)) {
                    markCurrentActionDirty(state);
                }
                ImGui::TreePop();
            }
        }
    } else {
        ImGui::TextDisabled("No parameters.");
    }

    ImGui::EndChild();
    ImGui::End();
}

static void drawMapPreviewPanel(AppState& state) {
    applyLockedDockWindowClass(state.lockDockLayout);
    ImGui::Begin("Map Preview", nullptr, lockedDockPanelFlags(state.lockDockLayout));
    syncMapPreviewSource(state);

    if (ImGui::Button("Refresh Preview")) {
        state.mapPreviewNeedsReload = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset View")) {
        resetMapPreviewView(state);
    }
    ImGui::SameLine();
    const bool hasSelection = !state.selectedMapUnitIdentifiers.empty() || state.mapPreviewPickTarget.has_value();
    if (!hasSelection) ImGui::BeginDisabled();
    if (ImGui::Button("Clear Selection")) {
        clearMapUnitSelection(state);
    }
    if (!hasSelection) ImGui::EndDisabled();
    ImGui::Separator();

    if (state.mapPreviewNeedsReload) {
        reloadMapPreviewTexture(state);
    }

    ImGui::BeginChild("##MapPreviewCanvas", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoScrollbar);
    if (state.mapPreviewTextureId != 0 && state.mapPreviewWidth > 0 && state.mapPreviewHeight > 0) {
        constexpr float terrainPreviewPixelsPerCell = 8.0f;
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 canvasScreenPos = ImGui::GetCursorScreenPos();
        const float fitScaleX = avail.x / static_cast<float>(state.mapPreviewWidth);
        const float fitScaleY = avail.y / static_cast<float>(state.mapPreviewHeight);
        float fitScale = (std::min)(fitScaleX, fitScaleY);
        if (fitScale <= 0.0f) fitScale = 1.0f;

        const bool canvasHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (canvasHovered && io.MouseWheel != 0.0f) {
            const float oldZoom = state.mapPreviewZoom;
            const float oldScale = fitScale * oldZoom;
            const ImVec2 oldImageSize(static_cast<float>(state.mapPreviewWidth) * oldScale,
                                      static_cast<float>(state.mapPreviewHeight) * oldScale);
            const ImVec2 oldCenterOffset((avail.x - oldImageSize.x) * 0.5f,
                                         (avail.y - oldImageSize.y) * 0.5f);
            const ImVec2 mouseLocal(io.MousePos.x - canvasScreenPos.x, io.MousePos.y - canvasScreenPos.y);
            const float zoomFactor = std::pow(1.15f, io.MouseWheel);
            state.mapPreviewZoom = ImClamp(state.mapPreviewZoom * zoomFactor, 1.0f, 24.0f);
            const float newScale = fitScale * state.mapPreviewZoom;
            const ImVec2 imageLocal((mouseLocal.x - oldCenterOffset.x - state.mapPreviewPan.x) / oldScale,
                                    (mouseLocal.y - oldCenterOffset.y - state.mapPreviewPan.y) / oldScale);
            const ImVec2 newImageSize(static_cast<float>(state.mapPreviewWidth) * newScale,
                                      static_cast<float>(state.mapPreviewHeight) * newScale);
            const ImVec2 newCenterOffset((avail.x - newImageSize.x) * 0.5f,
                                         (avail.y - newImageSize.y) * 0.5f);
            state.mapPreviewPan.x = mouseLocal.x - newCenterOffset.x - imageLocal.x * newScale;
            state.mapPreviewPan.y = mouseLocal.y - newCenterOffset.y - imageLocal.y * newScale;
        }

        if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            state.mapPreviewPan.x += io.MouseDelta.x;
            state.mapPreviewPan.y += io.MouseDelta.y;
        }

        const float scale = fitScale * state.mapPreviewZoom;
        ImVec2 imageSize(static_cast<float>(state.mapPreviewWidth) * scale,
                         static_cast<float>(state.mapPreviewHeight) * scale);
        ImVec2 centeredOffset((avail.x - imageSize.x) * 0.5f,
                              (avail.y - imageSize.y) * 0.5f);
        const float maxPanX = imageSize.x > avail.x ? (imageSize.x - avail.x) * 0.5f : 0.0f;
        const float maxPanY = imageSize.y > avail.y ? (imageSize.y - avail.y) * 0.5f : 0.0f;
        state.mapPreviewPan.x = ImClamp(state.mapPreviewPan.x, -maxPanX, maxPanX);
        state.mapPreviewPan.y = ImClamp(state.mapPreviewPan.y, -maxPanY, maxPanY);

        ImVec2 cursor = ImGui::GetCursorPos();
        cursor.x += centeredOffset.x + state.mapPreviewPan.x;
        cursor.y += centeredOffset.y + state.mapPreviewPan.y;
        ImGui::SetCursorPos(cursor);
        ImGui::Image((ImTextureID)(intptr_t)state.mapPreviewTextureId, imageSize);

        const ImVec2 imageMin = ImGui::GetItemRectMin();
        const ImVec2 imageMax = ImGui::GetItemRectMax();
        const bool imageHovered = ImGui::IsItemHovered();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 markerOutline = IM_COL32(25, 30, 36, 255);
        const ImU32 selectedUnitOutline = IM_COL32(126, 214, 255, 255);
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        struct MarkerHitInfo {
            uint16_t identifier = 0;
            std::string tag;
            float screenX = 0.0f;
            float screenY = 0.0f;
        };
        std::vector<MarkerHitInfo> markerHits;
        markerHits.reserve(state.mapPreviewUnitMarkers.size());
        float clickedBestDistanceSq = FLT_MAX;
        float hoveredBestDistanceSq = FLT_MAX;
        std::optional<uint16_t> clickedUnitIdentifier;
        const MapUnitMarker* hoveredMarker = nullptr;
        for (const MapUnitMarker& marker : state.mapPreviewUnitMarkers) {
            const float imageX = static_cast<float>(state.mapPreviewWidth) - (marker.cellX * terrainPreviewPixelsPerCell);
            const float imageY = marker.cellY * terrainPreviewPixelsPerCell;
            const float screenX = imageMin.x + imageX * scale;
            const float screenY = imageMin.y + imageY * scale;
            if (screenX < imageMin.x || screenX > imageMax.x || screenY < imageMin.y || screenY > imageMax.y) continue;
            markerHits.push_back(MarkerHitInfo{marker.identifier, marker.tag, screenX, screenY});

            const bool selectedMapUnit = state.selectedMapUnitIdentifiers.count(marker.identifier) > 0;
            const float markerHalfSize = selectedMapUnit ? 5.5f : 3.0f;
            const ImVec2 p0(screenX - markerHalfSize, screenY - markerHalfSize);
            const ImVec2 p1(screenX + markerHalfSize, screenY + markerHalfSize);
            const ImU32 markerFill = previewMarkerColorForUnitTag(marker.tag);
            drawList->AddRectFilled(p0, p1, markerFill, 1.0f);
            if (selectedMapUnit) {
                drawList->AddRect(ImVec2(p0.x - 2.0f, p0.y - 2.0f),
                                  ImVec2(p1.x + 2.0f, p1.y + 2.0f),
                                  selectedUnitOutline,
                                  1.0f,
                                  0,
                                  2.5f);
            }
            drawList->AddRect(p0, p1, markerOutline, 1.0f, 0, selectedMapUnit ? 2.5f : 1.0f);

            const float dx = mousePos.x - screenX;
            const float dy = mousePos.y - screenY;
            const float distanceSq = dx * dx + dy * dy;
            const float hitRadius = markerHalfSize + 5.0f;
            if (distanceSq <= hitRadius * hitRadius && distanceSq < clickedBestDistanceSq) {
                clickedBestDistanceSq = distanceSq;
                clickedUnitIdentifier = marker.identifier;
            }
            if (distanceSq <= hitRadius * hitRadius && distanceSq < hoveredBestDistanceSq) {
                hoveredBestDistanceSq = distanceSq;
                hoveredMarker = &marker;
            }
        }

        if (state.mapPreviewPickTarget.has_value()) {
            if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (clickedUnitIdentifier.has_value()) {
                    if (applyMapPickedUnitToTarget(state, *clickedUnitIdentifier)) {
                        state.mapPreviewPickTarget.reset();
                    }
                    state.selectedMapUnitIdentifiers.clear();
                    state.selectedMapUnitIdentifiers.insert(*clickedUnitIdentifier);
                } else {
                    state.actionsStatus = "Click directly on a unit marker to fill this value.";
                }
            }
        } else {
            if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                state.mapPreviewDragSelecting = true;
                state.mapPreviewDragStart = mousePos;
                state.mapPreviewDragCurrent = mousePos;
            }
            if (state.mapPreviewDragSelecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                state.mapPreviewDragCurrent = mousePos;
            }
            if (state.mapPreviewDragSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                state.mapPreviewDragSelecting = false;
                const float dragDx = state.mapPreviewDragCurrent.x - state.mapPreviewDragStart.x;
                const float dragDy = state.mapPreviewDragCurrent.y - state.mapPreviewDragStart.y;
                const bool isBoxSelection = std::fabs(dragDx) >= 4.0f || std::fabs(dragDy) >= 4.0f;
                if (isBoxSelection) {
                    const float minX = (std::min)(state.mapPreviewDragStart.x, state.mapPreviewDragCurrent.x);
                    const float minY = (std::min)(state.mapPreviewDragStart.y, state.mapPreviewDragCurrent.y);
                    const float maxX = (std::max)(state.mapPreviewDragStart.x, state.mapPreviewDragCurrent.x);
                    const float maxY = (std::max)(state.mapPreviewDragStart.y, state.mapPreviewDragCurrent.y);
                    std::set<uint16_t> selectedIds = io.KeyShift ? state.selectedMapUnitIdentifiers : std::set<uint16_t>{};
                    for (const MarkerHitInfo& hit : markerHits) {
                        if (hit.screenX >= minX && hit.screenX <= maxX &&
                            hit.screenY >= minY && hit.screenY <= maxY) {
                            selectedIds.insert(hit.identifier);
                        }
                    }
                    state.selectedMapUnitIdentifiers.swap(selectedIds);
                } else if (clickedUnitIdentifier.has_value()) {
                    if (io.KeyShift) {
                        if (state.selectedMapUnitIdentifiers.count(*clickedUnitIdentifier) > 0) {
                            state.selectedMapUnitIdentifiers.erase(*clickedUnitIdentifier);
                        } else {
                            state.selectedMapUnitIdentifiers.insert(*clickedUnitIdentifier);
                        }
                    } else {
                        state.selectedMapUnitIdentifiers.clear();
                        state.selectedMapUnitIdentifiers.insert(*clickedUnitIdentifier);
                    }
                } else {
                    if (!io.KeyShift) state.selectedMapUnitIdentifiers.clear();
                }
            }
        }
        if (state.mapPreviewDragSelecting) {
            const ImVec2 p0((std::min)(state.mapPreviewDragStart.x, state.mapPreviewDragCurrent.x),
                            (std::min)(state.mapPreviewDragStart.y, state.mapPreviewDragCurrent.y));
            const ImVec2 p1((std::max)(state.mapPreviewDragStart.x, state.mapPreviewDragCurrent.x),
                            (std::max)(state.mapPreviewDragStart.y, state.mapPreviewDragCurrent.y));
            drawList->AddRectFilled(p0, p1, IM_COL32(90, 156, 232, 36), 2.0f);
            drawList->AddRect(p0, p1, IM_COL32(110, 184, 255, 220), 2.0f, 0, 1.5f);
        }
        if (hoveredMarker != nullptr) {
            if (state.mapPreviewPickTarget.has_value()) {
                ImGui::SetTooltip("%s #%u\nClick to fill armed parameter",
                                  hoveredMarker->tag.c_str(),
                                  static_cast<unsigned>(hoveredMarker->identifier));
            } else {
                ImGui::SetTooltip("%s #%u",
                                  hoveredMarker->tag.c_str(),
                                  static_cast<unsigned>(hoveredMarker->identifier));
            }
        }
    } else {
        const std::string& message = state.mapPreviewStatus.empty()
            ? std::string("Terrain preview is not available.")
            : state.mapPreviewStatus;
        ImGui::TextWrapped("%s", message.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
}

static void drawUnitInfoPanel(AppState& state) {
    applyLockedDockWindowClass(state.lockDockLayout);
    ImGui::SetNextWindowDockID(ImGui::GetID("MainDockSpace"), ImGuiCond_FirstUseEver);
    ImGui::Begin("Unit Info", nullptr, lockedDockPanelFlags(state.lockDockLayout));

    if (state.selectedMapUnitIdentifiers.empty()) {
        ImGui::TextWrapped("Click a unit marker on the map preview, or drag a selection box, to inspect units and browse related actions.");
        ImGui::End();
        return;
    }

    const std::set<int> relatedActionIndices = relatedActionIndicesForSelectedMapUnit(state);
    if (state.selectedMapUnitIdentifiers.size() == 1) {
        ImGui::Text("Selected Unit: %s", selectedMapUnitLabel(state).c_str());
    } else {
        ImGui::Text("Selected Units: %s", selectedMapUnitLabel(state).c_str());
    }

    ImGui::SeparatorText("Related Actions");
    if (relatedActionIndices.empty()) {
        ImGui::TextDisabled("No matching actions found for this unit.");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("%zu matching actions", relatedActionIndices.size());
    ImGui::BeginChild("##RelatedActions", ImVec2(0.0f, 0.0f), true);
    for (int actionIndex : relatedActionIndices) {
        if (!state.actionsDoc.contains("actions") || !state.actionsDoc["actions"].is_array()) break;
        const json& action = state.actionsDoc["actions"][static_cast<size_t>(actionIndex)];
        const std::string name = jsonStringOrEmpty(action, "name");
        const std::string type = jsonStringOrEmpty(action, "type");
        const int actionId = jsonIntOrDefault(action, "id");
        std::string label = "#" + std::to_string(actionId) + "  ";
        label += name.empty() ? "(unnamed)" : name;
        if (!type.empty()) {
            label += "  [";
            label += type;
            label += "]";
        } else {
            label += "  [container]";
        }
        if (ImGui::Selectable((label + "##related" + std::to_string(actionIndex)).c_str(),
                              actionIndex == state.selectedActionIndex,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            setSelectedActionIndex(state, actionIndex);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ImGui::SetWindowFocus("Actions###Actions");
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

static void setupStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 8.0f;
    style.ScrollbarRounding = 10.0f;
    style.PopupRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.IndentSpacing = 20.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                 = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.56f, 0.61f, 0.68f, 1.00f);
    colors[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.10f, 0.12f, 0.16f, 0.98f);
    colors[ImGuiCol_Border]               = ImVec4(0.20f, 0.24f, 0.30f, 1.00f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.15f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.16f, 0.22f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.20f, 0.28f, 0.38f, 1.00f);
    colors[ImGuiCol_TitleBg]              = ImVec4(0.09f, 0.11f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.11f, 0.15f, 0.20f, 1.00f);
    colors[ImGuiCol_MenuBarBg]            = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.06f, 0.07f, 0.10f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.25f, 0.31f, 0.39f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.32f, 0.40f, 0.50f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.38f, 0.48f, 0.60f, 1.00f);
    colors[ImGuiCol_CheckMark]            = ImVec4(0.42f, 0.76f, 0.96f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.42f, 0.76f, 0.96f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.60f, 0.86f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]               = ImVec4(0.17f, 0.22f, 0.29f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.24f, 0.31f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.30f, 0.40f, 0.51f, 1.00f);
    colors[ImGuiCol_Header]               = ImVec4(0.16f, 0.23f, 0.32f, 0.90f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.23f, 0.33f, 0.44f, 0.95f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.28f, 0.40f, 0.54f, 1.00f);
    colors[ImGuiCol_Separator]            = ImVec4(0.22f, 0.26f, 0.33f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.35f, 0.58f, 0.88f, 1.00f);
    colors[ImGuiCol_SeparatorActive]      = ImVec4(0.42f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_ResizeGrip]           = ImVec4(0.27f, 0.34f, 0.43f, 0.50f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.35f, 0.58f, 0.88f, 0.70f);
    colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.42f, 0.70f, 1.00f, 0.95f);
    colors[ImGuiCol_Tab]                  = ImVec4(0.12f, 0.16f, 0.22f, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.20f, 0.29f, 0.40f, 1.00f);
    colors[ImGuiCol_TabActive]            = ImVec4(0.17f, 0.24f, 0.33f, 1.00f);
    colors[ImGuiCol_TabUnfocused]         = ImVec4(0.09f, 0.12f, 0.17f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.13f, 0.18f, 0.25f, 1.00f);
    colors[ImGuiCol_DockingPreview]       = ImVec4(0.35f, 0.58f, 0.88f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg]       = ImVec4(0.07f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_PlotLines]            = ImVec4(0.70f, 0.74f, 0.82f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]     = ImVec4(0.98f, 0.65f, 0.24f, 1.00f);
    colors[ImGuiCol_PlotHistogram]        = ImVec4(0.36f, 0.72f, 0.94f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.98f, 0.65f, 0.24f, 1.00f);
    colors[ImGuiCol_TextSelectedBg]       = ImVec4(0.29f, 0.50f, 0.79f, 0.45f);
    colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.04f, 0.05f, 0.08f, 0.74f);
}

ImFont* g_monoFont = nullptr;

static void loadEmbeddedFonts(ImGuiIO& io) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = 3;
    cfg.OversampleV = 1;
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<std::uint8_t*>(kInterRegular_data),
        static_cast<int>(kInterRegular_size),
        16.0f, &cfg);

    ImFontConfig monoCfg;
    monoCfg.FontDataOwnedByAtlas = false;
    monoCfg.OversampleH = 2;
    monoCfg.OversampleV = 1;
    g_monoFont = io.Fonts->AddFontFromMemoryTTF(
        const_cast<std::uint8_t*>(kJetBrainsMono_data),
        static_cast<int>(kJetBrainsMono_size),
        14.0f, &monoCfg);
}

static void pushPrimaryButtonStyle() {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.55f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.66f, 1.00f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.14f, 0.44f, 0.78f, 1.0f));
}

static void popPrimaryButtonStyle() {
    ImGui::PopStyleColor(3);
}

static void drawStatusChip(const char* text, const ImVec4& bgColor, const ImVec4& textColor) {
    ImGui::PushStyleColor(ImGuiCol_Button, bgColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bgColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bgColor);
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 4.0f));
    ImGui::Button(text);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

int main(int argc, char** argv) {
    // AppState is built first so settings (including window pos/size) load
    // before glfwCreateWindow uses them.
    AppState state;
    state.exeDir = fs::weakly_canonical(fs::path(argv[0])).parent_path();
    discoverPaths(state);
    copyToBuffer(state.tagsSource, "myth2_tags");
    copyToBuffer(state.meshTag, "le3e");
    copyToBuffer(state.outputFolder, (fs::path("out") / "le3e").string());
    copyToBuffer(state.pluginOutput, (fs::path("out") / "le3e_plugin").string());
    loadSettings(state);

    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    GLFWwindow* window = glfwCreateWindow(state.windowW, state.windowH, "Myth2ools", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    if (state.windowX != INT_MIN && state.windowY != INT_MIN) {
        glfwSetWindowPos(window, state.windowX, state.windowY);
    }
    if (state.windowMaximized) {
        glfwMaximizeWindow(window);
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    loadEmbeddedFonts(io);
    setupStyle();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window) && state.actionsLoaded && state.actionsDirty) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            state.pendingDiscard = AppState::PENDING_QUIT;
            state.openDiscardPopup = true;
        }
        syncDerivedPaths(state);
        io.ConfigDockingAlwaysTabBar = true;

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGuiWindowFlags hostFlags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoDocking;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##DockHost", nullptr, hostFlags);
        ImGui::PopStyleVar(3);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
        ImGui::BeginChild("##TopBar", ImVec2(0.0f, 84.0f), false, ImGuiWindowFlags_NoScrollbar);
        drawTopBar(state);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
        ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
        if (state.lockDockLayout) {
            dockspaceFlags |= ImGuiDockNodeFlags_NoDockingSplit | ImGuiDockNodeFlags_NoUndocking;
        }
        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockspaceFlags);

        static bool dockLayoutInitialized = false;
        if (!dockLayoutInitialized) {
            dockLayoutInitialized = true;
            ImGuiDockNode* existingNode = ImGui::DockBuilderGetNode(dockspaceId);
            if (existingNode == nullptr) {
                ImGui::DockBuilderRemoveNode(dockspaceId);
                ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

                ImGuiID dockMain = dockspaceId;
                ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.30f, nullptr, &dockMain);
                ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.36f, nullptr, &dockMain);
                ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.34f, nullptr, &dockMain);
                ImGuiID dockRightTop = dockRight;
                ImGuiID dockRightBottom = ImGui::DockBuilderSplitNode(dockRightTop, ImGuiDir_Down, 0.38f, nullptr, &dockRightTop);

                ImGui::DockBuilderDockWindow("Workflow", dockLeft);
                ImGui::DockBuilderDockWindow("Actions###Actions", dockMain);
                ImGui::DockBuilderDockWindow("Command Log", dockBottom);
                ImGui::DockBuilderDockWindow("Map Preview", dockRightTop);
                ImGui::DockBuilderDockWindow("Unit Info", dockRightBottom);
                ImGui::DockBuilderFinish(dockspaceId);
            }
        }

        ImGui::End();

        drawWorkflowPanel(state);
        drawActionsPanel(state);
        drawLogPanel(state);
        drawMapPreviewPanel(state);
        drawUnitInfoPanel(state);

        static int workflowStartupFocusFrames = 3;
        if (workflowStartupFocusFrames > 0) {
            ImGui::SetWindowFocus("Workflow");
            --workflowStartupFocusFrames;
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.09f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Capture window state before tearing down so saveSettings persists the
    // current geometry. If maximized, we still record the underlying
    // non-maximized size/pos so the next launch restores to the same place.
    state.windowMaximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
    if (state.windowMaximized) {
        glfwRestoreWindow(window);
    }
    glfwGetWindowPos(window, &state.windowX, &state.windowY);
    glfwGetWindowSize(window, &state.windowW, &state.windowH);

    releaseMapPreviewTexture(state);
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    saveSettings(state);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
