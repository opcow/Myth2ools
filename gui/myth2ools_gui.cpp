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
#include <mutex>
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

struct AppState {
    fs::path exeDir;
    fs::path scriptDir;
    fs::path toolDir;
    std::array<char, 512> tagsSource{};
    std::array<char, 64> meshTag{};
    std::array<char, 512> outputFolder{};
    std::array<char, 512> pluginOutput{};
    std::array<char, 512> blenderPath{};
    bool overwrite = true;
    bool writeOra = false;
    bool exportNoAnimationSnapshots = false;
    bool editOnBuild = true;
    std::string logSaveStatus;
    std::string settingsStatus;
    ShellRunner runner;
};

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

static void syncDerivedPaths(AppState& s) {
    std::string mesh = s.meshTag.data();
    std::string out = s.outputFolder.data();
    if (out.empty()) {
        copyToBuffer(s.outputFolder, (fs::path("out") / mesh).string());
    }
    std::string plugin = s.pluginOutput.data();
    std::string desiredPlugin = out.empty() ? (fs::path("out") / (mesh + "_plugin")).string()
                                            : (fs::path(out).string() + "_plugin");
    if (plugin.empty()) {
        copyToBuffer(s.pluginOutput, desiredPlugin);
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
    if (!ready) {
        scriptsText += " (missing required files)";
        toolsText += " (missing required files)";
    }
    ImGui::SeparatorText("Resolved Paths");
    ImGui::TextWrapped("Scripts: %s", scriptsText.c_str());
    ImGui::TextWrapped("Tools: %s", toolsText.c_str());

    const bool busy = state.runner.running.load();
    if (busy || !ready) ImGui::BeginDisabled();
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
    if (busy || !ready) ImGui::EndDisabled();

    ImGui::Separator();
    if (!ready) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Could not resolve the Myth2ools scripts/tools layout yet.");
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
    if (ImGui::Button("Save Log")) {
        fs::path savedPath;
        if (saveLogToFile(state, savedPath)) {
            state.logSaveStatus = "Saved log to " + savedPath.string();
        } else {
            state.logSaveStatus = "Failed to save log.";
        }
    }
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
    shouldStickToBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f;
    logBuffer.clear();
    std::lock_guard<std::mutex> lock(state.runner.logMutex);
    for (const std::string& line : state.runner.logLines) {
        logBuffer += line;
        logBuffer.push_back('\n');
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_ReadOnly;
    ImGui::InputTextMultiline("##logtext",
                              logBuffer.data(),
                              logBuffer.size() + 1,
                              ImVec2(-1.0f, -1.0f),
                              flags);
    if (shouldStickToBottom) {
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

        drawWorkflowPanel(state);
        drawLogPanel(state);

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
