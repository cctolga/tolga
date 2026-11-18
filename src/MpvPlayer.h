#pragma once

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// One entry of mpv's "track-list" (audio, video or subtitle stream).
struct Track {
    int64_t     id = 0;
    std::string type;      // "video", "audio" or "sub"
    std::string title;
    std::string lang;
    bool        selected = false;
    bool        external = false;  // loaded from a separate file (.srt etc.)
    int64_t     ffIndex = -1;      // stream index inside the file (for ffmpeg -map 0:N)
    std::string codec;             // "h264", "aac", "subrip", "mov_text"...
    std::string externalFile;      // path of an external subtitle/audio file
    bool        isDefault = false; // marked default in the file
    bool        forced = false;
    int         channels = 0;      // audio channel count
    int         width = 0, height = 0;  // video size
    double      fps = 0.0;

    std::string label() const;     // human readable name for menus
};

// One entry of mpv's playlist.
struct PlaylistEntry {
    std::string path;
    std::string title;     // from metadata, may be empty
    bool        current = false;
};

struct Chapter {
    std::string title;
    double      time = 0.0;
};

// Snapshot of the player state, updated from mpv property-change events.
struct PlayerState {
    double timePos  = 0.0;
    double duration = 0.0;
    double volume   = 100.0;
    bool   paused   = false;
    bool   muted    = false;
    bool   idle     = true;   // no file loaded
    std::string title;
    std::string path;         // file path or URL that is loaded
    int    videoW = 0;        // display size of the video (after aspect correction)
    int    videoH = 0;
    bool   subVisible = true;
    double audioDelay = 0.0;        // seconds, + = audio later
    double speed = 1.0;
    int    rotate = 0;              // 0 / 90 / 180 / 270
    bool   hwdec = true;            // hardware decoding allowed
    int    brightness = 0, contrast = 0, saturation = 0, gamma = 0, hue = 0;  // -100..100
    double subDelay = 0.0;          // seconds, + = subtitles later
    int64_t secondarySid = -1;      // second subtitle shown at the top, -1 = none
    bool   secondarySubVisible = true;
    std::vector<Track> tracks;

    std::vector<PlaylistEntry> playlist;
    int    playlistPos = -1;
    std::vector<Chapter> chapters;
    int    chapter = -1;
    bool   loopFile = false;        // repeat the current file
    bool   loopPlaylist = false;    // repeat the whole list
    bool   shuffled = false;
};

// Thin wrapper around libmpv + its OpenGL render API.
// All methods must be called from the thread that owns the GL context.
class MpvPlayer {
public:
    using GetProcAddressFn = void* (*)(void* ctx, const char* name);

    // `wakeup` is called from mpv's internal threads whenever there are new
    // events or a new frame. It must be thread-safe (we push an SDL event).
    bool init(GetProcAddressFn getProcAddress, std::function<void()> wakeup);
    // why init() failed, for the message to the user and the trace (empty when it worked)
    const std::string& initError() const { return m_initError; }
    void shutdown();
    // A property as text ("mpv-version", "ffmpeg-version"...), "" if unknown.
    std::string propertyString(const char* name) const;

    void load(const std::string& path);
    void togglePause();
    // playlist
    void append(const std::string& path);      // starts playing if nothing is loaded
    void playIndex(int index);
    void removeIndex(int index);
    void clearPlaylist();                       // keeps the playing file
    void movePlaylistEntry(int from, int to);   // `to` = index before which it lands
    void sortPlaylistByName();
    void setShuffle(bool on);
    void setRepeat(bool file, bool playlist);
    bool playNext();                            // false if this was the last one
    void seekChapter(int index);
    void setPause(bool paused);
    void seekRelative(double seconds);
    void seekAbsolute(double seconds, bool exact);
    void setVolume(double volume);
    void toggleMute();
    void setMute(bool mute);

    // Subtitles / audio tracks. id < 0 means "off".
    void selectSubtitle(int64_t id);
    void selectAudio(int64_t id);
    void addSubtitle(const std::string& path, const std::string& title = {}, const std::string& lang = {});
    void toggleSubVisibility();
    void setSubVisibility(bool visible);
    void selectSecondarySubtitle(int64_t id);  // -1 = none
    void setSecondarySubVisibility(bool visible);
    void selectVideo(int64_t id);
    void setAudioDelay(double seconds);
    void setSpeed(double speed);
    void setAspect(const std::string& ratio);   // "" = the file's own, else "16:9" ...
    void setCrop(const std::string& ratio);     // "" = none, else crop to "21:9" ... (centred)
    void setRotate(int degrees);
    void setHardwareDecoding(bool on);
    // "brightness", "contrast", "saturation", "gamma", "hue": -100..100
    void setPicture(const char* property, int value);
    // Back to defaults for a new file (aspect, crop, rotation, speed, picture).
    void resetVideoSettings();
    void setSubDelay(double seconds);
    // 10-band graphic equalizer (32 Hz ... 16 kHz), gains in dB; all zero = off.
    static constexpr int kEqBands = 10;
    static const float kEqFrequencies[kEqBands];
    void setEqualizer(const float gains[kEqBands]);
    void cycleSubtitle();
    // Vertical subtitle position, 0 = top, 100 = bottom (mpv's sub-pos).
    void setSubPos(int pos);

    // Drains the mpv event queue. Returns false when mpv wants to quit.
    // `fileLoaded` is set to true when a new file finished loading.
    bool processEvents(bool& fileLoaded);

    // True if mpv has a new video frame to show.
    bool hasNewFrame();

    // Renders the current video frame into the given framebuffer.
    void render(int fbo, int width, int height);
    void reportSwap();

    const PlayerState& state() const { return m_state; }

private:
    std::string m_initError;
    static void onWakeup(void* self);
    void command(const char* const* args);
    void readTrackList(const mpv_node& list);
    void readPlaylist(const mpv_node& list);
    void readChapters(const mpv_node& list);
    void applyEqualizer();

    mpv_handle*         m_mpv = nullptr;
    mpv_render_context* m_render = nullptr;
    std::function<void()> m_wakeup;
    PlayerState m_state;
    float m_eqGains[kEqBands] = {};    // what the user set
    float m_eqApplied[kEqBands] = {};  // what the filter chain has now
    bool m_eqBuilt = false;
    bool m_eqStale = false;            // gains changed by af-command since it was built
    int m_eqRate = 0;
    int m_sampleRate = 0;              // of the audio playing now
};
