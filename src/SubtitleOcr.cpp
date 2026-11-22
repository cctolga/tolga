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

// The picture of one subtitle as dark text on white (`sw` x `sh`, unscaled). Bright letters
// become dark, their dark outline and the transparent parts become white.
bool subtitlePicture(const AVSubtitle& sub, std::vector<uint8_t>& src, int& sw, int& sh) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (unsigned i = 0; i < sub.num_rects; ++i) {
        const AVSubtitleRect* r = sub.rects[i];
        if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 || !r->data[0] || !r->data[1]) continue;
        x0 = std::min(x0, r->x); y0 = std::min(y0, r->y);
        x1 = std::max(x1, r->x + r->w); y1 = std::max(y1, r->y + r->h);
    }
    if (x1 < 0) return false;
    sw = x1 - x0;
    sh = y1 - y0;
    src.assign(size_t(sw) * sh, 255);
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
    return true;
}

// The picture at `scale` (bilinear: smooth edges read better than blocks) with a white margin
// of `pad` around it. Recognition misses or misreads text that is too big for its picture or
// has too little room around it (measured on Korean and Japanese films: "응" read as "00").
void scaledPicture(const std::vector<uint8_t>& src, int sw, int sh, float scale, int pad, std::vector<uint8_t>& out,
                   int& w, int& h) {
    const int dw = std::max(1, int(sw * scale + 0.5f)), dh = std::max(1, int(sh * scale + 0.5f));
    w = dw + 2 * pad;
    h = dh + 2 * pad;
    out.assign(size_t(w) * h, 255);
    const float inv = 1.0f / scale;
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            float v;
            if (scale >= 1.0f) {
                const float fx = (x + 0.5f) * inv - 0.5f, fy = (y + 0.5f) * inv - 0.5f;
                const int ix = std::clamp(int(std::floor(fx)), 0, sw - 1), iy = std::clamp(int(std::floor(fy)), 0, sh - 1);
                const int jx = std::min(ix + 1, sw - 1), jy = std::min(iy + 1, sh - 1);
                const float ax = std::clamp(fx - ix, 0.0f, 1.0f), ay = std::clamp(fy - iy, 0.0f, 1.0f);
                const float top = src[size_t(iy) * sw + ix] * (1 - ax) + src[size_t(iy) * sw + jx] * ax;
                const float bot = src[size_t(jy) * sw + ix] * (1 - ax) + src[size_t(jy) * sw + jx] * ax;
                v = top * (1 - ay) + bot * ay;
            } else {
                // smaller: the average of the source pixels under this one
                const int sx0 = int(x * inv), sx1 = std::min(sw, std::max(sx0 + 1, int((x + 1) * inv)));
                const int sy0 = int(y * inv), sy1 = std::min(sh, std::max(sy0 + 1, int((y + 1) * inv)));
                int sum = 0, n = 0;
                for (int yy = sy0; yy < sy1; ++yy)
                    for (int xx = sx0; xx < sx1; ++xx) { sum += src[size_t(yy) * sw + xx]; ++n; }
                v = n ? float(sum) / n : 255.0f;
            }
            out[size_t(y + pad) * w + x + pad] = uint8_t(v + 0.5f);
        }
}

}  // namespace

namespace {

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
}

// How many plain upright bars the first word of this line is made of, when it is nothing
// else: 1 for a lone capital "I" (English), 2 for "Il" (French); 0 otherwise. Recognition
// often drops such a word or glues it to the next one ("I swerved" -> "swerved",
// "I ever" -> "lever", "Il y a" -> "y a"): bars alone look like stray lines.
// The line is rows fy0..fy1; it is looked at a little above and below too (band0..band1),
// as the recognised box can miss the top of the letters (a "T" seen without its bar is an I).
// `nextIsBar`: the next word also starts with a tall bar ("I lost": that "l" is real).
int leadingBars(const std::vector<uint8_t>& gray, int w, int h, float band0, float band1, bool& nextIsBar) {
    nextIsBar = false;
    const int y0 = std::clamp(int(band0), 0, h - 1), y1 = std::clamp(int(band1), 0, h - 1);
    if (y1 - y0 < 8) return 0;
    auto dark = [&](int x, int y) { return gray[size_t(y) * w + x] < 128; };
    int top = h, bottom = -1;
    for (int y = y0; y <= y1; ++y)
        for (int x = 0; x < w; ++x)
            if (dark(x, y)) { top = std::min(top, y); bottom = std::max(bottom, y); break; }
    if (bottom < 0) return 0;
    const int lineH = bottom - top + 1;
    auto inkColumn = [&](int x, int& ct, int& cb) {
        bool any = false;
        for (int y = top; y <= bottom; ++y)
            if (dark(x, y)) { if (!any) ct = y; cb = y; any = true; }
        return any;
    };
    struct Blob { int x0, x1, top, bottom; };
    int x = 0, ct = 0, cb = 0;
    auto nextBlob = [&](Blob& b, int& gapBefore) {
        const int from = x;
        while (x < w && !inkColumn(x, ct, cb)) ++x;
        gapBefore = x - from;
        if (x >= w) return false;
        b = {x, x, ct, cb};
        while (x < w && inkColumn(x, ct, cb)) { b.top = std::min(b.top, ct); b.bottom = std::max(b.bottom, cb); ++x; }
        b.x1 = x;
        return true;
    };
    // (measured on whole films: a word space is 0.29-0.5 of the line height, the space between
    // letters at most 0.19; an upright I is 0.1 wide, an italic one up to 0.35)
    auto isBar = [&](const Blob& b) { return b.x1 - b.x0 <= lineH * 0.36f && b.bottom - b.top + 1 >= lineH * 0.55f; };
    const float wordGap = lineH * 0.25f;
    Blob first, second, after;
    int gap0 = 0, gap1 = 0, gap2 = 0;
    if (!nextBlob(first, gap0) || !isBar(first) || !nextBlob(second, gap1)) return 0;
    int bars = 0;
    if (gap1 >= wordGap) {
        bars = 1;
        after = second;
    } else if (second.bottom - second.top + 1 <= lineH * 0.35f && second.bottom > first.top + lineH * 0.5f &&
               nextBlob(after, gap2) && gap2 >= wordGap) {
        bars = 1;  // "I, too": a comma right after it
    } else if (isBar(second) && nextBlob(after, gap2) && gap2 >= wordGap) {
        bars = 2;
    } else {
        return 0;
    }
    // the first letter of the next word: an "l" is as tall as the bar or taller, an "e" much lower
    // (letters can touch: only its first few columns count)
    int nTop = h, nBottom = -1;
    for (int c = after.x0; c < std::min(after.x1, after.x0 + std::max(2, first.x1 - first.x0)); ++c)
        if (inkColumn(c, ct, cb)) { nTop = std::min(nTop, ct); nBottom = std::max(nBottom, cb); }
    nextIsBar = nBottom >= 0 && nBottom - nTop + 1 >= (first.bottom - first.top + 1) * 0.95f;
    return bars;
}

// The words of a line as the picture shows them (rows band0..band1): runs of ink split where
// the space between them is a word space (measured on whole films: a word space is 0.29-0.5
// of the line height, the space between letters at most 0.19). `bar`: the word is a lone
// plain upright bar, i.e. a capital "I" (maybe with a comma after it: `comma`).
// `firstI`: what the word's first letter is when it is a thin stroke, for Turkish, where
// recognition mixes up I, İ, i and ı: 'I' tall, 'D' tall with a dot (İ), 'i' short with a
// dot, 'd' short (ı); 0 when it isn't a thin stroke.
// `smallIs`: the same for the word's other letters that are a short thin stroke ('i' / 'd'), in order.
struct InkWord { int x0, x1; bool bar, comma; char firstI = 0; std::string smallIs; };
std::vector<InkWord> inkWords(const std::vector<uint8_t>& gray, int w, int h, float band0, float band1) {
    std::vector<InkWord> words;
    const int y0 = std::clamp(int(band0), 0, h - 1), y1 = std::clamp(int(band1), 0, h - 1);
    if (y1 - y0 < 8) return words;
    auto dark = [&](int x, int y) { return gray[size_t(y) * w + x] < 128; };
    int top = h, bottom = -1;
    for (int y = y0; y <= y1; ++y)
        for (int x = 0; x < w; ++x)
            if (dark(x, y)) { top = std::min(top, y); bottom = std::max(bottom, y); break; }
    if (bottom < 0) return words;
    const float lineH = float(bottom - top + 1);
    struct Blob { int x0, x1, top, bottom; };
    std::vector<Blob> blobs;
    for (int x = 0; x < w; ++x) {
        int ct = -1, cb = -1;
        for (int y = top; y <= bottom; ++y)
            if (dark(x, y)) { if (ct < 0) ct = y; cb = y; }
        if (ct < 0) continue;
        if (!blobs.empty() && blobs.back().x1 == x - 1) {
            Blob& b = blobs.back();
            b.x1 = x; b.top = std::min(b.top, ct); b.bottom = std::max(b.bottom, cb);
        } else {
            blobs.push_back({x, x, ct, cb});
        }
    }
    // (a thin, unbroken stroke: not a "1" with its flag, not a "!" with its dot)
    auto isBar = [&](const Blob& b) {
        if (b.x1 - b.x0 + 1 > lineH * 0.22f || b.bottom - b.top + 1 < lineH * 0.55f) return false;
        const int mid = (b.x0 + b.x1) / 2;
        int inked = 0;
        for (int y = b.top; y <= b.bottom; ++y)
            if (dark(mid, y) || (mid > b.x0 && dark(mid - 1, y)) || (mid < b.x1 && dark(mid + 1, y))) ++inked;
        return inked >= (b.bottom - b.top + 1) * 0.92f;
    };
    auto isDot = [&](const Blob& b) { return b.bottom - b.top + 1 <= lineH * 0.35f && b.bottom > top + lineH * 0.5f; };
    // the longest unbroken vertical stroke of the line: the height of a capital or a tall
    // letter (l, k, d), to tell a capital I from a small i by
    int tallest = 0;
    for (int x = 0; x < w; ++x) {
        int run = 0;
        for (int y = top; y <= bottom; ++y) {
            run = dark(x, y) ? run + 1 : 0;
            tallest = std::max(tallest, run);
        }
    }
    size_t k = 0;
    while (k < blobs.size()) {
        size_t e = k;
        while (e + 1 < blobs.size() && blobs[e + 1].x0 - blobs[e].x1 - 1 < lineH * 0.25f) ++e;
        InkWord word{blobs[k].x0, blobs[e].x1, false, false};
        // a thin stroke, maybe with a dot above it (a gap in its middle column): 'I' tall, 'D' tall
        // with a dot (İ), 'i' short with a dot, 'd' short (ı); 0 for anything else
        auto strokeKind = [&](const Blob& f) -> char {
            if (f.x1 - f.x0 + 1 > lineH * 0.22f) return 0;
            const int mid = (f.x0 + f.x1) / 2;
            auto inkAt = [&](int y) { return dark(mid, y) || (mid > f.x0 && dark(mid - 1, y)) || (mid < f.x1 && dark(mid + 1, y)); };
            std::vector<std::pair<int, int>> runs;  // runs of ink down the middle column
            for (int y = f.top; y <= f.bottom; ++y)
                if (inkAt(y)) {
                    if (!runs.empty() && runs.back().second == y - 1) runs.back().second = y;
                    else runs.push_back({y, y});
                }
            if (runs.empty() || runs.size() > 2) return 0;
            const auto& stem = runs.back();
            const int stemH = stem.second - stem.first + 1;
            const bool dot = runs.size() == 2 && runs[0].second - runs[0].first + 1 < lineH * 0.2f;
            if (runs.size() == 2 && !dot) return 0;
            // (measured: a small i is about 0.73 of a capital, a capital I all of it)
            if (stemH >= tallest * 0.87f) return dot ? 'D' : 'I';
            if (stemH >= tallest * 0.6f) return dot ? 'i' : 'd';
            return 0;
        };
        if (e == k && isBar(blobs[k])) word.bar = true;
        if (e == k + 1 && isBar(blobs[k]) && isDot(blobs[e])) word.bar = word.comma = true;
        word.firstI = strokeKind(blobs[k]);
        for (size_t b = k + 1; b <= e; ++b) {
            const char kind = strokeKind(blobs[b]);
            if (kind == 'i' || kind == 'd') word.smallIs += kind;
        }
        words.push_back(word);
        k = e + 1;
    }
    return words;
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
    if (latinLanguage(lang)) {
        replaceAll(t, "|", "I");  // (elsewhere a bar is a bar: Thai "เด็ด / เด็ดมาก")
        // Cyrillic letters that look like Latin ones ("А long one")
        static const char* look[][2] = {{"А", "A"}, {"В", "B"}, {"Е", "E"}, {"К", "K"}, {"М", "M"}, {"Н", "H"},
                                        {"О", "O"}, {"Р", "P"}, {"С", "C"}, {"Т", "T"}, {"Х", "X"}, {"а", "a"},
                                        {"е", "e"}, {"о", "o"}, {"р", "p"}, {"с", "c"}, {"у", "y"}, {"х", "x"}};
        for (auto& l : look) replaceAll(t, l[0], l[1]);
        replaceAll(t, "н", "H");  // a small Cyrillic n is a small capital H ("нey")
        // the dotless i belongs to Turkish and Azerbaijani only ("Nidavellır")
        if (lang != "tur" && lang != "tr" && lang != "aze" && lang != "az") replaceAll(t, "ı", "i");
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
        const bool english = lang.empty() || lang == "eng" || lang == "en";
        const bool french = lang == "fre" || lang == "fra" || lang == "fr";
        const bool turkic = lang == "tur" || lang == "tr" || lang == "aze" || lang == "az";
        // an italic capital I read as a slash: "/ am", "As / was"
        if (english && word == "/") word = "I";
        // "ll" read as two capital I: "I'II", "AIl", "AII"
        replaceAll(word, "'II", "'ll");
        replaceAll(word, "'Il", "'ll");
        if (word == "AIl" || word == "AII" || word == "AIl." || word == "AII.") word.replace(1, 2, "ll");
        // French "Il" / "Ils" read as two capital I ("II y a"); in English "II" is a number
        if (french && (word == "II" || startsWith(word, "II ") || word == "IIs" || word == "IIS")) {
            word[1] = 'l';
            if (word.size() == 3) word[2] = 's';
        }
        // a capital I among small letters is a small one: "moI", "IcI" -> "moi", "Ici"
        {
            int lowers = 0, others = 0;
            for (size_t c = 1; c < word.size(); ++c) {
                const unsigned char ch = static_cast<unsigned char>(word[c]);
                if (std::islower(ch)) ++lowers;
                else if (std::isupper(ch) && ch != 'I') ++others;
            }
            bool fixed = false;
            if (lowers > 0 && others == 0 && std::isalpha(static_cast<unsigned char>(word[0])))
                for (size_t c = 1; c < word.size(); ++c)
                    if (word[c] == 'I' && (c + 1 >= word.size() || word[c + 1] != '\'')) {
                        // (Turkish and Azerbaijani: the small of I is the dotless ı)
                        if (turkic) word.replace(c, 1, "ı"); else word[c] = 'i';
                        fixed = true;
                    }
            // "IcI" in the middle of a sentence: the first one was small too
            size_t back = out.find_last_not_of(' ');
            if (fixed && word[0] == 'I' && back != std::string::npos && std::strchr(".!?-", out[back]) == nullptr) {
                if (turkic) word.replace(0, 1, "ı"); else word[0] = 'i';
            }
        }
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
    const bool english = lang.empty() || lang == "eng" || lang == "en";
    const bool french = lang == "fre" || lang == "fra" || lang == "fr";
    std::string text;
    for (size_t li = 0; li < lines.size(); ++li) {
        Line& l = lines[li];
        std::sort(l.pieces.begin(), l.pieces.end(), [](const OcrPiece& a, const OcrPiece& b) { return a.x0 < b.x0; });
        const float extra = 0.3f * (l.y1 - l.y0);
        const float band0 = std::max(l.y0 - extra, li > 0 ? lines[li - 1].y1 : 0.0f);
        const float band1 = std::min(l.y1 + extra, li + 1 < lines.size() ? lines[li + 1].y0 : float(height));
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
        if (english) {
            // A lone "I" left out ("Because I was" -> "Because was", "I, too" -> ", too"): the
            // picture has more words than the text, and the extra ones are bars. Put them back
            // where they are, only when that accounts for every missing word.
            const std::vector<InkWord> ink = inkWords(gray, width, height, band0, band1);
            std::vector<std::string> said;
            for (size_t a = 0; a < t.size();) {
                const size_t sp = t.find(' ', a);
                const size_t e = sp == std::string::npos ? t.size() : sp;
                if (e > a) said.push_back(t.substr(a, e - a));
                a = e + 1;
            }
            int missing = int(ink.size()) - int(said.size());
            if (missing > 0) {
                std::vector<std::string> fixed;
                size_t j = 0;
                for (size_t i = 0; i < ink.size(); ++i) {
                    const bool readAsI = j < said.size() && (said[j] == "I" || said[j] == "I," || said[j] == "I.");
                    if (ink[i].bar && missing > 0 && !readAsI) {
                        if (ink[i].comma && j < said.size() && said[j].size() > 1 && std::strchr(",.", said[j][0])) {
                            fixed.push_back("I" + said[j++]);  // ", too" -> "I, too"
                        } else if (ink[i].comma && j < said.size() && (said[j] == "," || said[j] == ".")) {
                            fixed.push_back("I" + said[j++]);
                        } else {
                            fixed.push_back(ink[i].comma ? "I," : "I");
                        }
                        --missing;
                    } else if (j < said.size()) {
                        fixed.push_back(said[j++]);
                    }
                }
                while (j < said.size()) fixed.push_back(said[j++]);
                if (missing == 0) {
                    t.clear();
                    for (const std::string& f : fixed) { if (!t.empty()) t += ' '; t += f; }
                }
            }
        }
        t = fixLine(t, lang);
        // Turkish: the first letter of each word as the picture shows it (İ/I, i/ı), when the
        // picture and the text have the same words
        if (lang == "tur" || lang == "tr" || lang == "aze" || lang == "az") {
            const std::vector<InkWord> ink = inkWords(gray, width, height, band0, band1);
            std::vector<size_t> starts;
            for (size_t a = 0; a < t.size(); ++a)
                if (t[a] != ' ' && (a == 0 || t[a - 1] == ' ')) starts.push_back(a);
            if (ink.size() == starts.size()) {
                // (from the right, so the earlier places stay where they are)
                for (size_t k = starts.size(); k-- > 0;) {
                    size_t a = starts[k];
                    while (a < t.size() && std::strchr("-\"'(", t[a])) ++a;  // "-İyi", "\"İnan"
                    if (a >= t.size() || !ink[k].firstI) continue;
                    size_t len = 0;
                    if (t[a] == 'I' || t[a] == 'i') len = 1;
                    else if (t.compare(a, 2, "İ") == 0 || t.compare(a, 2, "ı") == 0) len = 2;
                    if (!len) continue;
                    const char* letter = ink[k].firstI == 'I' ? "I" : ink[k].firstI == 'D' ? "İ"
                                       : ink[k].firstI == 'i' ? "i" : "ı";
                    t.replace(a, len, letter);
                }
                // the i and ı inside each word ("Hazir" -> "Hazır"), when the word has as many of
                // them as the picture shows short strokes
                for (size_t k = starts.size(); k-- > 0;) {
                    const size_t from = starts[k];
                    size_t end = t.find(' ', from);
                    if (end == std::string::npos) end = t.size();
                    std::vector<std::pair<size_t, size_t>> at;  // byte place and length of each i / ı
                    size_t a = from;
                    while (a < end && std::strchr("-\"'(", t[a])) ++a;
                    a += (a < end && (static_cast<unsigned char>(t[a]) & 0x80)) ? 2 : 1;  // after the first letter
                    for (; a < end; ++a) {
                        if (t[a] == 'i') at.push_back({a, 1});
                        else if (t.compare(a, 2, "ı") == 0) { at.push_back({a, 2}); ++a; }
                    }
                    if (at.empty() || at.size() != ink[k].smallIs.size()) continue;
                    for (size_t q = at.size(); q-- > 0;)
                        t.replace(at[q].first, at[q].second, ink[k].smallIs[q] == 'i' ? "i" : "ı");
                }
            }
        }
        // a word made of bars only, dropped or glued to the next one
        if (english || french) {
            bool nextIsBar = false;
            const int bars = leadingBars(gray, width, height, band0, band1, nextIsBar);
            const bool glued = t.size() > 1 && std::islower(static_cast<unsigned char>(t[1]));
            // something was read for the bars ("I", "il", "[I", "l"): only make it the right word
            const std::string firstWord = t.substr(0, t.find(' '));
            const bool readAsBars = !firstWord.empty() && firstWord.size() <= 3 &&
                                    firstWord.find_first_not_of("Il1|[]i!/") == std::string::npos;
            const bool startsWithI = t[0] == 'I' && (t.size() == 1 || !std::isalpha(static_cast<unsigned char>(t[1])));
            if (bars > 0 && readAsBars) {
                if (english && bars == 1 && firstWord != "I") t = "I" + t.substr(firstWord.size());
                if (french && bars == 2 && firstWord != "Il" && firstWord != "il") t = "Il" + t.substr(firstWord.size());
            } else if (english && bars == 1 && std::strchr(",.", t[0])) {
                t = "I" + t;  // ", too, sought" -> "I, too, sought"
            } else if (english && bars == 1 && !startsWithI && !startsWith(t, "1")) {
                if (glued && t[0] == 'I')
                    t = "I " + t.substr(1);  // "Ilost" -> "I lost"
                else if (glued && t[0] == 'l' && !nextIsBar)
                    t = "I " + t.substr(1);  // "lever met" -> "I ever met" (the I read as an l)
                else
                    t = "I " + t;            // "swerved" -> "I swerved", "lost an eye" -> "I lost an eye"
            } else if (french && bars == 2 && !startsWith(t, "Il ") && t != "Il" && !startsWith(t, "Il.") &&
                       !startsWith(t, "Il,")) {
                t = "Il " + t;  // "y a un vaisseau" -> "Il y a un vaisseau"
            }
        }
        if (!text.empty()) text += '\n';
        text += t;
    }
    // an opening quotation mark not seen: '"Oh, I'm gonna / go in my pants."' read without its first one
    if (!text.empty() && text.front() != '"' && text.back() == '"' &&
        std::count(text.begin(), text.end(), '"') % 2 == 1)
        text.insert(0, "\"");
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
    std::vector<uint8_t> src, gray;
    while (!m_cancel && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == ffIndex && pkt->pts != AV_NOPTS_VALUE) {
            AVSubtitle sub{};
            int got = 0;
            if (avcodec_decode_subtitle2(dec, &sub, &got, pkt) >= 0 && got) {
                const double at = pkt->pts * tb - offset;
                const double start = at + sub.start_display_time / 1000.0;
                // an open subtitle ends where the next one (or an empty clear) begins
                if (!events.empty() && (events.back().end < 0 || events.back().end > start)) events.back().end = start;
                int sw = 0, sh = 0, w = 0, h = 0;
                if (sub.num_rects > 0 && subtitlePicture(sub, src, sw, sh)) {
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
                    // Letters about 50 pixels high read best: DVD subtitles (480 / 576 lines) three
                    // times bigger, Blu-ray ones as they are on a 1080 line picture (a 4K one: half)
                    const int canvas = dec->height > 0 ? dec->height : videoHeight;
                    const float base = canvas > 0 && canvas <= 600 ? 3.0f
                                       : canvas > 0 ? std::clamp(1080.0f / canvas, 0.4f, 2.0f) : 1.0f;
                    std::vector<OcrPiece> pieces;
                    bool found = false;
                    // nothing read: once more smaller, then bigger ("Hi.", "응", "ソー")
                    for (const float f : {1.0f, 0.66f, 1.5f}) {
                        scaledPicture(src, sw, sh, base * f, 48, gray, w, h);
                        if ((found = ocrRecognize(gray, w, h, lang, pieces))) break;
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
