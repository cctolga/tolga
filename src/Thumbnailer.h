#pragma once
// Small preview images for the seek bar (like IINA): when a local file opens, ffmpeg
// decodes only its keyframes in the background and keeps one small frame every few
// seconds. The UI asks for the frame nearest to the time under the mouse.

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Thumbnailer {
public:
    explicit Thumbnailer(std::function<void()> wake);
    ~Thumbnailer();

    // Starts making previews for `path` (a local file of `duration` seconds whose picture
    // is displayW x displayH). Cancels the previous file's work.
    void start(const std::string& path, double duration, int displayW, int displayH);
    void clear();

    // Copies the preview nearest to `seconds` into `rgba` (width x height, 4 bytes per
    // pixel). Returns its index, or -1 if none is ready yet.
    int frameAt(double seconds, std::vector<uint8_t>& rgba, int& width, int& height) const;
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    void run(std::string path, double duration, uint64_t gen);

    std::function<void()> m_wake;
    mutable std::mutex m_mutex;
    std::vector<std::vector<uint8_t>> m_frames;
    double m_interval = 0;
    int m_width = 0, m_height = 0;
    uint64_t m_gen = 0;
    std::string m_path;
    std::thread m_thread;
};
