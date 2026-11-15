#include "DurationCache.h"

#include "Ffmpeg.h"

DurationCache::DurationCache(std::function<void()> wake) : m_wake(std::move(wake)) {
    m_thread = std::thread([this] { worker(); });
}

DurationCache::~DurationCache() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

double DurationCache::get(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_known.find(path);
    if (it != m_known.end()) return it->second < 0 ? -1 : it->second;
    if (path.find("://") != std::string::npos) {  // streams: not probed
        m_known[path] = -1;
        return -1;
    }
    m_known[path] = -2;
    m_queue.push_back(path);
    m_cv.notify_one();
    return -1;
}

void DurationCache::set(const std::string& path, double seconds) {
    if (seconds <= 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_known[path] = seconds;
}

void DurationCache::worker() {
    for (;;) {
        std::string path;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return m_quit || !m_queue.empty(); });
            if (m_quit) return;
            path = m_queue.front();
            m_queue.pop_front();
        }
        const double d = probeDuration(path);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_known[path] < 0) m_known[path] = d;  // the player may have filled it in meanwhile
        }
        if (m_wake) m_wake();
    }
}
