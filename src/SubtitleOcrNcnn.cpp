// Text recognition on Windows and Linux: PaddleOCR's PP-OCRv5 recognition models (Apache-2.0)
// run by ncnn (BSD-3), built into Tolga. The models aren't: each language group is downloaded
// the first time a subtitle in it is converted (4-8 MB) from this project's releases, checked
// against the SHA-256 below and kept in the settings folder.
//
// Only the recognition model is used: where the text is, the subtitle picture already says
// (one line after another, found here by the rows with ink).
#include "SubtitleOcr.h"
#include "Paths.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <net.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace {

// One file per language group: "TOLGAOCR1\n<param bytes> <model bytes> <dictionary bytes>\n"
// and then the three (ncnn param text, ncnn weights, one character per line).
struct ModelFile { const char* group; const char* sha256; long long size; };
const ModelFile kModels[] = {
    {"latin", "8bf23239e4cdb2f7e53f2518005b591e3d6e6ec33deb470bcd756a7d2a99adc5", 4017897},
    {"en", "027a0b862e0bbd385339762637522e53c594903859c6484a2f34413197a90a95", 3919886},
    {"cjk", "3c76817374b59383d26b717dc4f6a6736eed5092a4e001f997d47b20b7a0e667", 8335654},
    {"korean", "7a802d27cf0f452ddfa249d9e62a88cef9acd404afc543cb8dcb3524157c2df9", 6751117},
    {"thai", "2e6a805281d922d66570fda5137916fec75f1cd54ca1510f8d8ee9509337050b", 3941530},
    {"eslav", "e2353746d442685c29caf488ecf001d9e79ab7686ed55d465b862fed7ff7c1b9", 3939755},
    {"cyrillic", "c1d240bfee593c2e1f71bef3709ef6ad259c6d240c9798c842bbf3c132bbfae6", 4021456},
    {"arabic", "a684979f8605c1d1ee97c4cd74102f73ea1eae5ebea755d6b12a03482ef8efbe", 3996105},
    {"greek", "77f1f262e7ea74d7bb17279bb8a736fd1f307075eb5d771af99b4d747875ffae", 3899755},
    {"devanagari", "5f93cc232a8595900cbb6d22628f2330b9fa09c61b27fa869732cb8d9209b1f0", 3952375},
};
constexpr const char* kModelsUrl = "https://github.com/cctolga/tolga/releases/download/ocr-models-1/";

// The model for a subtitle language (ISO 639-2 as in Matroska, or 639-1); "" = none.
std::string groupFor(std::string lang) {
    for (char& c : lang) c = char(std::tolower(static_cast<unsigned char>(c)));
    static const char* const latin[] = {
        "tur", "tr", "fre", "fra", "fr", "ger", "deu", "de", "spa", "es", "ita", "it", "por", "pt", "dut", "nld", "nl",
        "pol", "pl", "swe", "sv", "dan", "da", "nor", "nob", "nno", "no", "nb", "nn", "fin", "fi", "cze", "ces", "cs",
        "slo", "slk", "sk", "slv", "sl", "hrv", "hr", "hun", "hu", "rum", "ron", "ro", "cat", "ca", "ind", "id", "may",
        "msa", "ms", "aze", "az", "est", "et", "lav", "lv", "lit", "lt", "ice", "isl", "is", "gle", "ga", "wel", "cym",
        "cy", "baq", "eus", "eu", "glg", "gl", "alb", "sqi", "sq", "afr", "af", "tgl", "fil", "tl", "lat", "la", "bos",
        "bs", "scc", "srp-latn", "mlt", "mt", "ltz", "lb", "swa", "sw", "uzb", "uz", "tuk", "tk"};
    static const std::pair<const char*, const char*> other[] = {
        {"eng", "en"}, {"en", "en"}, {"chi", "cjk"}, {"zho", "cjk"}, {"zh", "cjk"}, {"jpn", "cjk"}, {"ja", "cjk"},
        {"kor", "korean"}, {"ko", "korean"}, {"tha", "thai"}, {"th", "thai"}, {"rus", "eslav"}, {"ru", "eslav"},
        {"ukr", "eslav"}, {"uk", "eslav"}, {"bel", "eslav"}, {"be", "eslav"}, {"bul", "cyrillic"}, {"bg", "cyrillic"},
        {"srp", "cyrillic"}, {"sr", "cyrillic"}, {"mac", "cyrillic"}, {"mkd", "cyrillic"}, {"mk", "cyrillic"},
        {"kaz", "cyrillic"}, {"kk", "cyrillic"}, {"kir", "cyrillic"}, {"ky", "cyrillic"}, {"mon", "cyrillic"},
        {"mn", "cyrillic"}, {"tgk", "cyrillic"}, {"tg", "cyrillic"}, {"ara", "arabic"}, {"ar", "arabic"},
        {"per", "arabic"}, {"fas", "arabic"}, {"fa", "arabic"}, {"urd", "arabic"}, {"ur", "arabic"}, {"gre", "greek"},
        {"ell", "greek"}, {"el", "greek"}, {"hin", "devanagari"}, {"hi", "devanagari"}, {"mar", "devanagari"},
        {"mr", "devanagari"}, {"nep", "devanagari"}, {"ne", "devanagari"}};
    if (lang.empty() || lang == "und" || lang == "mul") return "latin";  // unknown: most films are
    for (const char* l : latin)
        if (lang == l) return "latin";
    for (const auto& [code, group] : other)
        if (lang == code) return group;
    return {};
}

// ---- SHA-256 (FIPS 180-4), to check a downloaded model before it is used
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64];
    size_t used = 0;
    uint64_t total = 0;
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const unsigned char* p, size_t n) {
        total += n;
        while (n) {
            const size_t take = std::min(n, sizeof buf - used);
            std::memcpy(buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    std::string hex() {
        const uint64_t bits = total * 8;
        const unsigned char one = 0x80, zero = 0;
        add(&one, 1);
        while (used != 56) add(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        add(len, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
        return out;
    }
};

std::string modelDir() {
    std::string dir;
    if (char* pref = SDL_GetPrefPath("", "Tolga")) {
        dir = std::string(pref) + "ocr-models/";
        SDL_free(pref);
    }
    return dir;
}

bool fileMatches(const std::string& path, const ModelFile& m) {
    std::ifstream in(utf8Path(path), std::ios::binary);
    if (!in) return false;
    Sha256 sha;
    std::vector<unsigned char> chunk(1 << 16);
    long long size = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(chunk.data()), std::streamsize(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        sha.add(chunk.data(), size_t(got));
        size += got;
    }
    return size == m.size && sha.hex() == m.sha256;
}

// curl that trusts the system's certificates (as in OpenSubtitles.cpp: the Linux build carries
// its own curl, which doesn't know where this distribution keeps them)
CURL* newCurl() {
    CURL* c = curl_easy_init();
#if defined(__linux__)
    if (c) {
        static const char* const bundle = []() -> const char* {
            if (const char* env = std::getenv("SSL_CERT_FILE")) return env;
            for (const char* p : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                  "/etc/ssl/ca-bundle.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                                  "/etc/ssl/cert.pem"})
                if (FILE* f = std::fopen(p, "rb")) { std::fclose(f); return p; }
            return nullptr;
        }();
        if (bundle) curl_easy_setopt(c, CURLOPT_CAINFO, bundle);
    }
#endif
    return c;
}

struct Download {
    FILE* out = nullptr;
    const std::function<void(float)>* progress = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    long long expected = 0;
};

size_t writeChunk(char* data, size_t size, size_t n, void* user) {
    return std::fwrite(data, size, n, static_cast<Download*>(user)->out) * size;
}

int onProgress(void* user, curl_off_t, curl_off_t now, curl_off_t, curl_off_t) {
    auto* d = static_cast<Download*>(user);
    if (*d->cancel) return 1;  // stops the transfer
    if (d->progress && d->expected > 0) (*d->progress)(std::clamp(float(now) / float(d->expected), 0.0f, 1.0f));
    return 0;
}

bool download(const ModelFile& m, const std::string& path, const std::function<void(float)>& progress,
              const std::atomic<bool>& cancel, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(utf8Path(path).parent_path(), ec);
    const std::string part = path + ".part";
    Download d;
#ifdef _WIN32
    d.out = _wfopen(utf8Path(part).wstring().c_str(), L"wb");
#else
    d.out = std::fopen(part.c_str(), "wb");
#endif
    if (!d.out) { error = "Couldn't save the reading model."; return false; }
    d.progress = &progress;
    d.cancel = &cancel;
    d.expected = m.size;
    const char* base = std::getenv("TOLGA_OCR_MODELS_URL");  // tests
    const std::string url = std::string(base && *base ? base : kModelsUrl) + m.group + ".tocr";
    std::fprintf(stderr, "[ocr] downloading %s\n", url.c_str());
    CURL* c = newCurl();
    CURLcode rc = CURLE_FAILED_INIT;
    long status = 0;
    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_USERAGENT, "TolgaPlayer");
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);  // give up when it stalls for a minute
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeChunk);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &d);
        rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
    }
    std::fclose(d.out);
    if (cancel) { std::filesystem::remove(utf8Path(part), ec); return false; }
    if (rc != CURLE_OK || status != 200) {
        std::fprintf(stderr, "[ocr] download failed: %s (HTTP %ld)\n", curl_easy_strerror(rc), status);
        std::filesystem::remove(utf8Path(part), ec);
        error = "Couldn't download the reading model. Check the internet connection.";
        return false;
    }
    if (!fileMatches(part, m)) {
        std::fprintf(stderr, "[ocr] the downloaded model doesn't match its checksum\n");
        std::filesystem::remove(utf8Path(part), ec);
        error = "The downloaded reading model was damaged. Try again.";
        return false;
    }
    std::filesystem::rename(utf8Path(part), utf8Path(path), ec);
    if (ec) { error = "Couldn't save the reading model."; return false; }
    return true;
}

// ---- the loaded model (one at a time: the last language group used)
struct Model {
    std::string group;
    std::vector<unsigned char> file;  // ncnn reads the weights from here: kept as long as the net
    ncnn::Net net;
    std::vector<std::string> chars;   // [0] is the CTC blank, the last one a space
};
std::mutex g_modelMutex;
std::shared_ptr<Model> g_model;  // (readers keep their copy: a new language can't pull it away mid-picture)
std::string g_lang;  // the language ocrRecognize is for (set by ocrPrepare)

bool loadModel(const std::string& group, const std::string& path, std::string& error) {
    if (g_model && g_model->group == group) return true;
    auto m = std::make_shared<Model>();
    m->group = group;
    {
        std::ifstream in(utf8Path(path), std::ios::binary);
        m->file.assign(std::istreambuf_iterator<char>(in), {});
    }
    const std::string head = "TOLGAOCR1\n";
    if (m->file.size() < head.size() || std::memcmp(m->file.data(), head.data(), head.size()) != 0) {
        error = "The reading model is damaged.";
        return false;
    }
    size_t at = head.size();
    const size_t eol = std::string(m->file.begin() + long(at), m->file.begin() + long(std::min(m->file.size(), at + 64)))
                           .find('\n');
    if (eol == std::string::npos) { error = "The reading model is damaged."; return false; }
    long long sizes[3] = {0, 0, 0};
    if (std::sscanf(reinterpret_cast<const char*>(m->file.data() + at), "%lld %lld %lld", &sizes[0], &sizes[1], &sizes[2]) != 3) {
        error = "The reading model is damaged.";
        return false;
    }
    at += eol + 1;
    if (sizes[0] < 0 || sizes[1] < 0 || sizes[2] < 0 ||
        at + size_t(sizes[0] + sizes[1] + sizes[2]) > m->file.size()) {
        error = "The reading model is damaged.";
        return false;
    }
    const std::string param(reinterpret_cast<const char*>(m->file.data() + at), size_t(sizes[0]));
    const unsigned char* weights = m->file.data() + at + sizes[0];
    const std::string dict(reinterpret_cast<const char*>(m->file.data() + at + sizes[0] + sizes[1]), size_t(sizes[2]));
    m->net.opt.use_vulkan_compute = false;
    // one thread per picture: several pictures are read at once (ocrThreads), which uses the
    // cores better than splitting one small picture between them
    m->net.opt.num_threads = 1;
    if (m->net.load_param_mem(param.c_str()) != 0 || m->net.load_model(weights) <= 0) {
        error = "The reading model is damaged.";
        return false;
    }
    m->chars.push_back("");
    size_t a = 0;
    while (a < dict.size()) {
        size_t e = dict.find('\n', a);
        if (e == std::string::npos) e = dict.size();
        std::string ch = dict.substr(a, e - a);
        if (!ch.empty() && ch.back() == '\r') ch.pop_back();
        m->chars.push_back(ch);
        a = e + 1;
    }
    m->chars.push_back(" ");
    g_model = std::move(m);
    std::fprintf(stderr, "[ocr] model %s, %zu characters\n", group.c_str(), g_model->chars.size() - 2);
    return true;
}

// One line of the picture, `x0..x1` x `y0..y1`, as the model wants it: 48 pixels high, the
// width in proportion, -1..1 in three equal channels. Smaller: the average of the pixels under
// each new one (smooth, as the model was trained on); bigger: bilinear.
ncnn::Mat lineInput(const std::vector<uint8_t>& gray, int w, int x0, int y0, int x1, int y1) {
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    const int th = 48, tw = std::max(16, int(std::lround(cw * double(th) / ch)));
    ncnn::Mat in(tw, th, 3);
    const float sx = float(cw) / tw, sy = float(ch) / th;
    auto px = [&](int x, int y) { return float(gray[size_t(y0 + y) * w + x0 + x]); };
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            float v;
            if (sy > 1.0f) {
                const int ax = int(x * sx), bx = std::max(ax + 1, std::min(cw, int((x + 1) * sx)));
                const int ay = int(y * sy), by = std::max(ay + 1, std::min(ch, int((y + 1) * sy)));
                float sum = 0;
                for (int yy = ay; yy < by; ++yy)
                    for (int xx = ax; xx < bx; ++xx) sum += px(xx, yy);
                v = sum / float((bx - ax) * (by - ay));
            } else {
                const float fx = (x + 0.5f) * sx - 0.5f, fy = (y + 0.5f) * sy - 0.5f;
                const int ix = std::clamp(int(std::floor(fx)), 0, cw - 1), iy = std::clamp(int(std::floor(fy)), 0, ch - 1);
                const int jx = std::min(ix + 1, cw - 1), jy = std::min(iy + 1, ch - 1);
                const float ax = std::clamp(fx - ix, 0.0f, 1.0f), ay = std::clamp(fy - iy, 0.0f, 1.0f);
                v = (px(ix, iy) * (1 - ax) + px(jx, iy) * ax) * (1 - ay) + (px(ix, jy) * (1 - ax) + px(jx, jy) * ax) * ay;
            }
            v = v / 127.5f - 1.0f;
            for (int c = 0; c < 3; ++c) in.channel(c).row(y)[x] = v;
        }
    return in;
}

}  // namespace

bool ocrAvailable() { return true; }

// every core but one (the movie keeps playing smoothly meanwhile)
int ocrThreads() {
    if (const char* env = std::getenv("TOLGA_OCR_THREADS")) return std::max(1, std::atoi(env));  // tests
    return int(std::max(1u, std::thread::hardware_concurrency() - 1));
}

bool ocrPrepare(const std::string& lang, const std::function<void(float)>& progress, const std::atomic<bool>& cancel,
                std::string& error) {
    const std::string group = groupFor(lang);
    if (group.empty()) {
        error = "Subtitles in this language can't be converted to text yet.";
        return false;
    }
    const ModelFile* file = nullptr;
    for (const ModelFile& m : kModels)
        if (group == m.group) file = &m;
    const std::string dir = modelDir();
    if (!file || dir.empty()) { error = "Couldn't save the reading model."; return false; }
    const std::string path = dir + group + ".tocr";
    std::error_code ec;
    if (!std::filesystem::exists(utf8Path(path), ec) || std::filesystem::file_size(utf8Path(path), ec) != uintmax_t(file->size)) {
        if (!download(*file, path, progress, cancel, error)) return false;
    }
    std::lock_guard<std::mutex> lock(g_modelMutex);
    if (!loadModel(group, path, error)) {
        std::filesystem::remove(utf8Path(path), ec);  // fetched again next time
        return false;
    }
    g_lang = lang;
    return true;
}

bool ocrRecognize(const std::vector<uint8_t>& gray, int width, int height, const std::string&,
                  std::vector<OcrPiece>& pieces) {
    pieces.clear();
    std::shared_ptr<Model> model;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        model = g_model;
    }
    if (!model) return false;  // (an ncnn Net can run several extractors at once)
    // lines: rows with ink, joined across gaps much smaller than a line (the gaps inside letters)
    std::vector<std::pair<int, int>> runs;
    int start = -1;
    for (int y = 0; y <= height; ++y) {
        bool ink = false;
        if (y < height)
            for (int x = 0; x < width && !ink; ++x) ink = gray[size_t(y) * width + x] < 128;
        if (ink && start < 0) start = y;
        if (!ink && start >= 0) { runs.push_back({start, y - 1}); start = -1; }
    }
    int tallest = 0;
    for (const auto& r : runs) tallest = std::max(tallest, r.second - r.first + 1);
    std::vector<std::pair<int, int>> lines;
    for (const auto& r : runs) {
        if (!lines.empty() && r.first - lines.back().second - 1 < 0.1f * tallest) lines.back().second = r.second;
        else lines.push_back(r);
    }
    for (const auto& [ly0, ly1] : lines) {
        if (ly1 - ly0 + 1 < 0.25f * tallest) continue;  // a stray speck
        int x0 = width, x1 = -1;
        for (int y = ly0; y <= ly1; ++y)
            for (int x = 0; x < width; ++x)
                if (gray[size_t(y) * width + x] < 128) { x0 = std::min(x0, x); x1 = std::max(x1, x); }
        if (x1 < 0) continue;
        const int m = 8;
        const int cx0 = std::max(0, x0 - m), cx1 = std::min(width - 1, x1 + m);
        const int cy0 = std::max(0, ly0 - m), cy1 = std::min(height - 1, ly1 + m);
        ncnn::Extractor ex = model->net.create_extractor();
        ex.input("in0", lineInput(gray, width, cx0, cy0, cx1, cy1));
        ncnn::Mat out;
        if (ex.extract("out0", out) != 0) continue;
        // CTC: the best character for each step, repeats and blanks dropped
        std::string text;
        int prev = -1;
        for (int t = 0; t < out.h; ++t) {
            const float* row = out.row(t);
            const int best = int(std::max_element(row, row + out.w) - row);
            if (best != prev && best > 0 && best < int(model->chars.size())) text += model->chars[size_t(best)];
            prev = best;
        }
        // "..." comes out as ".." (two same characters in a row need a blank between them)
        for (size_t a = 0; (a = text.find("..", a)) != std::string::npos;) {
            size_t e = a;
            while (e < text.size() && text[e] == '.') ++e;
            if (e - a == 2) text.insert(a, ".");
            a = e + 1;
        }
        while (!text.empty() && text.back() == ' ') text.pop_back();
        while (!text.empty() && text.front() == ' ') text.erase(0, 1);
        if (text.empty()) continue;
        OcrPiece p;
        p.text = std::move(text);
        p.x0 = float(x0);
        p.x1 = float(x1);
        p.y0 = float(ly0);
        p.y1 = float(ly1);
        pieces.push_back(std::move(p));
    }
    return !pieces.empty();
}
