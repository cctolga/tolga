#pragma once
// Runs a helper program (ffmpeg, ffprobe) without a shell: arguments are passed as they are,
// stdout is read through a pipe, stderr goes to a file (or nowhere), stdin is empty.
// On Windows no console window opens and paths with any characters (UTF-8) work.
#include <algorithm>
#include <cerrno>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace tolga_sub {

#ifdef _WIN32
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// one argument as the C runtime of the child splits the command line again
inline std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += c;
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            out += c;
            slashes = 0;
        }
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}
#endif

class Process {
public:
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process() {
        if (running()) {
            terminate();
            wait();
        }
        closeOut();
    }

    // args[0] is the full path of the program. stderrFile "" = discard.
    bool start(const std::vector<std::string>& args, const std::string& stderrFile = "") {
        if (args.empty()) return false;
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 16)) return false;
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);  // only the write end goes to the child
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, 0, nullptr);
        HANDLE err = stderrFile.empty() ? nul
                                        : CreateFileW(widen(stderrFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (err == INVALID_HANDLE_VALUE) err = nul;
        std::wstring cmd;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) cmd += L' ';
            cmd += quoteArg(widen(args[i]));
        }
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = writeEnd;
        si.hStdError = err;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(widen(args[0]).c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(writeEnd);
        if (err != nul) CloseHandle(err);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) {
            CloseHandle(readEnd);
            return false;
        }
        CloseHandle(pi.hThread);
        m_proc = pi.hProcess;
        m_out = readEnd;
        return true;
#else
        int out[2];
        if (pipe(out) != 0) return false;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&fa, out[0]);
        posix_spawn_file_actions_addclose(&fa, out[1]);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, stderrFile.empty() ? "/dev/null" : stderrFile.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0600);
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        pid_t pid = 0;
        const int rc = posix_spawn(&pid, args[0].c_str(), &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(out[1]);
        if (rc != 0) {
            close(out[0]);
            return false;
        }
        m_pid = pid;
        m_out = out[0];
        return true;
#endif
    }

    // Reads stdout: > 0 bytes read, 0 at the end, -1 if nothing came within timeoutMs.
    long read(char* buf, size_t size, int timeoutMs) {
#ifdef _WIN32
        if (!m_out) return 0;
        DWORD waited = 0;
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(m_out, nullptr, 0, nullptr, &avail, nullptr)) return 0;  // child closed it
            if (avail > 0) {
                DWORD got = 0;
                if (!ReadFile(m_out, buf, DWORD(std::min<size_t>(size, avail)), &got, nullptr)) return 0;
                return long(got);
            }
            if (int(waited) >= timeoutMs) return -1;
            Sleep(10);
            waited += 10;
        }
#else
        if (m_out < 0) return 0;
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(m_out, &rs);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        const int r = select(m_out + 1, &rs, nullptr, nullptr, &tv);
        if (r == 0) return -1;
        if (r < 0) return errno == EINTR ? -1 : 0;
        const ssize_t n = ::read(m_out, buf, size);
        if (n < 0) return errno == EINTR || errno == EAGAIN ? -1 : 0;
        return long(n);
#endif
    }

    void terminate() {
#ifdef _WIN32
        if (m_proc) TerminateProcess(m_proc, 1);
#else
        if (m_pid > 0) kill(m_pid, SIGTERM);
#endif
        m_terminated = true;
    }

    // Exit code, or -1 if it was stopped or didn't end normally.
    int wait() {
        closeOut();
        int code = -1;
#ifdef _WIN32
        if (!m_proc) return -1;
        WaitForSingleObject(m_proc, INFINITE);
        DWORD c = 0;
        if (GetExitCodeProcess(m_proc, &c)) code = int(c);
        CloseHandle(m_proc);
        m_proc = nullptr;
#else
        if (m_pid <= 0) return -1;
        int status = 0;
        while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(status)) code = WEXITSTATUS(status);
        m_pid = -1;
#endif
        return m_terminated ? -1 : code;
    }

    bool running() const {
#ifdef _WIN32
        return m_proc != nullptr;
#else
        return m_pid > 0;
#endif
    }

private:
    void closeOut() {
#ifdef _WIN32
        if (m_out) CloseHandle(m_out);
        m_out = nullptr;
#else
        if (m_out >= 0) close(m_out);
        m_out = -1;
#endif
    }
#ifdef _WIN32
    HANDLE m_proc = nullptr;
    HANDLE m_out = nullptr;
#else
    pid_t m_pid = -1;
    int m_out = -1;
#endif
    bool m_terminated = false;
};

}  // namespace tolga_sub
