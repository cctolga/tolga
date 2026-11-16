#include "Thumbnailer.h"

#include "Ffmpeg.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>

#include "Paths.h"
#include "Subprocess.h"

#include <fstream>

#include <filesystem>

namespace {
constexpr int kThumbWidth = 240;
constexpr int kMaxThumbs = 240;
}

Thumbnailer::Thumbnailer(std::function<void()> wake) : m_wake(std::move(wake)) {}

Thumbnailer::~Thumbnailer() { clear(); }

void Thumbnailer::clear() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_gen;
        m_frames.clear();
        m_path.clear();
    }
    if (m_thread.joinable()) m_thread.join();
}

void Thumbnailer::start(const std::string& path, double duration, int displayW, int displayH) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (path == m_path) return;
    }
    clear();
    if (path.empty() || path.find("://") != std::string::npos || duration <= 0 || displayW <= 0 || displayH <= 0)
        return;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_path = path;
        m_width = kThumbWidth;
        m_height = std::max(2, int(std::lround(double(kThumbWidth) * displayH / displayW)) & ~1);
        // one preview every few seconds, at most kMaxThumbs for the whole file
        m_interval = std::max(2.0, duration / kMaxThumbs);
        gen = m_gen;
    }
    m_thread = std::thread([this, path, duration, gen] { run(path, duration, gen); });
}

int Thumbnailer::frameAt(double seconds, std::vector<uint8_t>& rgba, int& width, int& height) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_frames.empty() || m_interval <= 0) return -1;
    int i = int(std::lround(seconds / m_interval));
    i = std::clamp(i, 0, int(m_frames.size()) - 1);
    // only show it if it is close to the asked time (the rest isn't decoded yet)
    if (std::abs(i * m_interval - seconds) > m_interval * 1.5) return -1;
    rgba = m_frames[size_t(i)];
    width = m_width;
    height = m_height;
    return i;
}

void Thumbnailer::run(std::string path, double duration, uint64_t gen) {
    (void)duration;
    const std::string ffmpeg = findFfmpeg();
    if (ffmpeg.empty()) return;
    int w, h;
    double interval;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        w = m_width;
        h = m_height;
        interval = m_interval;
    }
    char vf[160];
    // fps=1/interval keeps one frame per step; only keyframes are decoded (fast)
    std::snprintf(vf, sizeof vf, "fps=1/%.3f,scale=%d:%d:flags=fast_bilinear", interval, w, h);
    const std::vector<std::string> args = {ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error",
                                           "-skip_frame", "nokey", "-threads", "2", "-i", path,
                                           "-an", "-sn", "-dn", "-vf", vf, "-fps_mode", "vfr",
                                           "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"};
    const size_t frameBytes = size_t(w) * size_t(h) * 4;
    auto cancelled = [&] {
        std::lock_guard<std::mutex> lock(m_mutex);
        return gen != m_gen;
    };

    const std::string logPath = utf8String(std::filesystem::temp_directory_path() / "tolga-thumbnails.log");
    tolga_sub::Process proc;
    if (!proc.start(args, logPath)) {
        std::fprintf(stderr, "[thumbs] could not start %s\n", ffmpeg.c_str());
        return;
    }
    std::fprintf(stderr, "[thumbs] making previews (one every %.0f s) with %s\n", interval, ffmpeg.c_str());
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> frame(frameBytes);
    size_t have = 0;
    uint64_t lastWake = 0;
    for (;;) {
        if (cancelled()) {
            proc.terminate();
            break;
        }
        const long n = proc.read(reinterpret_cast<char*>(frame.data()) + have, frameBytes - have, 200);
        if (n == 0) break;
        if (n < 0) continue;
        have += size_t(n);
        if (have == frameBytes) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (gen == m_gen) m_frames.push_back(frame);
            }
            have = 0;
            if (m_wake && ++lastWake % 8 == 0) m_wake();
        }
    }
    proc.wait();
    size_t count;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        count = gen == m_gen ? m_frames.size() : 0;
    }
    if (!cancelled()) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (count == 0) {
            // show why: the last line ffmpeg wrote
            std::string last;
            std::ifstream f(utf8Path(logPath));
            for (std::string line; std::getline(f, line);) last = line;
            std::fprintf(stderr, "[thumbs] no previews; ffmpeg said: %s\n", last.empty() ? "(nothing)" : last.c_str());
        } else {
            std::fprintf(stderr, "[thumbs] %zu previews ready in %.1f s\n", count, secs);
        }
    }
    if (m_wake) m_wake();
}
