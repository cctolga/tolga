#pragma once
// The boundary between the Tolga player and its AirPlay module.
//
// The player (this repository) only talks to AirPlay through this header. The AirPlay module
// is a separate, closed-source library that implements createAirPlay(). It comes either
// prebuilt (downloaded by CMake on macOS, see cmake/AirPlayPrebuilt.cmake) or from its private
// source folder (TOLGA_AIRPLAY_DIR). Without it src/AirPlayNone.cpp is linked: no AirPlay,
// and the menu points to the official app.
//
// Change anything in this file (types, members, virtual functions, their order) and
// TOLGA_AIRPLAY_API_VERSION must go up: a module built for another version refuses to start.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#define TOLGA_AIRPLAY_API_VERSION 1

// An AirPlay receiver on the local network, as the menu shows it.
struct AirPlayTarget {
    std::string deviceId;  // "AA:BB:CC:DD:EE:FF"
    std::string name;      // "Living Room"
    std::string model;     // "Apple TV 4K (3rd gen)"
    std::string details;   // tooltip text (address, model ID)
    bool        video = true;  // false: audio-only receiver
};

// A track of the loaded file, as the player knows it (mpv's track-list).
struct CastTrack {
    int64_t     mpvId = 0;
    std::string type;          // "video", "audio", "sub"
    int64_t     ffIndex = -1;
    std::string codec, lang, title;
    std::string externalFile;  // external subtitle file
    bool        selected = false;
    int         channels = 0;  // audio channel count (0 = unknown)
};

struct CastStatus {
    enum class Phase {
        Idle,        // not casting
        Preparing,   // getting the file ready for the TV (see progress)
        Connecting,  // connecting / verifying the pairing
        NeedPin,     // the TV shows a PIN, waiting for the user
        Pairing,     // checking the PIN
        Loading,     // video sent, Apple TV is buffering
        Playing,
        Paused,
        Ended,       // the video played to the end on the TV
        Failed,      // see `error`; casting has stopped
    };
    Phase       phase = Phase::Idle;
    std::string deviceId;
    std::string deviceName;
    double      position = 0.0;  // seconds, extrapolated to "now" while playing
    double      duration = 0.0;
    std::string error;
    bool        pinRejected = false;  // NeedPin again after a wrong PIN
    double      progress = 0.0;       // Preparing: 0..1

    bool        hls = false;          // audio/subtitle tracks can be switched on the TV
    double      subtitleDelay = 0.0;  // the delay the TV's subtitles were made with
    // Tracks the TV reports as selected (player track IDs; sub -1 = off), changed when
    // someone uses the TV remote. selectionSerial goes up on every report.
    int64_t     tvAudio = -1;
    int64_t     tvSub = -1;
    uint64_t    selectionSerial = 0;

    bool active() const { return phase != Phase::Idle && phase != Phase::Failed; }
    bool playing() const { return phase == Phase::Playing || phase == Phase::Loading; }
};

struct LicenseInfo {
    enum class State { Community, Licensed, Trial, Expired };
    State       state = State::Community;
    std::string email;          // Licensed: who the key was issued to
    int         trialDaysLeft = 0;
    std::string buyUrl;         // where to buy (may be empty)
};

class AirPlay {
public:
    virtual ~AirPlay() = default;

    // ---- receivers (found in the background from start() on)
    virtual void start() = 0;
    virtual void refresh() = 0;  // browse again now (the menu was opened)
    virtual std::vector<AirPlayTarget> devices() const = 0;
    virtual bool searching() const = 0;

    // ---- casting
    // Whether `media` with these tracks can be played on an Apple TV; `reason` says why not.
    virtual bool canCast(const std::string& media, const std::vector<CastTrack>& tracks, std::string& reason) const = 0;
    // Starts casting `media` (local file path or http(s) URL) to the receiver at `startSeconds`.
    virtual void startCast(const std::string& deviceId, const std::string& media, double startSeconds,
                           double duration, const std::vector<CastTrack>& tracks, double subtitleDelay) = 0;
    // Stops casting and disconnects. Returns the last known position.
    virtual double stopCast() = 0;
    virtual void submitPin(const std::string& pin) = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void seek(double seconds) = 0;
    virtual void selectAudio(int64_t mpvId) = 0;     // only while status().hls
    virtual void selectSubtitle(int64_t mpvId) = 0;  // -1 = off
    virtual CastStatus status() const = 0;

    // ---- license of the official app
    virtual LicenseInfo license() const = 0;
    virtual bool castingAllowed() const = 0;
    virtual bool activate(const std::string& key, std::string& error) = 0;

    // Stops discovery and casting; called once before exit.
    virtual void shutdown() = 0;
};

// Implemented by the AirPlay module. `configDir` is the app's settings folder; `wake` may be
// called from any thread when something changed and the UI should redraw. `apiVersion` is the
// version the caller was compiled with. Returns nullptr when this build has no AirPlay or the
// module was made for another API version.
std::unique_ptr<AirPlay> createAirPlay(const std::string& configDir, std::function<void()> wake,
                                       int apiVersion = TOLGA_AIRPLAY_API_VERSION);
