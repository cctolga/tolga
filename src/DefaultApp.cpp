#include "DefaultApp.h"
#include "I18n.h"

#ifndef __APPLE__  // macOS: DefaultAppMac.mm

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#else
#include "Subprocess.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#endif

#include <string>

namespace defaultapp {

#ifdef _WIN32
// ------------------------------------------------------------------ Windows

namespace {
// The installer registers this ProgID and "Tolga Player" under RegisteredApplications;
// the zip version has neither, and Windows can't offer it as a default app.
bool installed() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    const bool ok = RegQueryValueExW(key, L"Tolga Player", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool opensWithUs(const wchar_t* ext) {
    wchar_t exe[MAX_PATH], self[MAX_PATH];
    DWORD n = MAX_PATH;
    if (AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE, ext, L"open", exe, &n) != S_OK) return false;
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return false;
    return lstrcmpiW(exe, self) == 0;
}
}  // namespace

bool available() { return true; }

bool isDefault() { return opensWithUs(L".mkv") && opensWithUs(L".mp4"); }

std::string makeDefault() {
    if (!installed())
        return T("Install Tolga Player with Tolga-Player-Win.exe first: the zip version can't become the default player.");
    // Windows 11 opens the page of this app; Windows 10 opens the general Default apps page
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Tolga%20Player", nullptr, nullptr,
                  SW_SHOWNORMAL);
    return T("In the Settings window that opened, choose Tolga Player for .mkv, .mp4 and the other video types.");
}

#else
// ------------------------------------------------------------------ Linux

namespace fs = std::filesystem;

namespace {
constexpr const char* kDesktopId = "app.tolga.player.desktop";
const char* const kVideoTypes[] = {
    "video/x-matroska", "video/mp4", "video/webm", "video/quicktime", "video/x-msvideo", "video/x-flv",
    "video/mpeg", "video/mp2t", "video/x-ms-wmv", "video/ogg", "video/3gpp", "video/x-m4v", "application/x-matroska"};

std::string findProgram(const char* name) {
    const char* path = std::getenv("PATH");
    std::string dirs = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        const size_t end = dirs.find(':', start);
        const std::string dir = dirs.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            const std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) return full;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

// Runs a program and returns its output (empty if it couldn't run).
std::string run(std::vector<std::string> args, int* exitCode = nullptr) {
    tolga_sub::Process proc;
    if (exitCode) *exitCode = -1;
    if (!proc.start(args)) return {};
    std::string out;
    char buf[512];
    bool done = false;
    for (int waited = 0; waited < 10000;) {
        const long n = proc.read(buf, sizeof buf, 100);
        if (n == 0) { done = true; break; }  // output closed: the program is finishing
        if (n > 0) out.append(buf, size_t(n));
        else waited += 100;
    }
    if (!done) proc.terminate();  // hung for 10 s
    const int rc = proc.wait();
    if (exitCode) *exitCode = rc;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

fs::path dataHome() {
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x == '/') return x;
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "/tmp") / ".local/share";
}

// The file to start: the AppImage itself (its runtime sets $APPIMAGE), else this program.
std::string selfPath() {
    if (const char* a = std::getenv("APPIMAGE"); a && *a) return a;
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
}

// A desktop-entry Exec argument: quoted, with the characters the spec reserves escaped.
std::string quoteExec(const std::string& s) {
    std::string q = "\"";
    for (char c : s) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') q += '\\';
        q += c;
    }
    return q + "\"";
}
}  // namespace

bool available() { return true; }

bool isDefault() {
    const std::string xdg = findProgram("xdg-mime");
    if (xdg.empty()) return false;
    return run({xdg, "query", "default", "video/x-matroska"}) == kDesktopId &&
           run({xdg, "query", "default", "video/mp4"}) == kDesktopId;
}

std::string makeDefault() {
    const std::string exe = selfPath();
    if (exe.empty()) return T("Couldn't find where Tolga Player is.");
    std::error_code ec;
    const fs::path apps = dataHome() / "applications";
    fs::create_directories(apps, ec);

    // the applications-menu entry, pointing at this copy
    std::string mime;
    for (const char* t : kVideoTypes) mime += std::string(t) + ";";
    {
        std::ofstream f(apps / kDesktopId, std::ios::trunc);
        f << "[Desktop Entry]\n"
             "Type=Application\n"
             "Name=Tolga Player\n"
             "GenericName=Video Player\n"
             "Comment=Play videos and send them to an Apple TV\n"
             "Exec=" << quoteExec(exe) << " %F\n"
             "Icon=app.tolga.player\n"
             "Terminal=false\n"
             "Categories=AudioVideo;Video;Player;\n"
             "StartupWMClass=app.tolga.player\n"
             "MimeType=" << mime << "\n";
        if (!f) return TF("Couldn't write {}.", {(apps / kDesktopId).string()});
    }

    // the icon, from the AppImage (or next to the program in a normal install)
    fs::path icon;
    if (const char* appdir = std::getenv("APPDIR"); appdir && *appdir) icon = fs::path(appdir) / "app.tolga.player.png";
    if (icon.empty() || !fs::exists(icon, ec))
        icon = fs::path(exe).parent_path().parent_path() / "share/icons/hicolor/256x256/apps/app.tolga.player.png";
    if (fs::exists(icon, ec)) {
        const fs::path dir = dataHome() / "icons/hicolor/256x256/apps";
        fs::create_directories(dir, ec);
        fs::copy_file(icon, dir / "app.tolga.player.png", fs::copy_options::overwrite_existing, ec);
    }

    const std::string xdg = findProgram("xdg-mime");
    if (xdg.empty()) return T("Tolga Player is in your applications menu now; set it as the default in your system settings.");
    std::vector<std::string> args{xdg, "default", kDesktopId};
    for (const char* t : kVideoTypes) args.emplace_back(t);
    int rc = -1;
    run(args, &rc);
    if (const std::string udd = findProgram("update-desktop-database"); !udd.empty()) run({udd, apps.string()});
    if (rc != 0 || !isDefault())
        return T("Tolga Player is in your applications menu now; your desktop didn't accept it as the default player.");
    return T("Done: double-clicking a video now opens Tolga Player. It is in your applications menu too.");
}
#endif

}  // namespace defaultapp

#endif  // !__APPLE__
