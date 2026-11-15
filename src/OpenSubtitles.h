#pragma once
// Online subtitle search and download through the OpenSubtitles.com REST API (v1).
//
// - Needs an API key ("consumer") from https://www.opensubtitles.com/en/consumers.
//   Logging in with an opensubtitles.com account is optional but raises the daily
//   download limit. Key, user name and password are kept in the app's config folder,
//   in a file only the current user can read.
// - Searches by the file's OpenSubtitles hash (exact match for that release) plus a
//   text query, in the chosen languages.
// - Network work runs on a background thread; `wake` is called when something changed.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct OpenSubtitlesSettings {
    std::string apiKey;               // the user's own key (optional when one is built in)
    bool        builtInKey = false;   // the app was built with its own API key
    std::string username;
    std::string password;
    std::string languages = "tr,en";  // comma separated ISO 639-1 codes
    bool hasApiKey() const { return builtInKey || !apiKey.empty(); }
};

struct OnlineSubtitle {
    int64_t     fileId = 0;
    std::string fileName;   // e.g. "Movie.2010.1080p.BluRay.Turkish.srt"
    std::string release;    // release name the subtitle was made for
    std::string language;   // "tr"
    int64_t     downloads = 0;
    bool        hashMatch = false;     // made for exactly this file
    bool        hearingImpaired = false;
    double      fps = 0;
    int         year = 0;          // of the movie / episode
    std::string uploadDate;        // "2016-07-09"
};

struct OpenSubtitlesStatus {
    bool busy = false;
    bool searching = false;           // a search (not a download) is running
    std::string message;              // status / error text for the UI
    bool error = false;
    std::vector<OnlineSubtitle> results;
    uint64_t resultsSerial = 0;       // bumps on every new search result
    std::string downloadedPath;       // set once per finished download (see takeDownloaded)
    std::string downloadedLanguage;
    int64_t downloadingId = 0;        // fileId being downloaded, 0 = none
};

class OpenSubtitles {
public:
    OpenSubtitles(std::string configDir, std::function<void()> wake);
    ~OpenSubtitles();

    OpenSubtitlesSettings settings() const;
    void saveSettings(const OpenSubtitlesSettings& s);

    void search(const std::string& videoPath, const std::string& query, const std::string& languages);
    void download(const OnlineSubtitle& sub);

    OpenSubtitlesStatus status() const;
    // Returns the path of a subtitle that finished downloading since the last call ("" if none).
    std::string takeDownloaded(std::string* language = nullptr);

    // "Movie Name 2010" from "Movie.Name.2010.1080p.BluRay.x264.mkv"
    static std::string queryFromFileName(const std::string& path);
    // OpenSubtitles hash: size + sum of the first and last 64 KB as 64-bit words; "" on error.
    static std::string movieHash(const std::string& path);

private:
    void worker();
    void post(std::function<void()> job);
    void setStatus(const std::function<void(OpenSubtitlesStatus&)>& f);
    bool ensureLogin(std::string& error);
    struct Reply { long code = 0; std::string body; std::string error; };
    Reply request(const std::string& method, const std::string& url, const std::string& body, bool auth);
    std::string apiBase() const;

    std::string m_configDir;
    std::string m_settingsPath;
    std::function<void()> m_wake;

    mutable std::mutex m_mutex;
    OpenSubtitlesSettings m_settings;
    OpenSubtitlesStatus m_status;
    std::string m_token;     // JWT from /login
    std::string m_baseUrl;   // API host to use after login (VIP users get another one)

    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::deque<std::function<void()>> m_jobs;
    bool m_quit = false;
    std::thread m_thread;
};
