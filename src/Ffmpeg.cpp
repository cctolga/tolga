#include "Ffmpeg.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

bool isExecutable(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
#ifdef _WIN32
    return true;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

// folder of the running executable
std::string executableDir() {
    std::string path;
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) path.assign(buf, n);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) path = buf.c_str();
#else
    std::error_code ec;
    path = fs::read_symlink("/proc/self/exe", ec).string();
#endif
    if (path.empty()) return {};
    std::error_code cec;
    const fs::path canon = fs::weakly_canonical(path, cec);
    return (cec ? fs::path(path) : canon).parent_path().string();
}

// one argument for the shell
std::string quote(const std::string& s) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
#endif
}

}  // namespace

std::string findProgram(const std::string& name) {
#ifdef _WIN32
    const char sep = ';';
    const std::string exe = name + ".exe";
#else
    const char sep = ':';
    const std::string exe = name;
#endif
    static const std::string ownDir = executableDir();
    if (!ownDir.empty() && isExecutable(ownDir + "/" + exe)) return ownDir + "/" + exe;
    if (const char* path = std::getenv("PATH")) {
        std::stringstream ss(path);
        std::string dir;
        while (std::getline(ss, dir, sep))
            if (!dir.empty() && isExecutable(dir + "/" + exe)) return dir + "/" + exe;
    }
    // apps started from Finder don't get the shell's PATH
    for (const char* dir : {"/opt/homebrew/bin/", "/usr/local/bin/", "/usr/bin/"})
        if (isExecutable(dir + name)) return dir + name;
    return {};
}

std::string findFfmpeg() {
    if (const char* env = std::getenv("TOLGA_FFMPEG"))
        if (isExecutable(env)) return env;
    return findProgram("ffmpeg");
}

double probeDuration(const std::string& path) {
    const std::string ffprobe = findProgram("ffprobe");
    if (ffprobe.empty()) return -1;
    std::string cmd = quote(ffprobe) + " -v error -show_entries format=duration -of csv=p=0 " + quote(path);
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd.exe strips one pair of outer quotes
    FILE* p = _popen(cmd.c_str(), "r");
#else
    cmd += " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return -1;
    char line[128] = {};
    const bool got = std::fgets(line, sizeof line, p) != nullptr;
#ifdef _WIN32
    const int rc = _pclose(p);
#else
    const int rc = pclose(p);
#endif
    if (!got || rc != 0) return -1;
    const double d = std::atof(line);
    return d > 0 ? d : -1;
}
