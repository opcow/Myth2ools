#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl2.h"

#include <GLFW/glfw3.h>

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
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
    std::vector<MeshChoice> availableMeshTags;
    std::string lastScannedTagsSource;
    std::string mapScanStatus;
    std::string logSaveStatus;
    std::string settingsStatus;
    ShellRunner runner;
};

static std::string selectedMeshChoiceDisplay(const AppState& state) {
    std::string currentTag = state.meshTag.data();
    for (const MeshChoice& choice : state.availableMeshTags) {
        if (choice.tag == currentTag) return meshChoiceDisplay(choice);
    }
    return currentTag.empty() ? std::string("(choose map)") : currentTag;
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
    ImGui::TextUnformatted("Myth2ools");
    ImGui::SameLine();
    ImGui::TextDisabled("| ImGui shell");
    ImGui::Separator();
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
    ImGui::Begin("Workflow");
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float halfButtonWidth = (availWidth - spacing) * 0.5f;

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

    bool ready = hasRequiredLayout(state);
    std::string scriptsText = state.scriptDir.string();
    std::string toolsText = state.toolDir.string();
    const std::vector<std::string> workflowIssues = collectWorkflowIssues(state);
    if (!ready) {
        scriptsText += " (missing required files)";
        toolsText += " (missing required files)";
    }
    ImGui::SeparatorText("Resolved Paths");
    ImGui::TextWrapped("Scripts: %s", scriptsText.c_str());
    ImGui::TextWrapped("Tools: %s", toolsText.c_str());

    const bool busy = state.runner.running.load();
    const bool validWorkflow = workflowIssues.empty();
    if (busy || !ready || !validWorkflow) ImGui::BeginDisabled();
    const float thirdButtonWidth = (availWidth - spacing * 2.0f) / 3.0f;
    if (thirdButtonWidth >= 110.0f) {
        if (ImGui::Button("Extract Assets", ImVec2(thirdButtonWidth, 0))) {
            state.runner.start(buildExtractAssetsCommand(state));
        }
        ImGui::SameLine();
        if (ImGui::Button("Build Plugin", ImVec2(thirdButtonWidth, 0))) {
            state.runner.start(buildPluginCommand(state));
        }
        ImGui::SameLine();
        if (ImGui::Button("Create Blend", ImVec2(thirdButtonWidth, 0))) {
            state.runner.start(buildCreateBlendCommand(state));
        }
    } else {
        if (ImGui::Button("Extract Assets", ImVec2(-1.0f, 0))) {
            state.runner.start(buildExtractAssetsCommand(state));
        }
        if (ImGui::Button("Build Plugin", ImVec2(-1.0f, 0))) {
            state.runner.start(buildPluginCommand(state));
        }
        if (ImGui::Button("Create Blend", ImVec2(-1.0f, 0))) {
            state.runner.start(buildCreateBlendCommand(state));
        }
    }
    if (busy || !ready || !validWorkflow) ImGui::EndDisabled();

    ImGui::Separator();
    if (!ready) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Could not resolve the Myth2ools scripts/tools layout yet.");
    }
    for (const std::string& issue : workflowIssues) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.35f, 1.0f), "%s", issue.c_str());
    }
    ImGui::TextWrapped("This first pass is intentionally small: it wraps the existing extract/build/blend workflow so we can grow the editor around real user paths.");
    ImGui::End();
}

static void drawLogPanel(AppState& state) {
    ImGui::Begin("Command Log");
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
    ImGui::Separator();
    static std::string logBuffer;
    bool shouldStickToBottom = false;
    ImGui::BeginChild("logscroll");
    shouldStickToBottom = state.followLog && (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f);
    logBuffer.clear();
    std::lock_guard<std::mutex> lock(state.runner.logMutex);
    for (const std::string& line : state.runner.logLines) {
        logBuffer += line;
        logBuffer.push_back('\n');
    }
    ImGui::TextUnformatted(logBuffer.c_str(), logBuffer.c_str() + logBuffer.size());
    if (state.followLog && shouldStickToBottom) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

static void setupStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
}

int main(int argc, char** argv) {
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    GLFWwindow* window = glfwCreateWindow(1440, 900, "Myth2ools", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    setupStyle();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    AppState state;
    state.exeDir = fs::weakly_canonical(fs::path(argv[0])).parent_path();
    discoverPaths(state);
    copyToBuffer(state.tagsSource, "myth2_tags");
    copyToBuffer(state.meshTag, "le3e");
    copyToBuffer(state.outputFolder, (fs::path("out") / "le3e").string());
    copyToBuffer(state.pluginOutput, (fs::path("out") / "le3e_plugin").string());
    loadSettings(state);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        syncDerivedPaths(state);

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, 44.0f));
        ImGuiWindowFlags rootFlags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("Root", nullptr, rootFlags);
        drawTopBar(state);
        ImGui::End();

        drawLogPanel(state);
        drawWorkflowPanel(state);

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.09f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    saveSettings(state);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
