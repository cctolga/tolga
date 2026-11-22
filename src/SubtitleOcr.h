#pragma once
// Turns a subtitle track stored as pictures (DVD, Blu-ray PGS) into a text .srt file with the
// system's text recognition (macOS: Vision), so it can go to an Apple TV, which shows only
// text subtitles. Runs in the background; the result is kept, so the same track is converted
// once.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---- the system's text recognition (SubtitleOcrMac.mm, SubtitleOcrNone.cpp)
// This computer can read text from pictures.
bool ocrAvailable();
// A piece of text found in a picture and where it is (pixels, top-left origin).
struct OcrPiece {
    std::string text;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
// Reads the text in a grey picture (dark text on white, `width` x `height`, one byte per
// pixel). `lang`: the subtitle's language as in the file ("eng", "tur"...; may be empty).
// False when nothing could be read.
bool ocrRecognize(const std::vector<uint8_t>& gray, int width, int height, const std::string& lang,
                  std::vector<OcrPiece>& pieces);

// The subtitle's text from the pieces: lines top to bottom, pieces on one line left to right,
// with fixes for what recognition typically gets wrong in subtitles (see the .cpp).
std::string ocrAssemble(const std::vector<uint8_t>& gray, int width, int height, const std::string& lang,
                        std::vector<OcrPiece> pieces);

class SubtitleOcr {
public:
    enum class State { Idle, Running, Done, Failed };
    struct Status {
        State       state = State::Idle;
        float       progress = 0;   // 0..1 while running
        std::string video;          // the video file
        int64_t     trackId = -1;   // mpv's ID of the picture track
        std::string output;         // the .srt (Done)
    };

    explicit SubtitleOcr(std::function<void()> wake) : m_wake(std::move(wake)) {}
    ~SubtitleOcr() { cancel(); }

    // Where the text version of stream `ffIndex` of `video` is kept (under `configDir`).
    static std::string cachePath(const std::string& configDir, const std::string& video, int64_t ffIndex);

    // Starts converting stream `ffIndex` (mpv track `trackId`) of `video` into `output`.
    // Cancels a conversion already running.
    void start(const std::string& video, int64_t trackId, int64_t ffIndex, const std::string& lang,
               const std::string& output);
    void cancel();
    Status status() const;
    // Done / Failed was handled: back to Idle.
    void acknowledge();

private:
    void run(std::string video, int64_t ffIndex, std::string lang, std::string output);

    std::function<void()> m_wake;
    mutable std::mutex m_mutex;
    Status m_status;
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
};
