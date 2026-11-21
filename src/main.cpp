// Tolga — a small, good-looking video player.
// libmpv does the playback, SDL3 gives us a window + GL context, Dear ImGui draws the controls.

#include "tolga/AirPlayApi.h"
#include "MpvPlayer.h"
#include "OpenSubtitles.h"
#include "DurationCache.h"
#include "Thumbnailer.h"
#include "RecentFiles.h"
#include "ResumePositions.h"
#include "MacWindow.h"
#include "WindowChrome.h"

#include <imgui.h>
#include "DefaultApp.h"
#include "I18n.h"
#include "Overlay.h"
#include "Paths.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#ifdef _WIN32
#include <SDL3/SDL_main.h>  // WinMain -> main, with UTF-8 arguments
#endif

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <fstream>
#include <functional>
#include <string>
#include <map>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <csignal>
#endif

namespace {

// TOLGA_TRACE=1: timing of the start-up and slow frames, written to /tmp/tolga-trace.log
// (for "the window appears late" kind of problems; macOS: open --env TOLGA_TRACE=1 -a ...)
FILE* g_trace = nullptr;
Uint64 g_traceStart = 0;
void trace(const char* fmt, ...) {
    if (!g_trace) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::fprintf(g_trace, "%9.1f ms  %s\n", (SDL_GetTicksNS() - g_traceStart) / 1e6, msg);
    std::fflush(g_trace);
}

// When the graphics driver can't give OpenGL 2.1. The Microsoft Store version can't run on
// Intel graphics from before 2012 even where the installer's copy does: Windows treats a
// packaged app as a Windows 10 one, and those old drivers refuse to load into such programs.
std::string oldGraphicsMessage() {
    std::string msg = T("Tolga Player needs OpenGL 2.1 or newer, and the graphics driver of this computer doesn't provide it.");
    if (defaultapp::packaged())
        msg = msg + "\n\n" + T("This Microsoft Store version can't use older graphics drivers like this one. The installer from cctolga.github.io/tolga works on this computer.");
    return msg;
}

std::string readTextFile(const std::string& path) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Redraws during a live window resize: macOS runs its own loop then, and SDL reports the new
// size only through event watchers (SDL_EVENT_WINDOW_EXPOSED with data1 = 1).
bool SDLCALL liveResizeWatch(void* userdata, SDL_Event* event) {
    if (event->type == SDL_EVENT_WINDOW_EXPOSED && event->window.data1 == 1)
        (*static_cast<std::function<void()>*>(userdata))();
    return true;
}

// Stands in for the AirPlay module in builds without it: no receivers, never casting.
class NoAirPlay final : public AirPlay {
public:
    void start() override {}
    void refresh() override {}
    std::vector<AirPlayTarget> devices() const override { return {}; }
    bool searching() const override { return false; }
    bool canCast(const std::string&, const std::vector<CastTrack>&, std::string& reason) const override {
        reason = T("AirPlay is part of the official Tolga Player app.");
        return false;
    }
    void startCast(const std::string&, const std::string&, double, double, const std::vector<CastTrack>&,
                   double) override {}
    double stopCast() override { return 0.0; }
    void submitPin(const std::string&) override {}
    void setPaused(bool) override {}
    void seek(double) override {}
    void selectAudio(int64_t) override {}
    void selectSubtitle(int64_t) override {}
    CastStatus status() const override { return {}; }
    LicenseInfo license() const override { return {}; }
    bool castingAllowed() const override { return false; }
    bool activate(const std::string&, std::string& error) override {
        error = T("This build has no AirPlay.");
        return false;
    }
    void shutdown() override {}
};

Uint32 g_wakeEvent = 0;  // pushed by mpv threads (new frame / new events)
Uint32 g_aboutEvent = 0;  // "About Tolga Player" in the macOS app menu
Uint32 g_openEvent = 0;  // pushed by the file dialog callback (data1 = strdup'ed path,
                         // code 0 = open video, 1 = subtitle, 2 = add to the playlist)

void* getProcAddress(void*, const char* name) {
    return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
}

// Wakes the main loop. Called from other threads (mpv's video thread, AirPlay, downloads).
// They must not push the SDL event themselves: SDL_PushEvent takes SDL's event-watch lock,
// and while the window is being resized the main thread holds that lock while it waits in
// mpv_render_context_render for mpv's video thread. If that video thread then pushed the
// event itself, each would wait for the other forever (sound kept playing, picture and app
// hung). So callers only raise a flag, and this small thread does the pushing; if it has to
// wait for the lock, nobody else does.
class Waker {
public:
    void start() { m_thread = std::thread([this] { run(); }); }
    void stop() {
        if (!m_thread.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_cv.notify_one();
        if (m_thread.joinable()) m_thread.join();
    }
    ~Waker() { stop(); }  // also covers early returns from main
    void wake() {
        if (m_pending.exchange(true)) return;  // a wake-up is already on its way
        { std::lock_guard<std::mutex> lock(m_mutex); }  // the thread is waiting or will see the flag
        m_cv.notify_one();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_quit) {
            m_cv.wait(lock, [this] { return m_quit || m_pending.load(); });
            if (m_quit) break;
            m_pending = false;  // cleared first: a wake during the push sends another event
            lock.unlock();
            SDL_Event e{};
            e.type = g_wakeEvent;
            SDL_PushEvent(&e);
            lock.lock();
        }
    }
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_pending{false};
    bool m_quit = false;
};
Waker g_waker;

void pushWake() {
    g_waker.wake();
}

void onFileDialog(void* kind, const char* const* files, int) {
    if (!files || !files[0]) return;  // cancelled or error
    const int code = int(reinterpret_cast<intptr_t>(kind));
    for (int i = 0; files[i]; ++i) {
        SDL_Event e{};
        e.type = g_openEvent;
        // several videos picked with "Open": the first replaces what plays, the rest are added
        e.user.code = (code == 0 && i > 0) ? 2 : code;
        e.user.data1 = SDL_strdup(files[i]);
        SDL_PushEvent(&e);
        if (code == 1) break;
    }
}

bool isSubtitleFile(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = char(SDL_tolower(c));
    return ext == "srt" || ext == "ass" || ext == "ssa" || ext == "vtt" || ext == "sub" || ext == "sup";
}

void showSubtitleDialog(SDL_Window* window) {
    static const SDL_DialogFileFilter filters[] = {
        {T("Subtitles"), "srt;ass;ssa;vtt;sub;sup"},
        {T("All files"), "*"},
    };
    SDL_ShowOpenFileDialog(onFileDialog, reinterpret_cast<void*>(intptr_t(1)), window, filters, 2, nullptr, false);
}

void showOpenDialog(SDL_Window* window, bool add = false) {
    static const SDL_DialogFileFilter filters[] = {
        {T("Video"), "mkv;mp4;m4v;mov;avi;webm;wmv;flv;ts;m2ts;mpg;mpeg;ogv;3gp"},
        {T("All files"), "*"},
    };
    SDL_ShowOpenFileDialog(onFileDialog, reinterpret_cast<void*>(intptr_t(add ? 2 : 0)), window, filters, 2,
                           nullptr, true);
}

// Resize the window to the video's aspect ratio, fitting within 75% of the screen.
// How much bigger the controls are drawn than at 100 %: Windows and X11 report window sizes in
// pixels and leave scaling to the app (150 %, 200 % in the display settings). macOS and Wayland
// already work in points, so there it is 1.
float uiScale(SDL_Window* window) {
#ifdef __APPLE__
    (void)window;
    return 1.0f;
#else
    const float display = SDL_GetWindowDisplayScale(window), density = SDL_GetWindowPixelDensity(window);
    if (display <= 0 || density <= 0) return 1.0f;
    return std::clamp(display / density, 1.0f, 4.0f);
#endif
}

void fitWindowToVideo(SDL_Window* window, int vw, int vh) {
    if (vw <= 0 || vh <= 0) return;
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) return;
    SDL_Rect usable;
    if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable)) return;

    // dwidth/dheight are in video pixels; on Retina one window point is several pixels,
    // so we just fit by aspect ratio within the usable area.
    // (Windows at 200 %: a video pixel is two screen pixels, as on a Retina Mac)
    const float ui = uiScale(window);
    const float maxW = usable.w * 0.75f, maxH = usable.h * 0.75f;
    const float scale = std::min({maxW / (vw * ui), maxH / (vh * ui), 1.0f}) * ui;
    const int w = std::max(int(480 * ui), int(vw * scale));
    const int h = std::max(int(270 * ui), int(w * float(vh) / vw));

    SDL_SetWindowSize(window, w, h);
    chrome::keepAspect(window, float(vw) / vh);
    SDL_SetWindowPosition(window, usable.x + (usable.w - w) / 2, usable.y + (usable.h - h) / 2);
}

}  // namespace

// "audio:1 audio:2 sub:3 ..." - changes when tracks are added (e.g. a subtitle file)
static std::string trackSignature(const PlayerState& st) {
    std::string sig;
    for (const Track& t : st.tracks) sig += t.type + ":" + std::to_string(t.id) + " ";
    return sig;
}

static int64_t selectedTrack(const PlayerState& st, const char* type) {
    // a hidden primary subtitle counts as "off" (that is what the TV should show)
    if (std::strcmp(type, "sub") == 0 && !st.subVisible) return -1;
    for (const Track& t : st.tracks)
        if (t.type == type && t.selected) return t.id;
    return -1;
}

int main(int argc, char** argv) {
    g_traceStart = SDL_GetTicksNS();
    if (const char* t = std::getenv("TOLGA_TRACE"); t && *t && *t != '0') {
        g_trace = std::fopen(utf8String(std::filesystem::temp_directory_path() / "tolga-trace.log").c_str(), "w");
        trace("main, %d argument(s)%s%s", argc - 1, argc > 1 ? ": " : "", argc > 1 ? argv[1] : "");
    }
#ifndef _WIN32
    // A TV closing its connection mid-write must not kill the app.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    // name + id for the desktop: on Linux the id is the Wayland app id / X11 window class,
    // which ties the window to app.tolga.player.desktop (icon, name in the dock)
    SDL_SetAppMetadata("Tolga Player", TOLGA_VERSION, "app.tolga.player");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    i18n::init();  // the language of the user interface, from the system
    trace("SDL_Init done");
    g_wakeEvent = SDL_RegisterEvents(3);
    g_openEvent = g_wakeEvent + 1;
    g_aboutEvent = g_wakeEvent + 2;
    g_waker.start();

    // OpenGL: the newest of these that the graphics driver gives us. macOS only gives a modern GL
    // with a 3.2+ core, forward-compatible profile; older PCs (e.g. Intel graphics before 2011)
    // only have OpenGL 2.1, which mpv and the controls can use too.
    struct GlVersion { int major, minor, profile, flags; const char* glsl; };
#ifdef __APPLE__
    const GlVersion glVersions[] = {
        {3, 2, SDL_GL_CONTEXT_PROFILE_CORE, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG, "#version 150"}};
#else
    const GlVersion glVersions[] = {{3, 2, SDL_GL_CONTEXT_PROFILE_CORE, 0, "#version 150"},
                                    {3, 0, 0, 0, "#version 130"},
                                    {2, 1, 0, 0, "#version 120"}};
#endif
    SDL_Window* window = nullptr;
    SDL_GLContext gl = nullptr;
    const char* glslVersion = nullptr;
    std::string glError;
    // Linux draws its own frame; a transparent window lets it round the corners. If no GL
    // config with alpha is there, the same again without (square corners).
    for (int pass = 0; pass < 2 && !gl; ++pass) {
    const bool transparent = chrome::customFrame() && pass == 0;
    if (pass == 1 && !chrome::customFrame()) break;
    for (const GlVersion& v : glVersions) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, v.major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, v.minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, v.profile);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, v.flags);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        if (transparent) SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        // a new window each time: on EGL the window's config is tied to the context type
        window = SDL_CreateWindow("Tolga Player", 960, 540,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN |
                chrome::windowFlags(transparent));
        if (!window) {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return 1;
        }
        gl = SDL_GL_CreateContext(window);
        if (gl) {
            glslVersion = v.glsl;
            if (const float ui = uiScale(window); ui > 1.01f)  // the first size in points, as on macOS
                SDL_SetWindowSize(window, int(960 * ui), int(540 * ui));
            break;
        }
        glError += "\n  OpenGL " + std::to_string(v.major) + "." + std::to_string(v.minor) + ": " + SDL_GetError();
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    }
    if (!gl) {
        std::fprintf(stderr, "Could not create an OpenGL context (Tolga Player needs OpenGL 2.1 or newer):%s\n",
                     glError.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tolga Player",
            oldGraphicsMessage().c_str(),
            nullptr);
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    // What the driver really gave: on Windows SDL hands back an old-style context, without an
    // error, when the driver can't make the 3.x one asked for (Intel graphics before 2011 have
    // OpenGL 2.1 there). The shaders of the controls must then be GLSL 1.20.
    {
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        trace("OpenGL %s, %s", version ? version : "?", renderer ? renderer : "?");
        int major = 0, minor = 0;
        if (version) std::sscanf(version, "%d.%d", &major, &minor);
        const int got = major * 10 + minor;
        if (got > 0 && got < 21) {
            std::fprintf(stderr, "OpenGL %s is too old (Tolga Player needs 2.1 or newer)\n", version);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tolga Player",
                oldGraphicsMessage().c_str(),
                window);
            return 1;
        }
        if (got > 0 && got < 30) glslVersion = "#version 120";
        else if (got > 0 && got < 32 && std::strcmp(glslVersion, "#version 150") == 0) glslVersion = "#version 130";
    }
    chrome::setup(window, Overlay::kChromeH, 3 * Overlay::kChromeButtonW + 4);  // + the gap at the right edge
    SDL_GL_SetSwapInterval(1);  // vsync
    trace("window + OpenGL context ready (%s)", glslVersion);
    SDL_SetWindowMinimumSize(window, 480, 270);
    macSetupWindow(window);  // transparent title bar over the video (macOS only)
    macHookAboutMenu([] {
        SDL_Event ev{};
        ev.type = g_aboutEvent;
        SDL_PushEvent(&ev);
    });

    SDL_ShowWindow(window);
    macBringToFront(window);
    trace("SDL_ShowWindow");

    MpvPlayer player;
    if (!player.init(getProcAddress, pushWake)) {
        // say so instead of vanishing (a windowed app on Windows has no terminal for stderr)
        trace("mpv failed: %s", player.initError().c_str());
        const std::string msg = oldGraphicsMessage() +
                                "\n\n" + player.initError();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tolga Player", msg.c_str(), window);
        return 1;
    }
    if (std::strcmp(glslVersion, "#version 120") == 0) {
        player.useLightRendering();
        // Graphics this old decode in hardware, but mpv must copy every picture back for
        // OpenGL 2.1, and that copy is the slow part: a 1920x800 film stuttered with it and
        // played smoothly decoded by the processor (Intel HD of 2010, Windows 10). The
        // switch in the settings still turns it on. (Linux on the same laptop plays well
        // with VA-API, so only Windows.)
#ifdef _WIN32
        player.setHardwareDecoding(false);
#endif
        trace("OpenGL 2.1: mpv's fast profile");
    }
    trace("mpv ready");

    Overlay overlay;
    if (!overlay.init(window, gl, glslVersion)) {
        trace("controls failed (%s)", glslVersion);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tolga Player",
            oldGraphicsMessage().c_str(),
            window);
        return 1;
    }
    trace("overlay ready");

    // Settings live in the per-user app data folder (~/Library/Application Support/Tolga on
    // macOS, ~/.local/share/Tolga on Linux, %APPDATA%\Tolga on Windows).
    std::string configDir;
    if (char* pref = SDL_GetPrefPath("", "Tolga")) {
        configDir = pref;
        SDL_free(pref);
    }

    // AirPlay comes from the AirPlay module of the official app (see include/tolga/AirPlayApi.h).
    // Receivers are discovered in the background from the start, so the list is already
    // filled when the user opens the menu. Without the module the menu points to the app.
    std::unique_ptr<AirPlay> airplay = createAirPlay(configDir, pushWake);
    const bool hasAirPlay = airplay != nullptr;
    if (!airplay) airplay = std::make_unique<NoAirPlay>();
    airplay->start();
    trace("AirPlay module %s, started", hasAirPlay ? "loaded" : "absent");
    std::vector<AirPlayTarget> airplayDevices;
    std::string castTarget;  // selected AirPlay device ID, empty = this computer
    std::string castBlocked;   // why the open video can't be sent to an Apple TV ("" = it can)
    std::string castCheckKey;  // file + track count castBlocked was worked out for
    LicenseInfo licenseInfo;
    std::string licenseError;

    OpenSubtitles online(configDir, pushWake);
    RecentFiles recent(configDir);
    ResumePositions positions(configDir);  // where each video was left
    std::string castPath;                  // the file the TV was given (its position is the TV's)
    // "Make Tolga Player your default video player" on the idle screen, until it is
    defaultapp::refresh();  // Linux: a default entry left pointing at a deleted AppImage
    const bool defaultAvailable = defaultapp::available();
    bool offerDefault = defaultAvailable && !defaultapp::isDefault();
    std::string defaultNote;
    trace("OpenSubtitles ready");

    {   // About window: versions, license and third-party notices (from the app bundle)
        Overlay::AboutInfo about;
        about.version = TOLGA_VERSION;
        // "mpv v0.42.0-dirty" / "n9.0.2" -> "mpv 0.42.0" / "9.0.2"
        auto plain = [](std::string v) {
            const size_t d = v.find_first_of("0123456789");
            if (d == std::string::npos) return v;
            v = v.substr(d);
            const size_t end = v.find_first_not_of("0123456789.");
            return end == std::string::npos ? v : v.substr(0, end);
        };
        const std::string mpvVer = plain(player.propertyString("mpv-version"));
        about.mpvVersion = mpvVer.empty() ? std::string() : "mpv " + mpvVer;
        about.ffmpegVersion = plain(player.propertyString("ffmpeg-version"));
        std::string base;
        if (const char* b = SDL_GetBasePath()) base = b;  // .../Tolga Player.app/Contents/Resources/
#if defined(__linux__)
        base += "../share/tolga-player/";  // AppImage / install layout: usr/bin + usr/share/tolga-player
#endif
        about.licenseText = readTextFile(base + "Licenses/Tolga/LICENSE");
        if (about.licenseText.empty()) about.licenseText = readTextFile(std::string(TOLGA_SOURCE_DIR) + "/LICENSE");
        about.creditsText = readTextFile(base + "THIRD_PARTY_NOTICES.txt");
        overlay.setAbout(std::move(about));
    }
    trace("About info ready");
    OpenSubtitlesStatus onlineStatus;
    OpenSubtitlesSettings onlineSettings = online.settings();
    DurationCache durations(pushWake);
    Thumbnailer thumbnails(pushWake);
    overlay.setThumbnails(&thumbnails);
    std::map<std::string, float> watched;  // how far each file was watched this session (0..1)
    CastStatus castStatus;
    CastStatus::Phase lastCastPhase = CastStatus::Phase::Idle;
    std::string lastCastNotice;
    uint64_t lastCastRedraw = 0;

    // Track bookkeeping while casting: which tracks the cast was started with (a new
    // subtitle file means packaging again), and what was last sent to / heard from the TV.
    std::string castTrackSig;
    double   pendingSubDelay = 0.0;       // subtitle delay waiting to be sent to the TV
    uint64_t subDelayChangedAt = 0;
    int64_t  castSentAudio = -1, castSentSub = -1;
    uint64_t castSeenSerial = 0;

    // Starts casting what is loaded locally, from the current position.
    // startAt < 0: from where the local player is
    auto startCast = [&](const std::string& deviceId, double startAt = -1.0) {
        const PlayerState& st = player.state();
        if (!hasAirPlay) return;
        if (!airplay->castingAllowed()) {  // official build after the trial: AirPlay needs a license
            castTarget.clear();
            licenseError.clear();
            overlay.openLicenseDialog();
            return;
        }
        std::vector<CastTrack> tracks;
        for (const Track& t : st.tracks) {
            tracks.push_back({t.id, t.type, t.ffIndex, t.codec, t.lang, t.title, t.externalFile, t.selected});
            tracks.back().channels = t.channels;
        }
        for (const AirPlayTarget& d : airplayDevices) {
            if (d.deviceId != deviceId) continue;
            std::string reason;
            if (!airplay->canCast(st.path, tracks, reason)) {
                reason = T(reason.c_str());  // the module's English, translated when known
                if (castStatus.active() || castStatus.phase == CastStatus::Phase::Ended) {
                    // another video was picked while casting and the TV can't play it: end the
                    // TV session and keep the new video here, paused, instead of leaving the TV
                    // hanging and the computer playing on its own
                    airplay->stopCast();
                    castStatus = CastStatus{};
                    player.setPause(true);
                    reason += std::string(" ") + T("It stays on this computer, paused.");
                }
                overlay.showToast(reason);
                castTarget.clear();
                return;
            }
            castTarget = deviceId;
            std::fprintf(stderr, "[airplay] casting to %s\n", d.name.c_str());
            player.setPause(true);
            castTrackSig = trackSignature(st);
            castSentAudio = selectedTrack(st, "audio");
            castSentSub = selectedTrack(st, "sub");
            castSeenSerial = 0;
            castPath = st.path;
            airplay->startCast(d.deviceId, st.path, startAt >= 0 ? startAt : st.timePos, st.duration, tracks, st.subDelay);
            return;
        }
    };
    // Back to this computer, continuing where the TV was.
    auto stopCast = [&](bool resume) {
        if (!castStatus.active() && castTarget.empty()) return;
        const double pos = airplay->stopCast();
        castTarget.clear();
        castStatus = CastStatus{};
        if (!player.state().idle) {
            player.seekAbsolute(pos, true);
            player.setPause(!resume);
        }
    };
    auto castActive = [&] { return castStatus.active(); };
    // Opening another video starts it right away in mpv. While casting it must stay paused
    // here until it has been sent to the TV (or turned out not to be sendable).
    auto holdWhileCasting = [&] {
        if (castStatus.active() || !castTarget.empty()) player.setPause(true);
    };

    // tolga a.mp4 b.mp4 ...: the first plays, the others go into the playlist
    if (argc > 1) player.load(argv[1]);
    for (int i = 2; i < argc; ++i) player.append(argv[i]);

    bool running = true;
    bool fullscreen = false;
    int  redrawFrames = 2;
    int  fittedW = 0, fittedH = 0;  // video size the window was last fitted to
    int  subPos = 100;              // current mpv sub-pos  // ImGui needs a couple of frames to settle after input

    // moving the window by dragging anywhere on the video (like IINA)
    struct { bool armed = false, moving = false; float mouseX = 0, mouseY = 0; int winX = 0, winY = 0; } winDrag;

    auto toggleFullscreen = [&] {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(window, fullscreen);
    };

    // Draws one frame: video first, controls on top. Also called while the window is being
    // resized, when macOS holds the main loop and only an event watch gets to run.
    auto drawFrame = [&]() -> OverlayActions {
        const Uint64 tf0 = SDL_GetTicksNS();
        Uint64 tfRender = 0, tfDraw = 0, tfImgui = 0;
        int fbw = 0, fbh = 0;
        float pixelScale = 1.0f;
        if (!macDrawableSize(window, &fbw, &fbh, &pixelScale)) {
            SDL_GetWindowSizeInPixels(window, &fbw, &fbh);
            pixelScale = SDL_GetWindowPixelDensity(window);
        }
        overlay.setFramebuffer(fbw, fbh, pixelScale, uiScale(window));
        glViewport(0, 0, fbw, fbh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        const PlayerState& st = player.state();
        if (!st.idle) player.render(0, fbw, fbh);
        tfRender = SDL_GetTicksNS();

        airplayDevices = airplay->devices();
        licenseInfo = airplay->license();
        AirPlayView apView{&airplayDevices, airplay->searching(), castTarget, &castStatus};
        apView.available = hasAirPlay;
        apView.license = hasAirPlay ? &licenseInfo : nullptr;
        apView.licenseError = licenseError;
        // grey out the Apple TVs in the AirPlay menu when this video can't be sent (e.g. AV1);
        // checked again only when the file or its tracks change
        if (hasAirPlay && !st.idle && !st.tracks.empty() && !castStatus.active()) {
            const std::string key = st.path + "#" + std::to_string(st.tracks.size());
            if (key != castCheckKey) {
                castCheckKey = key;
                std::vector<CastTrack> tracks;
                for (const Track& t : st.tracks) {
                    tracks.push_back({t.id, t.type, t.ffIndex, t.codec, t.lang, t.title, t.externalFile, t.selected});
                    tracks.back().channels = t.channels;
                }
                castBlocked.clear();
                if (airplay->canCast(st.path, tracks, castBlocked)) castBlocked.clear();
            }
            apView.castBlocked = castBlocked;
        } else if (st.idle) {
            castCheckKey.clear();
        }
        onlineStatus = online.status();
        OnlineView onlineView{&onlineStatus, &onlineSettings, OpenSubtitles::queryFromFileName(st.path)};
        // While a video goes to the Apple TV the previews wait: on a slow computer they
        // would hold up the packaging, and the seek bar previews aren't needed right then.
        thumbnails.pause(castStatus.active() || !castTarget.empty());
        // seek-bar previews: start once the file's length and picture size are known
        if (!st.idle && st.duration > 0 && st.videoW > 0) thumbnails.start(st.path, st.duration, st.videoW, st.videoH);
        else if (st.idle) thumbnails.clear();

        PlaylistView playlistView;
        if (!st.idle && st.duration > 0) {
            durations.set(st.path, st.duration);
            const double pos = castStatus.active() ? castStatus.position : st.timePos;
            watched[st.path] = float(std::clamp(pos / st.duration, 0.0, 1.0));
        }
        for (const PlaylistEntry& e : st.playlist) {
            playlistView.durations.push_back(durations.get(e.path));
            auto w = watched.find(e.path);
            playlistView.progress.push_back(w == watched.end() ? 0.0f : w->second);
        }
#ifdef __APPLE__
        overlay.setTitlebarOverlay(true, fullscreen);
#endif
        const Uint64 tfBeforeDraw = SDL_GetTicksNS();
        overlay.setRecentFiles(st.idle ? &recent.existing() : nullptr);
        overlay.setDefaultPlayer(defaultAvailable, offerDefault, defaultNote);
        overlay.setCustomChrome(chrome::customFrame(), (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0);
        OverlayActions act = overlay.draw(st, fullscreen, apView, onlineView, playlistView);
        int winW = 0, winH = 0;
        SDL_GetWindowSize(window, &winW, &winH);
        if (const float uiW = ImGui::GetIO().DisplaySize.x; uiW > 0 && winW > 0)
            chrome::update(window, float(winW) / uiW, fullscreen);
        chrome::setDarkTitlebar(window, !overlay.lightTheme());
        tfDraw = SDL_GetTicksNS() - tfBeforeDraw;
        // the title bar buttons fade together with the controls
        macSetTitlebarAlpha(window, overlay.controlsAlpha());
        macSetAppearance(window, overlay.lightTheme());
        const Uint64 tfBeforeImgui = SDL_GetTicksNS();
        overlay.render();
        chrome::roundCorners(window, fbw, fbh, winW > 0 ? float(fbw) / float(winW) : 1.0f, fullscreen);
        tfImgui = SDL_GetTicksNS() - tfBeforeImgui;
        SDL_GL_SwapWindow(window);
        player.reportSwap();
        {
            static int frames = 0;
            const double total = (SDL_GetTicksNS() - tf0) / 1e6;
            if (g_trace && (frames < 15 || total > 100)) {
                ++frames;
                trace("frame %d: %.0f ms (mpv %.0f, controls %.0f, gl %.0f, swap+rest %.0f) %dx%d px, video %s", frames, total,
                      (tfRender - tf0) / 1e6, tfDraw / 1e6, tfImgui / 1e6,
                      total - (tfRender - tf0) / 1e6 - tfDraw / 1e6 - tfImgui / 1e6, fbw, fbh, st.idle ? "idle" : "loaded");
            }
        }
        return act;
    };
    std::function<void()> liveDraw = [&] { drawFrame(); };
    SDL_AddEventWatch(liveResizeWatch, &liveDraw);
    trace("main loop starts");
    Uint64 lastIter = SDL_GetTicksNS();

    while (running) {
        if (g_trace) {
            const Uint64 now = SDL_GetTicksNS();
            if (now - lastIter > 150000000ull) trace("main loop was busy %.0f ms", (now - lastIter) / 1e6);
            lastIter = now;
        }
        // Sleep until something happens. While the controls are fading, or ImGui
        // still needs frames, poll instead so animations stay smooth (vsync paces us).
        chrome::tick(window, !player.state().idle);  // Linux: the video's shape after a resize
        SDL_Event e;
        const bool busy = overlay.isAnimating() || redrawFrames > 0;
        // the cast screen breathes (Overlay::drawCastScreen): wake up for its next frame, unless
        // nobody can see it (window minimized or covered: a 2-hour film would keep the
        // graphics card drawing 30 frames a second for nothing)
        const bool windowHidden =
            (SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN)) != 0;
        const bool castScreen = castStatus.active() && !player.state().idle && !windowHidden;
        bool gotEvent = busy ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, castScreen ? 33 : 100);

        bool newFrame = false;
        while (gotEvent) {
            if (g_trace) {
                static std::map<Uint32, int> seen;
                if (seen[e.type]++ < 2 && e.type != g_wakeEvent && e.type != SDL_EVENT_MOUSE_MOTION)
                    trace("event 0x%x%s", unsigned(e.type), e.type == SDL_EVENT_WINDOW_EXPOSED ? " (window exposed: visible on screen)" : "");
            }
            if (e.type == g_wakeEvent) {
                bool fileLoaded = false;
                if (!player.processEvents(fileLoaded)) running = false;
                if (player.hasNewFrame()) newFrame = true;
                const PlayerState& st = player.state();
                // opened before: continue where it was left
                double resumeAt = -1;
                if (fileLoaded) {
                    resumeAt = positions.get(st.path);
                    if (resumeAt > 0 && (st.duration <= 0 || resumeAt < st.duration - 5)) {
                        player.seekAbsolute(resumeAt, true);
                        int t = int(resumeAt);
                        char at[32];
                        if (t >= 3600) std::snprintf(at, sizeof at, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
                        else           std::snprintf(at, sizeof at, "%d:%02d", t / 60, t % 60);
                        overlay.showToast(TF("Continuing from {}, where you left off.", {at}));
                    } else {
                        resumeAt = -1;
                    }
                }
                if (fileLoaded && !castTarget.empty()) {
                    // a new file was opened while casting: send it to the TV too (from where
                    // it was left: the jump above hasn't landed yet)
                    const std::string target = castTarget;
                    player.setPause(true);
                    startCast(target, resumeAt);
                }
                if (fileLoaded) {
                    trace("mpv: file loaded");
                    recent.add(st.path);
                    player.resetVideoSettings();  // aspect, crop, rotation... start fresh per file
                    overlay.resetVideoSettings();
                    fittedW = fittedH = 0;  // new file: fit again once its size is known
                    SDL_SetWindowTitle(window, st.title.empty() ? "Tolga Player" : st.title.c_str());
                }
                // dwidth/dheight can arrive a little after FILE_LOADED. Fit once per file only:
                // aspect / crop / rotation changes keep the window and letterbox instead.
                if (st.videoW > 0 && st.videoH > 0 && fittedW == 0) {
                    trace("video size %dx%d known, fitting the window", st.videoW, st.videoH);
                    fitWindowToVideo(window, st.videoW, st.videoH);
                    fittedW = st.videoW;
                    fittedH = st.videoH;
                }
                redrawFrames = std::max(redrawFrames, 1);  // time label etc.
            } else if (e.type == g_aboutEvent) {
                overlay.openAbout();
                redrawFrames = std::max(redrawFrames, 2);
            } else if (e.type == g_openEvent) {
                char* path = static_cast<char*>(e.user.data1);
                if (e.user.code == 1)      player.addSubtitle(path);
                else if (e.user.code == 2) player.append(path);
                else                       { player.load(path); holdWhileCasting(); }
                SDL_free(path);
            } else {
                overlay.processEvent(e);
                redrawFrames = 2;

                switch (e.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                    fullscreen = false;
                    macSetupWindow(window);  // SDL rebuilds the window style after fullscreen
                    break;
                case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                    fullscreen = true;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    // The default may have changed meanwhile: in Windows' settings after the
                    // button, or in Finder / the system settings for another player.
                    if (defaultAvailable) {
                        const bool isDef = defaultapp::isDefault();
                        if (isDef && offerDefault && !defaultNote.empty())
                            defaultNote = T("Done: double-clicking a video now opens Tolga Player.");
                        else if (!isDef && !offerDefault)
                            defaultNote.clear();  // another player took over: offer it again
                        offerDefault = !isDef;
                    }
                    break;
                case SDL_EVENT_WINDOW_RESIZED:
                    chrome::resized(window);
                    [[fallthrough]];
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    redrawFrames = std::max(redrawFrames, 2);  // draw again once the size has settled
                    break;
                case SDL_EVENT_DROP_FILE:
                    trace("drop/open file: %s", e.drop.data ? e.drop.data : "");
                    macBringToFront(window);  // opened from Finder: show the window now, not seconds later
                    // dropping an .srt onto a playing video adds it as a subtitle
                    if (isSubtitleFile(e.drop.data) && !player.state().idle)
                        player.addSubtitle(e.drop.data);
                    else
                        player.append(e.drop.data);  // dropped videos join the playlist (and play if idle)
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (e.button.button == SDL_BUTTON_LEFT && e.button.clicks == 2 &&
                        !overlay.wantsMouse() && !player.state().idle)
                        toggleFullscreen();
                    // pressing on the video (not on controls) may start moving the window
                    if (e.button.button == SDL_BUTTON_LEFT && e.button.clicks == 1 && !overlay.wantsMouse() &&
                        !fullscreen) {
                        winDrag.armed = true;
                        winDrag.moving = false;
                        SDL_GetGlobalMouseState(&winDrag.mouseX, &winDrag.mouseY);
                        SDL_GetWindowPosition(window, &winDrag.winX, &winDrag.winY);
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (winDrag.armed && (e.motion.state & SDL_BUTTON_LMASK)) {
                        float mx = 0, my = 0;
                        SDL_GetGlobalMouseState(&mx, &my);
                        const float dx = mx - winDrag.mouseX, dy = my - winDrag.mouseY;
                        // a few pixels of slack so a plain click (or double-click) stays a click
                        if (!winDrag.moving && dx * dx + dy * dy > 16.0f) winDrag.moving = true;
                        if (winDrag.moving)
                            SDL_SetWindowPosition(window, winDrag.winX + int(std::lround(dx)),
                                                  winDrag.winY + int(std::lround(dy)));
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        if (winDrag.moving) {  // land exactly where the mouse let go
                            float mx = 0, my = 0;
                            SDL_GetGlobalMouseState(&mx, &my);
                            SDL_SetWindowPosition(window, winDrag.winX + int(std::lround(mx - winDrag.mouseX)),
                                                  winDrag.winY + int(std::lround(my - winDrag.mouseY)));
                        }
                        winDrag.armed = winDrag.moving = false;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN: {
                    if (overlay.wantsKeyboard()) break;
                    overlay.poke();
                    const bool cmd = (e.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0;
                    switch (e.key.key) {
                    case SDLK_SPACE:
                        if (castActive()) airplay->setPaused(castStatus.phase != CastStatus::Phase::Paused);
                        else              player.togglePause();
                        break;
                    case SDLK_LEFT:
                        if (castActive()) airplay->seek(castStatus.position - 5);
                        else              player.seekRelative(-5);
                        break;
                    case SDLK_RIGHT:
                        if (castActive()) airplay->seek(castStatus.position + 5);
                        else              player.seekRelative(5);
                        break;
                    case SDLK_UP:     player.setVolume(std::min(100.0, player.state().volume + 5)); break;
                    case SDLK_DOWN:   player.setVolume(std::max(0.0, player.state().volume - 5)); break;
                    case SDLK_M:      player.toggleMute(); break;
                    case SDLK_F:      toggleFullscreen(); break;
                    case SDLK_V:      player.toggleSubVisibility(); break;
                    case SDLK_J:      player.cycleSubtitle(); break;
                    case SDLK_O:      if (cmd) showOpenDialog(window); break;
                    case SDLK_Q:      if (cmd) running = false; break;
                    case SDLK_ESCAPE:
                        if (overlay.licenseOpen())       overlay.closeLicenseDialog();
                        else if (overlay.resultsOpen())  overlay.closeResults();
                        else if (overlay.panelOpen())    overlay.closePanel();
                        else if (overlay.playlistOpen()) overlay.closePlaylist();
                        else if (fullscreen)     toggleFullscreen();
                        break;
                    case SDLK_S: if (!cmd) overlay.togglePanel(Overlay::PanelTab::Subtitles); break;
                    case SDLK_A: if (!cmd) overlay.togglePanel(Overlay::PanelTab::Audio); break;
                    case SDLK_P: if (!cmd) overlay.togglePlaylist(); break;
                    case SDLK_N: if (!cmd) { player.playNext(); holdWhileCasting(); } break;
                    default: break;
                    }
                    break;
                }
                default:
                    break;
                }
            }
            gotEvent = SDL_PollEvent(&e);
        }

        if (!running) break;

        // ---- a subtitle downloaded from OpenSubtitles: add it and show it
        {
            std::string lang;
            const std::string sub = online.takeDownloaded(&lang);
            if (!sub.empty() && !player.state().idle) {
                std::string name = sub.substr(sub.find_last_of("/\\") + 1);
                player.addSubtitle(sub, name, lang);
                player.setSubVisibility(true);
                overlay.showToast(online.status().message);
                overlay.showSubtitleList();
                redrawFrames = std::max(redrawFrames, 2);
            }
        }

        // ---- casting status (updated from the network thread, polled here)
        castStatus = airplay->status();
        // where the video is now (on the TV while casting), to continue from there next time
        if (const PlayerState& pst = player.state(); !pst.idle && pst.duration > 0) {
            if (!castStatus.active()) positions.update(pst.path, pst.timePos, pst.duration);
            else if (pst.path == castPath) positions.update(pst.path, castStatus.position, pst.duration);
        }
        if (castStatus.notice != lastCastNotice) {
            if (!castStatus.notice.empty()) overlay.showToast(T(castStatus.notice.c_str()));
            lastCastNotice = castStatus.notice;
        }
        if (castStatus.phase != lastCastPhase) {
            using P = CastStatus::Phase;
            if (castStatus.phase == P::Failed) {
                overlay.showToast(T(castStatus.error.c_str()));  // the module's English, translated when known
                stopCast(false);
            } else if (castStatus.phase == P::Ended) {
                const std::string name = castStatus.deviceName;
                if (player.playNext()) {
                    player.setPause(true);  // the file-loaded handler sends it to the TV
                    // next video of the playlist: keep casting (the file-loaded handler sends it)
                    overlay.showToast(TF("Playing the next video on {}.", {name}));
                    watched[player.state().path] = 1.0f;
                } else {
                    overlay.showToast(TF("Finished playing on {}.", {name}));
                    airplay->stopCast();
                    castTarget.clear();
                    castStatus = CastStatus{};
                    if (!player.state().idle) player.seekAbsolute(0, true);  // stay paused at the start
                }
            }
            lastCastPhase = castStatus.phase;
            redrawFrames = std::max(redrawFrames, 1);
        }
        // ---- keep audio/subtitle tracks in step between Tolga and the TV
        if (castStatus.active() && !castTarget.empty()) {
            const PlayerState& pst = player.state();
            using P = CastStatus::Phase;
            const bool live = castStatus.phase == P::Playing || castStatus.phase == P::Paused;
            if (!pst.idle && trackSignature(pst) != castTrackSig && (live || castStatus.phase == P::Loading)) {
                // a subtitle file was added: package again and continue on the TV where it is
                std::fprintf(stderr, "[airplay] tracks changed, sending the video again\n");
                const double pos = castStatus.position;
                const std::string target = castTarget;
                player.seekAbsolute(pos, true);
                startCast(target, pos);
            } else if (castStatus.hls && live && std::abs(pst.subDelay - castStatus.subtitleDelay) > 0.05) {
                // Subtitle delay on the TV: its subtitles are rewritten with the new timing and
                // the video is sent again from where it is. Wait until the slider rests first.
                if (std::abs(pst.subDelay - pendingSubDelay) > 0.001) {
                    pendingSubDelay = pst.subDelay;
                    subDelayChangedAt = SDL_GetTicks();
                } else if (SDL_GetTicks() - subDelayChangedAt > 700) {
                    std::fprintf(stderr, "[airplay] subtitle delay %.1f s, sending the video again\n", pst.subDelay);
                    const double pos = castStatus.position;
                    const std::string target = castTarget;
                    player.seekAbsolute(pos, true);
                    overlay.showToast(TF("Updating subtitle timing on {}...", {castStatus.deviceName}));
                    startCast(target, pos);
                }
                redrawFrames = std::max(redrawFrames, 1);
            } else if (castStatus.hls && live) {
                if (castStatus.selectionSerial != castSeenSerial) {
                    // the TV reports its tracks (e.g. changed with the Siri remote): mirror them here
                    castSeenSerial = castStatus.selectionSerial;
                    castSentAudio = castStatus.tvAudio;
                    castSentSub = castStatus.tvSub;
                    if (castStatus.tvAudio >= 0 && castStatus.tvAudio != selectedTrack(pst, "audio"))
                        player.selectAudio(castStatus.tvAudio);
                    if (castStatus.tvSub != selectedTrack(pst, "sub")) {
                        player.selectSubtitle(castStatus.tvSub);
                        if (castStatus.tvSub >= 0 && !pst.subVisible) player.setSubVisibility(true);
                    }
                } else {
                    const int64_t aid = selectedTrack(pst, "audio"), sid = selectedTrack(pst, "sub");
                    if (aid != castSentAudio && aid >= 0) { airplay->selectAudio(aid); castSentAudio = aid; }
                    if (sid != castSentSub)               { airplay->selectSubtitle(sid); castSentSub = sid; }
                }
            }
        }

        // keep the time label ticking while the TV plays (a few times per second is plenty);
        // about 30 times a second while the cast screen shows, for its animation
        const Uint64 castEvery = player.state().idle ? 250 : 33;
        if (castStatus.active() && !windowHidden && SDL_GetTicks() - lastCastRedraw >= castEvery) {
            lastCastRedraw = SDL_GetTicks();
            redrawFrames = std::max(redrawFrames, 1);
        }

        if (!newFrame && redrawFrames <= 0 && !overlay.isAnimating()) continue;
        if (redrawFrames > 0) --redrawFrames;

        // ---- render
        OverlayActions act = drawFrame();
        const PlayerState& st = player.state();
        if (act.openLicense) { licenseError.clear(); overlay.openLicenseDialog(); }
        if (act.closeLicense) overlay.closeLicenseDialog();
        if (act.openWebsite) SDL_OpenURL(TOLGA_WEBSITE);
        if (act.openBuyPage) {
            const std::string url = hasAirPlay ? licenseInfo.buyUrl : std::string(TOLGA_WEBSITE);
            if (!url.empty()) SDL_OpenURL(url.c_str());
        }
        if (act.activateLicense) {
            if (airplay->activate(act.licenseKey, licenseError)) {
                overlay.closeLicenseDialog();
                overlay.showToast(TF("Thank you! Tolga is licensed to {}.", {airplay->license().email}));
            }
        }

        // hide the mouse cursor together with the controls during playback
        if (overlay.controlsHidden() && !st.idle) SDL_HideCursor();
        else                                      SDL_ShowCursor();

        // ---- apply what the user did in the overlay
        if (act.togglePause) {
            if (castActive()) airplay->setPaused(castStatus.phase != CastStatus::Phase::Paused);
            else              player.togglePause();
        }
        if (act.toggleMute)        player.toggleMute();
        if (act.toggleFullscreen)  toggleFullscreen();
        if (act.minimizeWindow)    SDL_MinimizeWindow(window);
        if (act.toggleMaximize) {
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window);
            else                                                   SDL_MaximizeWindow(window);
        }
        if (act.closeWindow) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        if (act.openFile)          showOpenDialog(window);
        if (!act.openPath.empty()) { player.load(act.openPath.c_str()); holdWhileCasting(); }
        if (act.clearRecent)       recent.clear();
        if (act.makeDefaultPlayer) {
            defaultNote = defaultapp::makeDefault();
            offerDefault = !defaultapp::isDefault();
        }
        if (act.seekRelative != 0) {
            if (castActive()) airplay->seek(castStatus.position + act.seekRelative);
            else              player.seekRelative(act.seekRelative);
        }
        if (act.seekAbsolute) {
            // the TV gets one seek when the bar is released, not a stream while dragging
            if (castActive()) { if (act.seekExact) airplay->seek(act.seekTo); }
            else              player.seekAbsolute(act.seekTo, act.seekExact);
        }
        if (act.airplayMenuOpened) airplay->refresh();
        if (act.selectCastTarget) {
            if (act.castTarget.empty())               stopCast(true);
            else if (act.castTarget != castTarget ||
                     !castActive())                   startCast(act.castTarget);
        }
        if (act.submitPin) airplay->submitPin(act.pin);
        if (act.cancelPin) stopCast(false);
        if (act.loadSubFile)       showSubtitleDialog(window);
        if (act.selectSub)         player.selectSubtitle(act.subId);
        if (act.selectAudio)       player.selectAudio(act.audioId);
        if (act.selectSecondarySub)  player.selectSecondarySubtitle(act.secondarySubId);
        if (act.setSubVisible)       player.setSubVisibility(act.subVisible);
        if (act.setSecondaryVisible) player.setSecondarySubVisibility(act.secondaryVisible);
        if (act.selectVideo)         player.selectVideo(act.videoId);
        if (act.setAudioDelay)       player.setAudioDelay(act.audioDelay);
        if (act.setSpeed)            player.setSpeed(act.speed);
        if (act.setAspect)           player.setAspect(act.aspect);
        if (act.setCrop)             player.setCrop(act.crop);
        if (act.setRotate)           player.setRotate(act.rotate);
        if (act.setHwdec)            player.setHardwareDecoding(act.hwdec);
        if (act.pictureProperty)     player.setPicture(act.pictureProperty, act.pictureValue);
        if (act.setSubDelay)         player.setSubDelay(act.subDelay);
        if (act.setEqualizer)        player.setEqualizer(act.eqGains);
        if (act.playlistPlay >= 0)   { player.playIndex(act.playlistPlay); holdWhileCasting(); }
        if (act.playlistRemove >= 0) player.removeIndex(act.playlistRemove);
        if (act.playlistClear)       player.clearPlaylist();
        if (act.playlistAdd)         showOpenDialog(window, true);
        if (act.playlistMoveFrom >= 0) player.movePlaylistEntry(act.playlistMoveFrom, act.playlistMoveTo);
        if (act.playlistSort)        player.sortPlaylistByName();
        if (act.toggleShuffle)       player.setShuffle(!st.shuffled);
        if (act.cycleRepeat) {
            // off -> whole playlist -> this video -> off
            if (!st.loopFile && !st.loopPlaylist) player.setRepeat(false, true);
            else if (st.loopPlaylist)             player.setRepeat(true, false);
            else                                  player.setRepeat(false, false);
        }
        if (act.seekChapter >= 0) {
            if (castActive() && act.seekChapter < int(st.chapters.size())) airplay->seek(st.chapters[size_t(act.seekChapter)].time);
            else                                                            player.seekChapter(act.seekChapter);
        }
        if (act.onlineSaveSettings) {
            onlineSettings.apiKey = act.onlineApiKey;
            onlineSettings.username = act.onlineUser;
            onlineSettings.password = act.onlinePassword;
            online.saveSettings(onlineSettings);
        }
        if (act.onlineSearch) {
            onlineSettings.languages = act.onlineLanguages;  // remembered for next time
            online.saveSettings(onlineSettings);
            // the hash only works for local files
            const bool local = st.path.find("://") == std::string::npos;
            online.search(local ? st.path : std::string(), act.onlineQuery, act.onlineLanguages);
        }
        if (act.onlineDownload >= 0 && act.onlineDownload < int(onlineStatus.results.size()))
            online.download(onlineStatus.results[size_t(act.onlineDownload)]);

        // Lift subtitles above the control bar while it is shown, like IINA does.
        int wantPos = 100;
        if (overlay.controlsShown() && !st.idle) {
            int winW = 0, winH = 0;
            SDL_GetWindowSize(window, &winW, &winH);
            if (winH > 0)
                wantPos = std::clamp(100 - int(100.0f * Overlay::controlsHeight() * uiScale(window) / winH), 50, 100);
        }
        if (wantPos != subPos) {
            player.setSubPos(wantPos);
            subPos = wantPos;
        }
        if (act.setVolume) {
            player.setVolume(act.volume);
            if (st.muted) player.setMute(false);
        }
    }

    SDL_RemoveEventWatch(liveResizeWatch, &liveDraw);
    positions.flush();
    airplay->shutdown();
    overlay.shutdown();
    player.shutdown();  // needs the GL context still alive
    g_waker.stop();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
