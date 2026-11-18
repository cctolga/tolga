#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

struct AirPlayTarget;
struct CastStatus;
class Thumbnailer;
struct OpenSubtitlesStatus;
struct OpenSubtitlesSettings;
struct LicenseInfo;

struct PlayerState;

// What the user asked for during one overlay frame.
struct OverlayActions {
    bool   togglePause      = false;
    bool   toggleMute       = false;
    bool   toggleFullscreen = false;
    bool   openFile         = false;
    std::string openPath;             // a file from the recent list on the idle screen
    bool   clearRecent      = false;
    double seekRelative     = 0.0;
    bool   seekAbsolute     = false;
    bool   seekExact        = false;
    double seekTo           = 0.0;
    bool   setVolume        = false;
    double volume           = 0.0;

    bool    selectSub        = false;
    int64_t subId            = -1;    // -1 = off
    bool    selectAudio      = false;
    int64_t audioId          = -1;
    bool    loadSubFile      = false;
    bool    selectSecondarySub = false;
    int64_t secondarySubId   = -1;    // -1 = none
    bool    setSubVisible    = false;
    bool    subVisible       = true;
    bool    setSecondaryVisible = false;
    bool    secondaryVisible = true;
    bool    selectVideo      = false;
    int64_t videoId          = -1;

    bool    setAudioDelay = false;
    double  audioDelay = 0.0;
    bool    setSubDelay = false;
    double  subDelay = 0.0;
    bool    setSpeed = false;
    double  speed = 1.0;
    bool    setAspect = false;
    std::string aspect;        // "" = default
    bool    setCrop = false;
    std::string crop;          // "" = none
    bool    setRotate = false;
    int     rotate = 0;
    bool    setHwdec = false;
    bool    hwdec = true;
    const char* pictureProperty = nullptr;  // "brightness" ...
    int     pictureValue = 0;
    bool    setEqualizer = false;
    float   eqGains[10] = {};

    // playlist
    int     playlistPlay = -1;
    int     playlistRemove = -1;
    bool    playlistClear = false;
    bool    playlistAdd = false;     // open the file dialog in "add" mode
    int     playlistMoveFrom = -1, playlistMoveTo = -1;
    bool    playlistSort = false;
    bool    toggleShuffle = false;
    bool    cycleRepeat = false;
    int     seekChapter = -1;

    // online subtitles
    bool        onlineSearch = false;
    std::string onlineQuery, onlineLanguages;
    int         onlineDownload = -1;   // index into the results
    bool        onlineSaveSettings = false;
    std::string onlineApiKey, onlineUser, onlinePassword;

    bool        airplayMenuOpened = false;  // menu was just opened -> refresh the device list
    bool        selectCastTarget  = false;
    std::string castTarget;                 // device ID, empty = play on this computer

    bool        makeDefaultPlayer = false;  // "Make Tolga Player your default video player"

    // licensing (official builds)
    bool        openLicense = false;        // show the license dialog
    bool        closeLicense = false;
    bool        activateLicense = false;
    std::string licenseKey;
    bool        openBuyPage = false;
    bool        openWebsite = false;
    bool        minimizeWindow = false;  // the window buttons Tolga draws itself (Linux)
    bool        toggleMaximize = false;
    bool        closeWindow    = false;

    bool        submitPin = false;          // user typed the PIN shown on the TV
    std::string pin;
    bool        cancelPin = false;
};

// What the AirPlay menu needs to show.
struct AirPlayView {
    const std::vector<AirPlayTarget>* devices = nullptr;
    bool        searching = false;
    std::string castTarget;  // currently selected device ID ("" = this computer)
    const CastStatus* cast = nullptr;  // casting status; the panel acts as a remote while active
    const LicenseInfo* license = nullptr;
    std::string licenseError;          // last activation error, shown in the license dialog
    bool        available = true;      // false: this build has no AirPlay module
    std::string castBlocked;           // why the open video can't go to an Apple TV ("" = it can)
};

// What the playlist view needs besides the player state.
struct PlaylistView {
    std::vector<double> durations;  // per playlist entry, seconds (-1 = unknown)
    std::vector<float>  progress;   // per entry, 0..1 how far it was watched (0 = not yet)
};

// What the "Search Online" view needs to show.
struct OnlineView {
    const OpenSubtitlesStatus*   status = nullptr;
    const OpenSubtitlesSettings* settings = nullptr;
    std::string defaultQuery;  // title guessed from the file name
};

// The ImGui layer drawn on top of the video: floating control bar,
// idle/drop screen and fullscreen title bar.
class Overlay {
public:
    // `glslVersion`: shader version for the GL context in use ("#version 150" for GL 3.2, "#version 120" for 2.1)
    bool init(SDL_Window* window, SDL_GLContext gl, const char* glslVersion);
    void shutdown();

    // Forwards an SDL event to ImGui. Mouse motion also counts as activity
    // and brings the controls back.
    void processEvent(const SDL_Event& e);

    // Builds the UI for this frame. Call between video render and swap.
    OverlayActions draw(const PlayerState& st, bool fullscreen, const AirPlayView& airplay,
                        const OnlineView& online = {}, const PlaylistView& playlist = {});
    void render();

    bool wantsMouse() const;
    bool wantsKeyboard() const;
    bool isAnimating() const;
    // Short message shown at the top of the window for a few seconds.
    void showToast(const std::string& text);
    bool controlsHidden() const { return m_alpha <= 0.0f; }
    bool controlsShown() const { return m_targetAlpha > 0.0f; }
    // Height (in window points) covered by the control bar, so subtitles can move above it.
    static float controlsHeight();
    void poke();  // mark user activity (e.g. a key press)
    // macOS: the native title bar sits transparently over the video (see MacWindow.h).
    void setTitlebarOverlay(bool on, bool fullscreen) { m_titlebarOverlay = on; m_fullscreen = fullscreen; }
    // Linux: the window has no system frame; Tolga draws the title, the window buttons and
    // leaves the top strip for dragging (see chromeHeight / chromeButtonsWidth).
    void setCustomChrome(bool on, bool maximized) { m_customChrome = on; m_maximized = maximized; }
    static constexpr float kChromeH = 32.0f;          // in UI units, like everything here
    static constexpr float kChromeButtonW = 46.0f;
    float controlsAlpha() const { return m_alpha; }
    // Seek-bar previews come from here (may be null).
    void setThumbnails(const Thumbnailer* thumbs) { m_thumbs = thumbs; }
    // recently opened files for the idle screen (nullptr / empty = no list)
    void setRecentFiles(const std::vector<std::string>* recent) { m_recent = recent; }
    // The idle screen offers "Make Tolga Player your default video player" while `offer` is
    // true; `note` (the result of the last attempt) is shown instead when it isn't empty.
    void setDefaultPlayer(bool available, bool offer, const std::string& note) {
        m_defaultAvailable = available; m_offerDefault = offer; m_defaultNote = note;
    }
    // The drawable the frame is rendered into (pixels) and its pixels per point; the
    // controls are laid out for exactly this size.
    // scale: pixels per window unit (Retina 2); ui: how much bigger the controls are drawn
    // (Windows at 150 % display scaling: 1.5), mouse positions are divided by it too
    void setFramebuffer(int w, int h, float scale, float ui = 1.0f) {
        m_fbW = w; m_fbH = h; m_fbScale = scale > 0 ? scale : 1.0f; m_uiScale = ui > 0 ? ui : 1.0f;
    }

    // Side panel with video / audio / subtitle tracks (IINA-style), sliding in from the right.
    enum class PanelTab { Video, Audio, Subtitles, General };
    // Whatever the system uses (the default), dark or light; remembered between runs.
    enum class Theme { System, Dark, Light };
    Theme theme() const { return m_theme; }
    void setTheme(Theme theme);
    bool lightTheme() const;  // the light appearance is in use (after the last draw)
    void togglePanel(PanelTab tab);
    bool panelOpen() const { return m_panelOpen; }
    void togglePlaylist() { m_playlistOpen = !m_playlistOpen; poke(); }
    bool playlistOpen() const { return m_playlistOpen; }
    void closePlaylist() { m_playlistOpen = false; }
    void closePanel() { m_panelOpen = false; }
    // back from the online search to the subtitle list (e.g. after a download)
    // new file: aspect / crop choices back to their defaults
    void resetVideoSettings() { m_aspectIdx = 0; m_cropIdx = 0; }
    void showSubtitleList() { m_resultsOpen = false; m_accountOpen = false; }
    bool resultsOpen() const { return m_resultsOpen; }
    void openLicenseDialog() { m_licenseOpen = true; m_licenseFirstFrame = true; poke(); }
    // About window (like IINA's): name, version, mpv/FFmpeg versions, license and credits.
    struct AboutInfo {
        std::string version;        // "0.1.0"
        std::string mpvVersion;     // "mpv v0.41.0"
        std::string ffmpegVersion;  // "7.1"
        std::string licenseText;    // the player's license (Apache-2.0)
        std::string creditsText;    // third-party notices
    };
    void setAbout(AboutInfo info);
    void openAbout() { m_aboutOpen = true; m_aboutTab = 0; poke(); }
    bool aboutOpen() const { return m_aboutOpen; }
    void closeLicenseDialog() { m_licenseOpen = false; }
    bool licenseOpen() const { return m_licenseOpen; }
    void closeResults() { m_resultsOpen = false; }

private:
    // Old Intel OpenGL 2.1 drivers on Windows draw only the first of several vertex buffers
    // given in one frame: everything after it (the idle window, the side panel, menus) stays
    // invisible. There all lists go to the GPU as one (TOLGA_GL_MERGE=0/1 to force it off/on).
    bool m_mergeDrawLists = false;
    struct ImDrawList* m_merged = nullptr;
    void mergeDrawLists(struct ImDrawData* dd);
    void drawIdleScreen(OverlayActions& out);
    void drawControls(const PlayerState& st, bool fullscreen, const AirPlayView& airplay, OverlayActions& out);
    void drawAirPlayMenu(const AirPlayView& airplay, OverlayActions& out);
    void drawTitleBar(const PlayerState& st);
    void drawTitleShade();
    float topInset() const {
        if (m_fullscreen) return 0.0f;
        return m_customChrome ? kChromeH : m_titlebarOverlay ? 28.0f : 0.0f;
    }
    void drawWindowChrome(const PlayerState& st, OverlayActions& out);
    void drawSidePanel(const PlayerState& st, const AirPlayView& airplay, const OnlineView& online,
                       OverlayActions& out);
    void drawResultsDialog(const OnlineView& online, OverlayActions& out);
    bool delayCard(const char* id, double value, float inner, double& changed);
    void equalizerCard(float inner, OverlayActions& out);
    void videoCards(const PlayerState& st, float inner, OverlayActions& out);
    void drawPlaylist(const PlayerState& st, const PlaylistView& pl, OverlayActions& out);
    void openAccountForm(const OnlineView& online);
    void drawAccountForm(const OnlineView& online, float inner, OverlayActions& out);
    void drawCastScreen(const PlayerState& st, const CastStatus& cast);
    void drawPinDialog(const CastStatus& cast, OverlayActions& out);
    void drawLicenseDialog(const AirPlayView& airplay, OverlayActions& out);
    void drawAbout(const AirPlayView& airplay, OverlayActions& out);
    void drawGeneralTab(float inner, OverlayActions& out);
    void drawToast();

    SDL_Window* m_window = nullptr;
    float    m_alpha = 1.0f;
    float    m_targetAlpha = 1.0f;
    uint64_t m_lastActivity = 0;
    uint64_t m_lastFrame = 0;
    bool     m_hoveringPanel = false;
    bool     m_menuOpen = false;

    // seek-bar drag state
    bool     m_dragging = false;
    double   m_dragPos = 0.0;
    double   m_lastSentSeek = -1.0;
    uint64_t m_lastSeekTime = 0;

    bool        m_panelOpen = false;
    PanelTab    m_panelTab = PanelTab::Subtitles;
    float       m_panelAnim = 0.0f;   // 0 = hidden, 1 = fully in
    float       m_panelVisibleW = 0.0f;
    // playlist (left side)
    bool        m_playlistOpen = false;
    float       m_playlistAnim = 0.0f;
    float       m_leftVisibleW = 0.0f;
    bool        m_chaptersTab = false;
    int         m_fbW = 0, m_fbH = 0;
    float       m_fbScale = 1.0f;
    float       m_uiScale = 1.0f;  // see setFramebuffer
    int         m_playlistSelected = -1;
    int         m_dragFrom = -1;     // playlist row being dragged
    int         m_aspectIdx = 0;
    int         m_cropIdx = 0;
    float       m_eqGains[10] = {};
    int         m_eqPreset = 0;        // index into the preset list, 0 = Manual
    uint64_t    m_lastEqSent = 0;
    bool        m_eqDragging = false;
    bool        m_resultsOpen = false;  // online subtitle results dialog
    int         m_selectedResult = -1;
    bool        m_accountOpen = false;  // ... or the OpenSubtitles account form
    std::string m_queryFor;             // file the query box was filled for
    char        m_queryBuf[256] = {};
    char        m_langBuf[64] = {};
    char        m_keyBuf[128] = {};
    char        m_userBuf[128] = {};
    char        m_passBuf[128] = {};
    static float panelWidth(float displayW);

    bool        m_titlebarOverlay = false;
    bool        m_customChrome = false;
    bool        m_maximized = false;
    bool        m_fullscreen = false;
    const Thumbnailer* m_thumbs = nullptr;
    const std::vector<std::string>* m_recent = nullptr;
    Theme m_theme = Theme::System;
    void loadTheme();
    bool m_defaultAvailable = false;
    bool m_offerDefault = false;
    std::string m_defaultNote;
    unsigned    m_thumbTex = 0;       // GL texture with the preview being shown
    int         m_thumbIndex = -1;    // which preview is in the texture
    std::vector<uint8_t> m_thumbPixels;

    std::string m_toast;
    uint64_t    m_toastUntil = 0;
    char        m_pinBuf[8] = {};
    bool        m_pinOpen = false;
    bool        m_pinChecking = false;
    bool        m_licenseOpen = false;
    AboutInfo   m_about;
    std::string m_licenseReflowed;  // license text with its hard line breaks joined into paragraphs
    std::string m_creditsReflowed;  // the same for the third-party notices
    bool        m_aboutOpen = false;
    int         m_aboutTab = 0;  // 0 license, 1 credits
    bool        m_licenseFirstFrame = false;
    char        m_licenseBuf[512] = {};
};
