#include "OpenSubtitles.h"
#include "Paths.h"
#include "I18n.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kUserAgent = "Tolga v0.1";
constexpr const char* kDefaultApi = "https://api.opensubtitles.com/api/v1";

// Tolga's own OpenSubtitles API key, used by everyone (a user's own key under Account... wins).
// It comes from the build (opensubtitles.key file or -DTOLGA_OPENSUBTITLES_KEY=...), so it is
// in the official builds but not in the public source.
#ifndef TOLGA_OPENSUBTITLES_KEY
#define TOLGA_OPENSUBTITLES_KEY ""
#endif
constexpr const char* kBuiltInKey = TOLGA_OPENSUBTITLES_KEY;  // never put a key in the source

size_t collect(char* data, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string escape(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { std::snprintf(buf, sizeof buf, "%%%02X", c); out += buf; }
    }
    return out;
}

std::string safeFileName(std::string s) {
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|')
            c = '_';
    return s;
}

// Short message from an API error body ({"message": "..."} or {"errors": [...]}).
std::string apiMessage(const std::string& body) {
    try {
        const json j = json::parse(body);
        if (j.contains("message") && j["message"].is_string()) return j["message"];
        if (j.contains("errors") && j["errors"].is_array() && !j["errors"].empty() && j["errors"][0].is_string())
            return j["errors"][0];
    } catch (...) {
    }
    return {};
}

}  // namespace

// ================================================================== setup

OpenSubtitles::OpenSubtitles(std::string configDir, std::function<void()> wake)
    : m_configDir(std::move(configDir)), m_wake(std::move(wake)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    m_settingsPath = m_configDir + "opensubtitles.json";
    m_settings.builtInKey = kBuiltInKey[0] != 0;
    try {
        std::ifstream in(utf8Path(m_settingsPath));
        if (in) {
            const json j = json::parse(in);
            m_settings.apiKey = j.value("api_key", "");
            m_settings.username = j.value("username", "");
            m_settings.password = j.value("password", "");
            m_settings.languages = j.value("languages", m_settings.languages);
        }
    } catch (...) {
    }
    m_thread = std::thread([this] { worker(); });
}

OpenSubtitles::~OpenSubtitles() {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_quit = true;
    }
    m_jobCv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    curl_global_cleanup();
}

OpenSubtitlesSettings OpenSubtitles::settings() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_settings;
}

void OpenSubtitles::saveSettings(const OpenSubtitlesSettings& s) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool accountChanged = s.username != m_settings.username || s.password != m_settings.password ||
                                    s.apiKey != m_settings.apiKey;
        m_settings = s;
        m_settings.builtInKey = kBuiltInKey[0] != 0;
        if (accountChanged) {
            m_token.clear();
            m_baseUrl.clear();
        }
    }
    const json j = {{"api_key", s.apiKey}, {"username", s.username}, {"password", s.password},
                    {"languages", s.languages}};
    {
        std::ofstream out(utf8Path(m_settingsPath), std::ios::trunc);
#ifndef _WIN32
        chmod(m_settingsPath.c_str(), 0600);  // holds the account password
#endif
        out << j.dump(2) << "\n";
    }
    if (m_wake) m_wake();
}

OpenSubtitlesStatus OpenSubtitles::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

std::string OpenSubtitles::takeDownloaded(std::string* language) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string p;
    std::swap(p, m_status.downloadedPath);
    if (language) *language = m_status.downloadedLanguage;
    return p;
}

void OpenSubtitles::setStatus(const std::function<void(OpenSubtitlesStatus&)>& f) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        f(m_status);
    }
    if (m_wake) m_wake();
}

void OpenSubtitles::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_jobs.push_back(std::move(job));
    }
    m_jobCv.notify_one();
}

void OpenSubtitles::worker() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait(lock, [&] { return m_quit || !m_jobs.empty(); });
            if (m_quit) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        job();
    }
}

// ================================================================== helpers

std::string OpenSubtitles::queryFromFileName(const std::string& path) {
    std::string name = utf8String(utf8Path(path).stem());
    name = std::regex_replace(name, std::regex(R"(\[[^\]]*\]|\([^)]*\))"), " ");
    for (char& c : name)
        if (c == '.' || c == '_') c = ' ';
    // keep everything up to (and including) the year, or up to the first quality/release tag
    std::smatch m;
    if (std::regex_search(name, m, std::regex(R"(\b(19|20)\d\d\b)"))) {
        name = name.substr(0, size_t(m.position(0) + m.length(0)));
    } else if (std::regex_search(name, m, std::regex(
                   R"(\b(2160p|1080p|720p|480p|4k|bluray|blu ray|brrip|bdrip|web dl|webrip|web|hdtv|dvdrip|x264|x265|h264|h265|hevc|hdr|remux|s\d\d ?e\d\d)\b)",
                   std::regex::icase))) {
        if (std::regex_search(name, m, std::regex(R"(s\d\d ?e\d\d)", std::regex::icase)))
            name = name.substr(0, size_t(m.position(0) + m.length(0)));  // keep S01E02
        else
            name = name.substr(0, size_t(m.position(0)));
    }
    name = std::regex_replace(name, std::regex(R"(\s+)"), " ");
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    return name;
}

std::string OpenSubtitles::movieHash(const std::string& path) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(f.tellg());
    if (size < 65536) return {};
    uint64_t hash = size;
    auto addChunk = [&](uint64_t offset) {
        f.seekg(std::streamoff(offset));
        std::vector<unsigned char> buf(65536);
        f.read(reinterpret_cast<char*>(buf.data()), 65536);
        for (size_t i = 0; i < 65536; i += 8) {
            uint64_t v = 0;
            for (int b = 7; b >= 0; --b) v = (v << 8) | buf[i + size_t(b)];  // little endian
            hash += v;
        }
    };
    addChunk(0);
    addChunk(size - 65536);
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(hash));
    return out;
}

namespace {
// curl handle that trusts the system's certificates. The Linux build carries its own curl, which
// doesn't know where this distribution keeps them, so look in the usual places.
CURL* newCurl() {
    CURL* c = curl_easy_init();
#if defined(__linux__)
    if (c) {
        static const char* const bundle = [] () -> const char* {
            if (const char* env = std::getenv("SSL_CERT_FILE")) return env;
            for (const char* p : {"/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Gentoo
                                  "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL
                                  "/etc/ssl/ca-bundle.pem",                             // openSUSE
                                  "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // newer RHEL
                                  "/etc/ssl/cert.pem"})                                 // Alpine, Void
                if (FILE* f = std::fopen(p, "rb")) { std::fclose(f); return p; }
            return nullptr;
        }();
        if (bundle) curl_easy_setopt(c, CURLOPT_CAINFO, bundle);
    }
#endif
    return c;
}
}  // namespace

std::string OpenSubtitles::apiBase() const {
    if (const char* env = std::getenv("TOLGA_OPENSUBTITLES_URL")) return env;  // tests
    return m_baseUrl.empty() ? kDefaultApi : m_baseUrl;
}

OpenSubtitles::Reply OpenSubtitles::request(const std::string& method, const std::string& url,
                                            const std::string& body, bool auth) {
    Reply r;
    CURL* c = newCurl();
    if (!c) {
        r.error = "network unavailable";
        return r;
    }
    std::string apiKey, token;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        apiKey = m_settings.apiKey.empty() ? kBuiltInKey : m_settings.apiKey;  // a user's own key wins
        token = m_token;
    }
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, ("Api-Key: " + apiKey).c_str());
    if (auth && !token.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    if (method == "POST") headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    if (method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(body.size()));
    }
    const CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return r;
}

bool OpenSubtitles::ensureLogin(std::string& error) {
    std::string user, pass;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_token.empty() || m_settings.username.empty()) return true;  // logged in, or anonymous
        user = m_settings.username;
        pass = m_settings.password;
    }
    const Reply r = request("POST", apiBase() + "/login", json{{"username", user}, {"password", pass}}.dump(), false);
    if (!r.error.empty()) {
        error = TF("Couldn't reach OpenSubtitles: {}", {r.error});
        return false;
    }
    if (r.code != 200) {
        const std::string msg = apiMessage(r.body);
        error = r.code == 401 ? std::string(T("OpenSubtitles login failed: check the user name and password."))
                              : TF("OpenSubtitles login failed ({})", {std::to_string(r.code)}) +
                                    (msg.empty() ? "" : ": " + msg);
        return false;
    }
    try {
        const json j = json::parse(r.body);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_token = j.value("token", "");
        const std::string host = j.value("base_url", "");
        if (!host.empty()) m_baseUrl = "https://" + host + "/api/v1";  // VIP accounts get their own host
    } catch (...) {
        error = T("OpenSubtitles login: unexpected answer");
        return false;
    }
    return true;
}

// ================================================================== search

void OpenSubtitles::search(const std::string& videoPath, const std::string& query, const std::string& languages) {
    if (!settings().hasApiKey()) {
        setStatus([](OpenSubtitlesStatus& s) {
            s.error = true;
            s.message = T("This build has no OpenSubtitles API key. Add one under Account...");
        });
        return;
    }
    setStatus([](OpenSubtitlesStatus& s) {
        s.busy = true;
        s.searching = true;
        s.error = false;
        s.results.clear();
        s.message = T("Searching...");
    });
    post([=] {
        std::string error;
        if (!ensureLogin(error)) {
            setStatus([&](OpenSubtitlesStatus& s) { s.busy = false; s.searching = false; s.error = true; s.message = error; });
            return;
        }
        // languages: "tr, en" -> "en,tr" (the API wants lower case, sorted, no spaces)
        std::vector<std::string> langs;
        std::stringstream ls(lower(languages));
        std::string l;
        while (std::getline(ls, l, ',')) {
            l.erase(std::remove(l.begin(), l.end(), ' '), l.end());
            if (!l.empty()) langs.push_back(l);
        }
        std::vector<std::string> sorted = langs;
        std::sort(sorted.begin(), sorted.end());
        std::string langParam;
        for (const std::string& x : sorted) langParam += (langParam.empty() ? "" : ",") + x;

        const std::string hash = videoPath.empty() ? "" : movieHash(videoPath);
        // parameters in alphabetical order, as the API asks (avoids a redirect)
        auto buildUrl = [&](bool withHash, bool withQuery) {
            std::string url = apiBase() + "/subtitles?";
            std::string sep;
            if (!langParam.empty()) { url += "languages=" + langParam; sep = "&"; }
            if (withHash && !hash.empty()) { url += sep + "moviehash=" + hash; sep = "&"; }
            if (withQuery && !query.empty()) { url += sep + "query=" + escape(lower(query)); }
            return url;
        };

        std::vector<OnlineSubtitle> results;
        auto run = [&](const std::string& url) -> bool {
            const Reply r = request("GET", url, {}, true);
            if (!r.error.empty()) { error = TF("Couldn't reach OpenSubtitles: {}", {r.error}); return false; }
            if (r.code == 401 || r.code == 403) { error = T("OpenSubtitles refused the API key."); return false; }
            if (r.code == 429) { error = T("Too many requests, try again in a few seconds."); return false; }
            if (r.code != 200) {
                const std::string msg = apiMessage(r.body);
                error = TF("Search failed ({})", {std::to_string(r.code)}) + (msg.empty() ? "" : ": " + msg);
                return false;
            }
            try {
                const json j = json::parse(r.body);
                for (const json& item : j.value("data", json::array())) {
                    const json& a = item.value("attributes", json::object());
                    for (const json& f : a.value("files", json::array())) {
                        OnlineSubtitle s;
                        s.fileId = f.value("file_id", int64_t(0));
                        s.fileName = f.value("file_name", "");
                        s.release = a.value("release", "");
                        s.language = a.value("language", "");
                        s.downloads = a.value("download_count", int64_t(0));
                        s.hashMatch = a.value("moviehash_match", false);
                        s.hearingImpaired = a.value("hearing_impaired", false);
                        if (a.contains("fps") && a["fps"].is_number()) s.fps = a["fps"];
                        const std::string up = a.value("upload_date", "");
                        s.uploadDate = up.substr(0, 10);
                        if (a.contains("feature_details") && a["feature_details"].is_object()) {
                            const json& fd = a["feature_details"];
                            if (fd.contains("year") && fd["year"].is_number()) s.year = fd["year"];
                        }
                        if (s.release.empty()) s.release = s.fileName;
                        const bool dup = std::any_of(results.begin(), results.end(),
                                                     [&](const OnlineSubtitle& o) { return o.fileId == s.fileId; });
                        if (s.fileId && !dup) results.push_back(s);
                    }
                }
            } catch (...) {
                error = T("Search: unexpected answer from OpenSubtitles");
                return false;
            }
            return true;
        };
        bool ok = run(buildUrl(true, !query.empty()));
        if (ok && results.empty() && !hash.empty() && !query.empty()) ok = run(buildUrl(false, true));

        // exact file matches first, then the user's language order, then popularity
        auto langRank = [&](const std::string& code) {
            auto it = std::find(langs.begin(), langs.end(), code);
            return it == langs.end() ? 99 : int(it - langs.begin());
        };
        std::stable_sort(results.begin(), results.end(), [&](const OnlineSubtitle& a, const OnlineSubtitle& b) {
            if (a.hashMatch != b.hashMatch) return a.hashMatch;
            if (langRank(a.language) != langRank(b.language)) return langRank(a.language) < langRank(b.language);
            return a.downloads > b.downloads;
        });

        setStatus([&](OpenSubtitlesStatus& s) {
            s.busy = false;
            s.searching = false;
            s.error = !ok;
            s.results = ok ? results : std::vector<OnlineSubtitle>{};
            ++s.resultsSerial;
            if (!ok) s.message = error;
            else if (results.empty()) s.message = T("No subtitles found.");
            else s.message = results.size() == 1 ? std::string(T("1 subtitle found.")) : TF("{} subtitles found.", {std::to_string(results.size())});
        });
    });
}

// ================================================================== download

void OpenSubtitles::download(const OnlineSubtitle& sub) {
    setStatus([&](OpenSubtitlesStatus& s) {
        s.busy = true;
        s.error = false;
        s.downloadingId = sub.fileId;
        s.message = T("Downloading...");
    });
    post([=] {
        std::string error;
        auto finish = [&](bool ok, const std::string& msg, const std::string& path) {
            setStatus([&](OpenSubtitlesStatus& s) {
                s.busy = false;
                s.downloadingId = 0;
                s.error = !ok;
                s.message = msg;
                if (ok) {
                    s.downloadedPath = path;
                    s.downloadedLanguage = sub.language;
                }
            });
        };
        if (!ensureLogin(error)) return finish(false, error, {});

        const Reply r = request("POST", apiBase() + "/download", json{{"file_id", sub.fileId}}.dump(), true);
        if (!r.error.empty()) return finish(false, TF("Couldn't reach OpenSubtitles: {}", {r.error}), {});
        if (r.code == 406 || r.code == 429) {
            const std::string msg = apiMessage(r.body);
            return finish(false, msg.empty() ? std::string(T("Daily download limit reached. Log in (Account...) for more."))
                                             : msg, {});
        }
        if (r.code != 200) {
            const std::string msg = apiMessage(r.body);
            return finish(false, TF("Download failed ({})", {std::to_string(r.code)}) + (msg.empty() ? "" : ": " + msg), {});
        }
        std::string link, fileName = sub.fileName, remaining;
        try {
            const json j = json::parse(r.body);
            link = j.value("link", "");
            fileName = j.value("file_name", fileName);
            if (j.contains("remaining") && j["remaining"].is_number())
                remaining = std::to_string(j["remaining"].get<int64_t>());
        } catch (...) {
        }
        if (link.empty()) return finish(false, T("Download failed: no link in the answer"), {});

        // the link points at a plain file server; no API headers needed
        std::string data;
        CURL* c = newCurl();
        curl_easy_setopt(c, CURLOPT_URL, link.c_str());
        curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &data);
        const CURLcode rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
        if (rc != CURLE_OK || code != 200 || data.empty())
            return finish(false, T("Download failed while fetching the file"), {});

        std::error_code ec;
        const fs::path dir = utf8Path(m_configDir) / "Subtitles" / std::to_string(sub.fileId);
        fs::create_directories(dir, ec);
        if (fileName.empty()) fileName = "subtitle.srt";
        const fs::path out = dir / safeFileName(fileName);
        std::ofstream(out, std::ios::binary | std::ios::trunc) << data;
        std::string msg = remaining.empty() ? TF("Added {}", {fileName}) : TF("Added {} ({} downloads left today)", {fileName, remaining});
        finish(true, msg, utf8String(out));
    });
}
