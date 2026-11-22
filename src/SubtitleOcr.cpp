#include "SubtitleOcr.h"
#include "Paths.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace {

struct Event {
    double start = 0, end = -1;  // seconds from the start of the video; end < 0: until the next one
    std::string text;
};

std::string srtTime(double s) {
    if (s < 0) s = 0;
    const long long ms = (long long)std::llround(s * 1000.0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60,
                  ms % 1000);
    return buf;
}

// The picture of one subtitle as dark text on white, made bigger for small (DVD) pictures:
// text recognition reads letters a few dozen pixels high best. Bright letters become dark,
// their dark outline and the transparent parts become white.
bool subtitlePicture(const AVSubtitle& sub, int videoHeight, std::vector<uint8_t>& out, int& w, int& h) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (unsigned i = 0; i < sub.num_rects; ++i) {
        const AVSubtitleRect* r = sub.rects[i];
        if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 || !r->data[0] || !r->data[1]) continue;
        x0 = std::min(x0, r->x); y0 = std::min(y0, r->y);
        x1 = std::max(x1, r->x + r->w); y1 = std::max(y1, r->y + r->h);
    }
    if (x1 < 0) return false;
    const int sw = x1 - x0, sh = y1 - y0;
    std::vector<uint8_t> src(size_t(sw) * sh, 255);
    for (unsigned i = 0; i < sub.num_rects; ++i) {
        const AVSubtitleRect* r = sub.rects[i];
        if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 || !r->data[0] || !r->data[1]) continue;
        const uint32_t* pal = reinterpret_cast<const uint32_t*>(r->data[1]);  // 0xAARRGGBB
        for (int y = 0; y < r->h; ++y)
            for (int x = 0; x < r->w; ++x) {
                const uint32_t c = pal[r->data[0][size_t(y) * r->linesize[0] + x]];
                const int a = int(c >> 24), red = int(c >> 16 & 255), g = int(c >> 8 & 255), b = int(c & 255);
                const int lum = (red * 299 + g * 587 + b * 114) / 1000;
                src[size_t(y + r->y - y0) * sw + (x + r->x - x0)] = uint8_t(255 - a * lum / 255);
            }
    }
    const int scale = (videoHeight > 0 && videoHeight <= 600) || sh < 60 ? 3 : 1;
    const int pad = 24;
    w = sw * scale + 2 * pad;
    h = sh * scale + 2 * pad;
    out.assign(size_t(w) * h, 255);
    for (int y = 0; y < sh * scale; ++y)
        for (int x = 0; x < sw * scale; ++x) {
            // bilinear: smooth edges read better than blocks
            const float fx = (x + 0.5f) / scale - 0.5f, fy = (y + 0.5f) / scale - 0.5f;
            const int ix = std::clamp(int(std::floor(fx)), 0, sw - 1), iy = std::clamp(int(std::floor(fy)), 0, sh - 1);
            const int jx = std::min(ix + 1, sw - 1), jy = std::min(iy + 1, sh - 1);
            const float ax = std::clamp(fx - ix, 0.0f, 1.0f), ay = std::clamp(fy - iy, 0.0f, 1.0f);
            const float top = src[size_t(iy) * sw + ix] * (1 - ax) + src[size_t(iy) * sw + jx] * ax;
            const float bot = src[size_t(jy) * sw + ix] * (1 - ax) + src[size_t(jy) * sw + jx] * ax;
            out[size_t(y + pad) * w + x + pad] = uint8_t(top * (1 - ay) + bot * ay + 0.5f);
        }
    return true;
}

}  // namespace

namespace {

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
}

// Is the first thing on this line (rows y0..y1) a lone capital "I", i.e. a plain upright bar
// followed by a word space? Recognition often drops it or glues it to the next word
// ("I swerved" -> "swerved", "I ever" -> "lever"): a bar alone looks like a stray line.
// `nextIsBar`: the word after it also starts with a tall thin bar ("I lost": that "l" is real).
bool startsWithLoneI(const std::vector<uint8_t>& gray, int w, int h, float fy0, float fy1, bool& nextIsBar) {
    nextIsBar = false;
    const int y0 = std::clamp(int(fy0), 0, h - 1), y1 = std::clamp(int(fy1), 0, h - 1);
    if (y1 - y0 < 8) return false;
    auto dark = [&](int x, int y) { return gray[size_t(y) * w + x] < 128; };
    int top = h, bottom = -1;
    for (int y = y0; y <= y1; ++y)
        for (int x = 0; x < w; ++x)
            if (dark(x, y)) { top = std::min(top, y); bottom = std::max(bottom, y); break; }
    if (bottom < 0) return false;
    const int lineH = bottom - top + 1;
    auto inkColumn = [&](int x, int& ct, int& cb) {
        bool any = false;
        for (int y = top; y <= bottom; ++y)
            if (dark(x, y)) { if (!any) ct = y; cb = y; any = true; }
        return any;
    };
    int x = 0, ct = 0, cb = 0;
    while (x < w && !inkColumn(x, ct, cb)) ++x;
    if (x >= w) return false;
    int blobTop = ct, blobBottom = cb;
    const int start = x;
    while (x < w && inkColumn(x, ct, cb)) { blobTop = std::min(blobTop, ct); blobBottom = std::max(blobBottom, cb); ++x; }
    const int blobW = x - start, blobH = blobBottom - blobTop + 1;
    const int gapStart = x;
    while (x < w && !inkColumn(x, ct, cb)) ++x;
    const int gap = x - gapStart;
    // (measured on a whole film: a word space is 0.29-0.5 of the line height, the space between
    // letters at most 0.19; an upright I is 0.1 wide, an italic one up to 0.35)
    if (!(x < w && blobW <= lineH * 0.36f && blobH >= lineH * 0.55f && gap >= lineH * 0.25f)) return false;
    // the first letter of the next word: an "l" is as tall as the I or taller, an "e" much lower
    int nTop = h, nBottom = -1;
    const int nStart = x;
    while (x < w && inkColumn(x, ct, cb)) { nTop = std::min(nTop, ct); nBottom = std::max(nBottom, cb); ++x; }
    // (letters can touch: only the first few columns count)
    nTop = h; nBottom = -1;
    for (int c = nStart; c < std::min(x, nStart + std::max(2, blobW)); ++c)
        if (inkColumn(c, ct, cb)) { nTop = std::min(nTop, ct); nBottom = std::max(nBottom, cb); }
    nextIsBar = nBottom >= 0 && nBottom - nTop + 1 >= blobH * 0.95f;
    return true;
}

bool latinLanguage(const std::string& lang) {
    static const char* other[] = {"rus", "ru", "ukr", "uk", "bul", "bg", "srp", "sr", "mac", "mkd", "mk", "bel", "be",
                                  "kaz", "kk", "gre", "ell", "el", "chi", "zho", "zh", "jpn", "ja", "kor", "ko",
                                  "ara", "ar", "heb", "he", "tha", "th", "hin", "hi"};
    for (const char* o : other)
        if (lang == o) return false;
    return true;
}

// Word by word fixes for one line
std::string fixLine(std::string t, const std::string& lang) {
    replaceAll(t, "|", "I");
    if (latinLanguage(lang)) {
        // Cyrillic letters that look like Latin ones ("А long one")
        static const char* look[][2] = {{"А", "A"}, {"В", "B"}, {"Е", "E"}, {"К", "K"}, {"М", "M"}, {"Н", "H"},
                                        {"О", "O"}, {"Р", "P"}, {"С", "C"}, {"Т", "T"}, {"Х", "X"}, {"а", "a"},
                                        {"е", "e"}, {"о", "o"}, {"р", "p"}, {"с", "c"}, {"у", "y"}, {"х", "x"}};
        for (auto& l : look) replaceAll(t, l[0], l[1]);
    }
    // word by word (keeping the punctuation around each word)
    std::string out;
    size_t i = 0;
    while (i < t.size()) {
        if (t[i] == ' ') { out += ' '; ++i; continue; }
        size_t j = t.find(' ', i);
        if (j == std::string::npos) j = t.size();
        std::string word = t.substr(i, j - i);
        const bool first = i == 0;
        // an italic capital I read as a slash: "/ am", "As / was"
        if (word == "/") word = "I";
        // "ll" read as two capital I: "I'II", "AIl", "AII"
        replaceAll(word, "'II", "'ll");
        replaceAll(word, "'Il", "'ll");
        if (word == "AIl" || word == "AII" || word == "AIl." || word == "AII.") word.replace(1, 2, "ll");
        const bool english = lang.empty() || lang == "eng" || lang == "en";
        if (english) {
            // "I'll" without its apostrophe at the start of a line or after a dash
            if ((first || out.empty() || out.back() == '-') && word == "Ill") word = "I'll";
            if (word.size() > 1 && word[0] == '-' && word.substr(1) == "Ill") word = "-I'll";
            // contractions without their apostrophe
            static const char* fix[][2] = {{"cant", "can't"}, {"dont", "don't"}, {"didnt", "didn't"},
                                           {"doesnt", "doesn't"}, {"isnt", "isn't"}, {"wasnt", "wasn't"},
                                           {"werent", "weren't"}, {"arent", "aren't"}, {"wouldnt", "wouldn't"},
                                           {"couldnt", "couldn't"}, {"shouldnt", "shouldn't"},
                                           {"havent", "haven't"}, {"hasnt", "hasn't"}, {"youre", "you're"},
                                           {"theyre", "they're"}, {"Dont", "Don't"}, {"Cant", "Can't"},
                                           {"Didnt", "Didn't"}, {"Youre", "You're"}, {"Theyre", "They're"}};
            size_t a = 0, b = word.size();
            while (a < b && !std::isalpha(static_cast<unsigned char>(word[a]))) ++a;
            while (b > a && !std::isalpha(static_cast<unsigned char>(word[b - 1]))) --b;
            const std::string core = word.substr(a, b - a);
            for (auto& f : fix)
                if (core == f[0]) { word = word.substr(0, a) + f[1] + word.substr(b); break; }
        }
        out += word;
        i = j;
    }
    return out;
}

}  // namespace

std::string ocrAssemble(const std::vector<uint8_t>& gray, int width, int height, const std::string& lang,
                        std::vector<OcrPiece> pieces) {
    // lines: pieces that share most of their height are on one line (an italic part is a
    // piece of its own, a little higher or lower)
    std::sort(pieces.begin(), pieces.end(), [](const OcrPiece& a, const OcrPiece& b) { return a.y0 + a.y1 < b.y0 + b.y1; });
    struct Line { float y0, y1; std::vector<OcrPiece> pieces; };
    std::vector<Line> lines;
    for (OcrPiece& p : pieces) {
        Line* on = nullptr;
        for (Line& l : lines) {
            const float overlap = std::min(l.y1, p.y1) - std::max(l.y0, p.y0);
            if (overlap > 0.5f * std::min(l.y1 - l.y0, p.y1 - p.y0)) { on = &l; break; }
        }
        if (!on) { lines.push_back({p.y0, p.y1, {}}); on = &lines.back(); }
        on->y0 = std::min(on->y0, p.y0);
        on->y1 = std::max(on->y1, p.y1);
        on->pieces.push_back(std::move(p));
    }
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.y0 + a.y1 < b.y0 + b.y1; });
    std::string text;
    for (Line& l : lines) {
        std::sort(l.pieces.begin(), l.pieces.end(), [](const OcrPiece& a, const OcrPiece& b) { return a.x0 < b.x0; });
        std::string t;
        for (const OcrPiece& p : l.pieces) {
            std::string piece = p.text;
            while (!piece.empty() && piece.back() == ' ') piece.pop_back();
            while (!piece.empty() && piece.front() == ' ') piece.erase(0, 1);
            if (piece.empty()) continue;
            if (!t.empty()) t += ' ';
            t += piece;
        }
        if (t.empty()) continue;
        t = fixLine(t, lang);
        bool nextIsBar = false;
        if (latinLanguage(lang) && !startsWith(t, "I ") && t != "I" && !startsWith(t, "I.") && !startsWith(t, "1") &&
            startsWithLoneI(gray, width, height, l.y0, l.y1, nextIsBar)) {
            const bool glued = t.size() > 1 && std::islower(static_cast<unsigned char>(t[1]));
            if (glued && t[0] == 'I')
                t = "I " + t.substr(1);  // "Ilost" -> "I lost"
            else if (glued && t[0] == 'l' && !nextIsBar)
                t = "I " + t.substr(1);  // "lever met" -> "I ever met" (the I read as an l)
            else
                t = "I " + t;            // "swerved" -> "I swerved", "lost an eye" -> "I lost an eye"
        }
        if (!text.empty()) text += '\n';
        text += t;
    }
    return text;
}

std::string SubtitleOcr::cachePath(const std::string& configDir, const std::string& video, int64_t ffIndex) {
    // the file's name, size and time: a changed or another file with the same name gets its own
    std::error_code ec;
    const auto size = std::filesystem::file_size(utf8Path(video), ec);
    const auto time = std::filesystem::last_write_time(utf8Path(video), ec).time_since_epoch().count();
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    // (libc++ keeps file times in 128 bits, which std::to_string doesn't take)
    const std::string key = video + "|" + std::to_string((unsigned long long)(ec ? 0 : size)) + "|" +
                            std::to_string((long long)time) + "|" +
                            std::to_string(ffIndex);
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
    char name[40];
    std::snprintf(name, sizeof name, "%016llx.srt", (unsigned long long)h);
    return configDir + "subtitle-text/" + name;
}

void SubtitleOcr::start(const std::string& video, int64_t trackId, int64_t ffIndex, const std::string& lang,
                        const std::string& output) {
    cancel();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = Status{};
        m_status.state = State::Running;
        m_status.video = video;
        m_status.trackId = trackId;
        m_status.output = output;
    }
    m_cancel = false;
    m_thread = std::thread(&SubtitleOcr::run, this, video, ffIndex, lang, output);
}

void SubtitleOcr::cancel() {
    m_cancel = true;
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_status.state == State::Running) m_status = Status{};
}

SubtitleOcr::Status SubtitleOcr::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void SubtitleOcr::acknowledge() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_status.state == State::Done || m_status.state == State::Failed) m_status = Status{};
}

void SubtitleOcr::run(std::string video, int64_t ffIndex, std::string lang, std::string output) {
    auto finish = [&](bool ok, const char* why) {
        if (!ok) std::fprintf(stderr, "[ocr] %s\n", why);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_cancel) m_status.state = ok ? State::Done : State::Failed;
        if (m_wake) m_wake();
    };

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, video.c_str(), nullptr, nullptr) < 0) return finish(false, "can't open the video");
    struct Closer { AVFormatContext** f; ~Closer() { avformat_close_input(f); } } closeFmt{&fmt};
    if (ffIndex < 0 || ffIndex >= int64_t(fmt->nb_streams)) return finish(false, "no such stream");
    AVStream* st = fmt->streams[ffIndex];
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        fmt->streams[i]->discard = int64_t(i) == ffIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;

    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return finish(false, "no decoder for the subtitles");
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    struct DecCloser { AVCodecContext** c; ~DecCloser() { avcodec_free_context(c); } } closeDec{&dec};
    avcodec_parameters_to_context(dec, st->codecpar);
    dec->pkt_timebase = st->time_base;
    if (avcodec_open2(dec, codec, nullptr) < 0) return finish(false, "can't open the subtitle decoder");

    int videoHeight = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !videoHeight)
            videoHeight = fmt->streams[i]->codecpar->height;
    if (dec->height > 0) videoHeight = dec->height;  // the subtitle's own canvas (DVD: 480 / 576)

    const double tb = av_q2d(st->time_base);
    const double offset = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time / double(AV_TIME_BASE) : 0.0;
    const int64_t fileSize = fmt->pb ? avio_size(fmt->pb) : -1;

    std::vector<Event> events;
    int read = 0, recognized = 0;
    AVPacket* pkt = av_packet_alloc();
    struct PktFree { AVPacket** p; ~PktFree() { av_packet_free(p); } } freePkt{&pkt};
    std::vector<uint8_t> gray;
    while (!m_cancel && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == ffIndex && pkt->pts != AV_NOPTS_VALUE) {
            AVSubtitle sub{};
            int got = 0;
            if (avcodec_decode_subtitle2(dec, &sub, &got, pkt) >= 0 && got) {
                const double at = pkt->pts * tb - offset;
                const double start = at + sub.start_display_time / 1000.0;
                // an open subtitle ends where the next one (or an empty clear) begins
                if (!events.empty() && (events.back().end < 0 || events.back().end > start)) events.back().end = start;
                int w = 0, h = 0;
                if (sub.num_rects > 0 && subtitlePicture(sub, videoHeight, gray, w, h)) {
                    ++read;
                    Event ev;
                    ev.start = start;
                    // the end, when the subtitle says it (else: the next one's start); "forever"
                    // and similar nonsense counts as unknown
                    constexpr double kLongest = 60.0;
                    if (sub.end_display_time > sub.start_display_time &&
                        (sub.end_display_time - sub.start_display_time) / 1000.0 < kLongest)
                        ev.end = at + sub.end_display_time / 1000.0;
                    else if (pkt->duration > 0 && pkt->duration * tb < kLongest)
                        ev.end = start + pkt->duration * tb;
                    std::vector<OcrPiece> pieces;
                    bool found = ocrRecognize(gray, w, h, lang, pieces);
                    if (!found && w > 200 && h > 100) {
                        // big lettering is sometimes not found at all ("Hi."): once more at half size
                        std::vector<uint8_t> half(size_t(w / 2) * (h / 2));
                        for (int y = 0; y < h / 2; ++y)
                            for (int x = 0; x < w / 2; ++x)
                                half[size_t(y) * (w / 2) + x] = uint8_t(
                                    (gray[size_t(2 * y) * w + 2 * x] + gray[size_t(2 * y) * w + 2 * x + 1] +
                                     gray[size_t(2 * y + 1) * w + 2 * x] + gray[size_t(2 * y + 1) * w + 2 * x + 1] + 2) / 4);
                        if ((found = ocrRecognize(half, w / 2, h / 2, lang, pieces)))
                            for (OcrPiece& p : pieces) { p.x0 *= 2; p.x1 *= 2; p.y0 *= 2; p.y1 *= 2; }
                    }
                    if (found) {
                        ev.text = ocrAssemble(gray, w, h, lang, std::move(pieces));
                        if (!ev.text.empty()) {
                            ++recognized;
                            events.push_back(std::move(ev));
                        }
                    }
                }
                avsubtitle_free(&sub);
            }
        }
        av_packet_unref(pkt);
        if (fileSize > 0) {
            std::lock_guard<std::mutex> lock(m_mutex);
            const float p = float(double(avio_tell(fmt->pb)) / double(fileSize));
            if (p - m_status.progress > 0.01f || p < m_status.progress) {
                m_status.progress = std::clamp(p, 0.0f, 1.0f);
                if (m_wake) m_wake();
            }
        }
    }
    if (m_cancel) return;
    if (!events.empty() && events.back().end < 0) events.back().end = events.back().start + 4.0;
    std::fprintf(stderr, "[ocr] %d picture(s), %d read as text\n", read, recognized);
    if (events.empty()) return finish(false, "no text found");

    // one entry per subtitle; the same text repeated back to back is one longer entry
    std::vector<Event> merged;
    for (Event& e : events) {
        if (e.end <= e.start) e.end = e.start + 2.0;
        if (!merged.empty() && merged.back().text == e.text && e.start - merged.back().end < 0.1)
            merged.back().end = e.end;
        else
            merged.push_back(std::move(e));
    }
    std::error_code ec;
    std::filesystem::create_directories(utf8Path(output).parent_path(), ec);
    const std::string tmp = output + ".tmp";
    {
        std::ofstream out(utf8Path(tmp), std::ios::binary);
        for (size_t i = 0; i < merged.size(); ++i)
            out << (i + 1) << "\n" << srtTime(merged[i].start) << " --> " << srtTime(merged[i].end) << "\n"
                << merged[i].text << "\n\n";
        if (!out) return finish(false, "can't write the text subtitles");
    }
    std::filesystem::rename(utf8Path(tmp), utf8Path(output), ec);
    if (ec) return finish(false, "can't save the text subtitles");
    finish(true, nullptr);
}
