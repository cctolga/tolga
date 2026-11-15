#pragma once
// Lengths of playlist files that are not playing (for the playlist view), probed with
// ffprobe on a background thread and kept for the session.

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

class DurationCache {
public:
    explicit DurationCache(std::function<void()> wake);
    ~DurationCache();

    // Seconds, or -1 while unknown (a probe is queued the first time a path is asked for).
    double get(const std::string& path);
    void set(const std::string& path, double seconds);  // e.g. from the player for the playing file

private:
    void worker();

    std::function<void()> m_wake;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<std::string, double> m_known;  // -2 = queued, -1 = failed
    std::deque<std::string> m_queue;
    bool m_quit = false;
    std::thread m_thread;
};
