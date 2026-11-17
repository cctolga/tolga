#include "Thumbnailer.h"

#include "Ffmpeg.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#include <cstdio>

#include "Paths.h"
#include "Subprocess.h"

#include <fstream>

#include <filesystem>

namespace {
constexpr int kThumbWidth = 240;
constexpr int kMaxThumbs = 240;
constexpr int kSearchSteps = 8;  // how far frameAt looks for a ready preview
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
        // one slot per preview, empty until it is made (they come in out of order)
        m_frames.assign(size_t(std::max(1.0, std::floor(duration / m_interval))), {});
        gen = m_gen;
    }
    m_thread = std::thread([this, path, duration, gen] { run(path, duration, gen); });
}

int Thumbnailer::frameAt(double seconds, std::vector<uint8_t>& rgba, int& width, int& height) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_frames.empty() || m_interval <= 0) return -1;
    const int n = int(m_frames.size());
    const int want = std::clamp(int(std::lround(seconds / m_interval)), 0, n - 1);
    // The previews come in coarse to fine (see run), so early on the nearest ready one can
    // be a few steps away; further than that it would show the wrong scene.
    for (int d = 0; d <= kSearchSteps; ++d) {
        for (const int i : {want - d, want + d}) {
            if (i < 0 || i >= n || m_frames[size_t(i)].empty()) continue;
            rgba = m_frames[size_t(i)];
            width = m_width;
            height = m_height;
            return i;
        }
    }
    return -1;
}

namespace {

// The order the previews are made in: every 16th first, then the ones between them, and so
// on, so the whole seek bar has rough previews soon and they get denser over time.
std::vector<int> fillOrder(int count) {
    std::vector<int> order;
    std::vector<bool> taken(size_t(count), false);
    for (int step = 16; step >= 1; step /= 2)
        for (int i = 0; i < count; i += step)
            if (!taken[size_t(i)]) {
                taken[size_t(i)] = true;
                order.push_back(i);
            }
    return order;
}

}  // namespace

// One short ffmpeg run per preview: it jumps to the keyframe at or before that time and
// decodes only that one picture. Reading the whole file and decoding every keyframe (the
// old way) costs far more on long films, in CPU and in disk reads.
void Thumbnailer::run(std::string path, double duration, uint64_t gen) {
    const std::string ffmpeg = findFfmpeg();
    if (ffmpeg.empty()) return;
    int w, h, count;
    double interval;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        w = m_width;
        h = m_height;
        interval = m_interval;
        count = int(m_frames.size());
    }
    (void)duration;
    char vf[96];
    std::snprintf(vf, sizeof vf, "scale=%d:%d:flags=fast_bilinear", w, h);
    const size_t frameBytes = size_t(w) * size_t(h) * 4;
    auto cancelled = [&] {
        std::lock_guard<std::mutex> lock(m_mutex);
        return gen != m_gen;
    };
    const std::string logPath = utf8String(std::filesystem::temp_directory_path() / "tolga-thumbnails.log");
    std::fprintf(stderr, "[thumbs] making %d previews (one every %.0f s) with %s\n", count, interval, ffmpeg.c_str());

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> frame(frameBytes);
    int made = 0, tried = 0;
    bool suspended = false;
    for (const int index : fillOrder(count)) {
        if (cancelled()) break;
        // while paused, wait here between two previews
        while (m_paused && !cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (cancelled()) break;

        char at[32];
        std::snprintf(at, sizeof at, "%.3f", index * interval);
        const std::vector<std::string> args = {ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error",
                                               "-threads", "1", "-skip_frame", "nokey", "-noaccurate_seek",
                                               "-ss", at, "-i", path, "-an", "-sn", "-dn", "-frames:v", "1",
                                               "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"};
        tolga_sub::Process proc;
        if (!proc.start(args, logPath)) {
            std::fprintf(stderr, "[thumbs] could not start %s\n", ffmpeg.c_str());
            return;
        }
        proc.setBackground();  // previews wait for everything else: playback, AirPlay, other apps
        size_t have = 0;
        suspended = false;
        for (;;) {
            if (cancelled()) {
                proc.terminate();
                break;
            }
            if (const bool pause = m_paused; pause != suspended) {  // AirPlay started mid-preview
                proc.suspend(pause);
                suspended = pause;
            }
            const long n = proc.read(reinterpret_cast<char*>(frame.data()) + have, frameBytes - have, 200);
            if (n == 0) break;
            if (n < 0) continue;
            have += size_t(n);
            if (have == frameBytes) break;
        }
        proc.wait();
        ++tried;
        if (have == frameBytes) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (gen == m_gen) m_frames[size_t(index)] = frame;
            }
            ++made;
            if (m_wake && made % 8 == 0) m_wake();
        } else if (made == 0 && tried >= 3 && !cancelled()) {
            // three misses before any picture: this file won't give previews; show why
            std::string last;
            std::ifstream f(utf8Path(logPath));
            for (std::string line; std::getline(f, line);) last = line;
            std::fprintf(stderr, "[thumbs] no previews; ffmpeg said: %s\n", last.empty() ? "(nothing)" : last.c_str());
            break;
        }
    }
    if (!cancelled() && made > 0) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[thumbs] %d previews ready in %.1f s\n", made, secs);
    }
    if (m_wake) m_wake();
}
