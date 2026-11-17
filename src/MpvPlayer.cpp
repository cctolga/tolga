#include "MpvPlayer.h"
#include "I18n.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>

namespace {
enum PropId : uint64_t {
    kTimePos = 1, kDuration, kPause, kVolume, kMute, kIdle, kTitle, kDWidth, kDHeight,
    kTrackList, kSubVisible, kPath, kSecondarySid, kSecondaryVisible, kAudioDelay, kSubDelay,
    kSpeed, kRotate, kHwdec, kBrightness, kContrast, kSaturation, kGamma, kHue,
    kPlaylist, kPlaylistPos, kChapterList, kChapter, kLoopFile, kLoopPlaylist, kShuffle
};
}

void MpvPlayer::onWakeup(void* self) {
    auto* p = static_cast<MpvPlayer*>(self);
    if (p->m_wakeup) p->m_wakeup();
}

namespace {
#if defined(__linux__)
// NVIDIA: NVDEC straight to OpenGL. Intel/AMD: VA-API, copied back to the CPU; the LGPL
// libmpv has no X11/Wayland code for handing VA-API frames to OpenGL directly.
const char* const kHwdecMode = "nvdec,vaapi-copy,auto-copy-safe";
#elif defined(_WIN32)
// D3D11 (any GPU) or NVDEC, copied back: this libmpv draws through OpenGL without D3D interop
const char* const kHwdecMode = "auto-copy-safe";
#else
const char* const kHwdecMode = "auto-safe";   // VideoToolbox
#endif
}  // namespace

bool MpvPlayer::init(GetProcAddressFn getProcAddress, std::function<void()> wakeup) {
    m_wakeup = std::move(wakeup);

    m_mpv = mpv_create();
    if (!m_mpv) {
        std::fprintf(stderr, "mpv_create failed\n");
        return false;
    }

    // Playback options. Errors on optional ones are ignored.
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", kHwdecMode);
    mpv_set_option_string(m_mpv, "keep-open", "yes");     // stay on last frame at the end
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "osc", "no");            // we draw our own controls
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "terminal", "yes");
    // TOLGA_MPV_LOG=v (or debug) enables verbose mpv logging for troubleshooting.
    const char* logLevel = std::getenv("TOLGA_MPV_LOG");
    mpv_set_option_string(m_mpv, "msg-level", (std::string("all=") + (logLevel ? logLevel : "warn")).c_str());

    // Subtitles: pick up "Movie.tr.srt", "Movie.srt" etc. from the video's folder,
    // prefer Turkish then English, and give them a clean, readable look.
    mpv_set_option_string(m_mpv, "sub-auto", "fuzzy");
    mpv_set_option_string(m_mpv, "slang", "tr,tur,en,eng");
    // UTF-8 files are always read as UTF-8; anything else is assumed to be Turkish
    // Windows-1254 (auto-detection mistakes it for Latin-1: "Ð" instead of "Ğ").
    // TODO: make this a setting for other languages (cp1251 for Russian, etc.)
    mpv_set_option_string(m_mpv, "sub-codepage", "cp1254");
    mpv_set_option_string(m_mpv, "sub-font-size", "44");
    if (mpv_set_option_string(m_mpv, "sub-outline-size", "2.2") < 0)  // newer mpv
        mpv_set_option_string(m_mpv, "sub-border-size", "2.2");       // older name
    mpv_set_option_string(m_mpv, "sub-shadow-offset", "1");
    mpv_set_option_string(m_mpv, "sub-shadow-color", "#80000000");
    mpv_set_option_string(m_mpv, "sub-margin-y", "36");

    if (mpv_initialize(m_mpv) < 0) {
        std::fprintf(stderr, "mpv_initialize failed\n");
        return false;
    }

    mpv_opengl_init_params glInit{getProcAddress, nullptr};
    // No MPV_RENDER_PARAM_ADVANCED_CONTROL: we render on the main thread, which also makes
    // ordinary (blocking) mpv calls. With advanced control the core may wait for the render
    // thread while that thread waits for the core, and both freeze for good (seen while
    // resizing the window: sound kept playing, picture and app hung). Without it mpv falls
    // back to short timeouts instead, at the cost of one extra copy per frame.
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&m_render, m_mpv, params) < 0) {
        std::fprintf(stderr, "mpv_render_context_create failed\n");
        return false;
    }

    mpv_observe_property(m_mpv, kTimePos,  "time-pos",    MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, kDuration, "duration",    MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, kPause,    "pause",       MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, kVolume,   "volume",      MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, kMute,     "mute",        MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, kIdle,     "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, kTitle,    "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, kDWidth,   "dwidth",      MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, kDHeight,  "dheight",     MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, kTrackList, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, kSubVisible, "sub-visibility", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, kSecondarySid, "secondary-sid", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, kAudioDelay, "audio-delay", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, kSubDelay, "sub-delay", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, kSpeed, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, kPlaylist, "playlist", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, kPlaylistPos, "playlist-pos", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, kChapterList, "chapter-list", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, kChapter, "chapter", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, kLoopFile, "loop-file", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, kLoopPlaylist, "loop-playlist", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, kShuffle, "shuffle", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, kRotate, "video-rotate", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, kHwdec, "hwdec", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, kBrightness, "brightness", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, kContrast, "contrast", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, kSaturation, "saturation", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, kGamma, "gamma", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, kHue, "hue", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, kSecondaryVisible, "secondary-sub-visibility", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, kPath,     "path",        MPV_FORMAT_STRING);

    mpv_set_wakeup_callback(m_mpv, &MpvPlayer::onWakeup, this);
    mpv_render_context_set_update_callback(m_render, &MpvPlayer::onWakeup, this);
    return true;
}

void MpvPlayer::shutdown() {
    // The render context must be freed before the mpv handle, with the GL context current.
    if (m_render) {
        mpv_render_context_free(m_render);
        m_render = nullptr;
    }
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}
// All property changes go through mpv's async API: a synchronous call that has to
// reconfigure the video output can deadlock with the render API, which runs on this
// same thread (mpv waits for a frame that we can't render while blocked).
static void setString(mpv_handle* mpv, const char* name, const std::string& value) {
    const char* v = value.c_str();  // mpv copies the value before returning
    mpv_set_property_async(mpv, 0, name, MPV_FORMAT_STRING, &v);
}
static void setDouble(mpv_handle* mpv, const char* name, double value) {
    mpv_set_property_async(mpv, 0, name, MPV_FORMAT_DOUBLE, &value);
}
static void setInt(mpv_handle* mpv, const char* name, int64_t value) {
    mpv_set_property_async(mpv, 0, name, MPV_FORMAT_INT64, &value);
}


void MpvPlayer::command(const char* const* args) {
    mpv_command_async(m_mpv, 0, const_cast<const char**>(args));
}

void MpvPlayer::load(const std::string& path) {
    const char* args[] = {"loadfile", path.c_str(), nullptr};
    command(args);
    setString(m_mpv, "pause", "no");
}

void MpvPlayer::togglePause() {
    const char* args[] = {"cycle", "pause", nullptr};
    command(args);
}

void MpvPlayer::setPause(bool paused) {
    setString(m_mpv, "pause", paused ? "yes" : "no");
}

void MpvPlayer::seekRelative(double seconds) {
    std::string s = std::to_string(seconds);
    const char* args[] = {"seek", s.c_str(), "relative", nullptr};
    command(args);
}

void MpvPlayer::seekAbsolute(double seconds, bool exact) {
    std::string s = std::to_string(seconds);
    const char* args[] = {"seek", s.c_str(), exact ? "absolute+exact" : "absolute+keyframes", nullptr};
    command(args);
}

void MpvPlayer::setVolume(double volume) {
    mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE, &volume);
}

void MpvPlayer::toggleMute() {
    const char* args[] = {"cycle", "mute", nullptr};
    command(args);
}

void MpvPlayer::setMute(bool mute) {
    int flag = mute ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "mute", MPV_FORMAT_FLAG, &flag);
}

void MpvPlayer::selectSubtitle(int64_t id) {
    std::string v = id < 0 ? "no" : std::to_string(id);
    const char* cv = v.c_str();  // mpv copies the value before returning
    mpv_set_property_async(m_mpv, 0, "sid", MPV_FORMAT_STRING, &cv);
}

void MpvPlayer::selectAudio(int64_t id) {
    std::string v = id < 0 ? "no" : std::to_string(id);
    const char* cv = v.c_str();
    mpv_set_property_async(m_mpv, 0, "aid", MPV_FORMAT_STRING, &cv);
}

void MpvPlayer::addSubtitle(const std::string& path, const std::string& title, const std::string& lang) {
    if (title.empty() && lang.empty()) {
        const char* args[] = {"sub-add", path.c_str(), "select", nullptr};
        command(args);
    } else {
        const char* args[] = {"sub-add", path.c_str(), "select", title.c_str(), lang.c_str(), nullptr};
        command(args);
    }
}

void MpvPlayer::toggleSubVisibility() {
    const char* args[] = {"cycle", "sub-visibility", nullptr};
    command(args);
}

void MpvPlayer::setSubVisibility(bool visible) {
    setString(m_mpv, "sub-visibility", visible ? "yes" : "no");
}

void MpvPlayer::selectSecondarySubtitle(int64_t id) {
    const std::string v = id < 0 ? "no" : std::to_string(id);
    setString(m_mpv, "secondary-sid", v.c_str());
}

void MpvPlayer::setSecondarySubVisibility(bool visible) {
    setString(m_mpv, "secondary-sub-visibility", visible ? "yes" : "no");
}

void MpvPlayer::selectVideo(int64_t id) {
    const std::string v = id < 0 ? "no" : std::to_string(id);
    setString(m_mpv, "vid", v.c_str());
}

void MpvPlayer::setAudioDelay(double seconds) {
    setDouble(m_mpv, "audio-delay", seconds);
}

void MpvPlayer::setSubDelay(double seconds) {
    setDouble(m_mpv, "sub-delay", seconds);
}

void MpvPlayer::setSpeed(double speed) {
    setDouble(m_mpv, "speed", speed);
}

void MpvPlayer::setAspect(const std::string& ratio) {
    setString(m_mpv, "video-aspect-override", ratio.empty() ? "no" : ratio.c_str());
}

void MpvPlayer::setCrop(const std::string& ratio) {
    // the source size of the playing video track
    int w = 0, h = 0;
    for (const Track& t : m_state.tracks)
        if (t.type == "video" && t.selected) { w = t.width; h = t.height; }
    int num = 0, den = 0;
    if (ratio.empty() || w <= 0 || h <= 0 || std::sscanf(ratio.c_str(), "%d:%d", &num, &den) != 2 || den <= 0) {
        setString(m_mpv, "video-crop", "");
        return;
    }
    const double r = double(num) / den;
    int cw = w, ch = h;
    if (double(w) / h > r) cw = int(h * r + 0.5) & ~1;  // too wide: cut the sides
    else                   ch = int(w / r + 0.5) & ~1;  // too tall: cut top and bottom
    char crop[64];
    std::snprintf(crop, sizeof crop, "%dx%d+%d+%d", cw, ch, (w - cw) / 2, (h - ch) / 2);
    setString(m_mpv, "video-crop", crop);
}

void MpvPlayer::setRotate(int degrees) {
    setInt(m_mpv, "video-rotate", degrees);
}

void MpvPlayer::setHardwareDecoding(bool on) {
    setString(m_mpv, "hwdec", on ? kHwdecMode : "no");
}

void MpvPlayer::setPicture(const char* property, int value) {
    setInt(m_mpv, property, value);
}

void MpvPlayer::resetVideoSettings() {
    setAspect("");
    setString(m_mpv, "video-crop", "");
    setRotate(0);
    setSpeed(1.0);
    for (const char* p : {"brightness", "contrast", "saturation", "gamma", "hue"}) setPicture(p, 0);
}

const float MpvPlayer::kEqFrequencies[MpvPlayer::kEqBands] = {32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

void MpvPlayer::setEqualizer(const float gains[kEqBands]) {
    // one ffmpeg "equalizer" peak filter per band, one octave wide, as IINA does
    std::string graph;
    bool any = false;
    for (int i = 0; i < kEqBands; ++i) {
        if (std::abs(gains[i]) < 0.05f) continue;
        char band[96];
        std::snprintf(band, sizeof band, "%sequalizer=f=%g:t=o:w=1:g=%.1f", any ? "," : "", kEqFrequencies[i], gains[i]);
        graph += band;
        any = true;
    }
    const std::string af = any ? "@tolgaeq:lavfi=[" + graph + "]" : "";
    setString(m_mpv, "af", af.c_str());
}

void MpvPlayer::cycleSubtitle() {
    const char* args[] = {"cycle", "sub", nullptr};
    command(args);
}

void MpvPlayer::setSubPos(int pos) {
    int64_t v = pos;
    mpv_set_property_async(m_mpv, 0, "sub-pos", MPV_FORMAT_INT64, &v);
}

std::string Track::label() const {
    std::string s;
    if (!lang.empty()) s = lang;
    if (!title.empty()) s += (s.empty() ? "" : " - ") + title;
    if (s.empty()) s = TF("Track {}", {std::to_string(id)});
    if (external) s += std::string(" (") + T("file") + ")";
    return s;
}

namespace {
const mpv_node* mapGet(const mpv_node& map, const char* key) {
    if (map.format != MPV_FORMAT_NODE_MAP) return nullptr;
    for (int k = 0; k < map.u.list->num; ++k)
        if (std::strcmp(map.u.list->keys[k], key) == 0) return &map.u.list->values[k];
    return nullptr;
}
}  // namespace

void MpvPlayer::readPlaylist(const mpv_node& list) {
    if (list.format != MPV_FORMAT_NODE_ARRAY) return;
    for (int i = 0; i < list.u.list->num; ++i) {
        const mpv_node& item = list.u.list->values[i];
        PlaylistEntry e;
        if (const mpv_node* v = mapGet(item, "filename"); v && v->format == MPV_FORMAT_STRING) e.path = v->u.string;
        if (const mpv_node* v = mapGet(item, "title"); v && v->format == MPV_FORMAT_STRING) e.title = v->u.string;
        if (const mpv_node* v = mapGet(item, "current"); v && v->format == MPV_FORMAT_FLAG) e.current = v->u.flag;
        m_state.playlist.push_back(std::move(e));
    }
}

void MpvPlayer::readChapters(const mpv_node& list) {
    if (list.format != MPV_FORMAT_NODE_ARRAY) return;
    for (int i = 0; i < list.u.list->num; ++i) {
        const mpv_node& item = list.u.list->values[i];
        Chapter c;
        if (const mpv_node* v = mapGet(item, "title"); v && v->format == MPV_FORMAT_STRING) c.title = v->u.string;
        if (const mpv_node* v = mapGet(item, "time"); v && v->format == MPV_FORMAT_DOUBLE) c.time = v->u.double_;
        if (c.title.empty()) c.title = TF("Chapter {}", {std::to_string(i + 1)});
        m_state.chapters.push_back(std::move(c));
    }
}

void MpvPlayer::append(const std::string& path) {
    const char* args[] = {"loadfile", path.c_str(), "append-play", nullptr};
    command(args);
}

void MpvPlayer::playIndex(int index) {
    setInt(m_mpv, "playlist-pos", index);
    setString(m_mpv, "pause", "no");
}

void MpvPlayer::removeIndex(int index) {
    const std::string i = std::to_string(index);
    const char* args[] = {"playlist-remove", i.c_str(), nullptr};
    command(args);
}

void MpvPlayer::clearPlaylist() {
    const char* args[] = {"playlist-clear", nullptr};
    command(args);
}

void MpvPlayer::movePlaylistEntry(int from, int to) {
    const std::string a = std::to_string(from), b = std::to_string(to);
    const char* args[] = {"playlist-move", a.c_str(), b.c_str(), nullptr};
    command(args);
}

void MpvPlayer::sortPlaylistByName() {
    // selection sort with playlist-move; mpv runs the async commands in order
    std::vector<std::string> names;
    for (const PlaylistEntry& e : m_state.playlist) {
        std::string n = e.path.substr(e.path.find_last_of("/\\") + 1);
        for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
        names.push_back(n);
    }
    for (size_t i = 0; i < names.size(); ++i) {
        size_t best = i;
        for (size_t j = i + 1; j < names.size(); ++j)
            if (names[j] < names[best]) best = j;
        if (best != i) {
            movePlaylistEntry(int(best), int(i));
            const std::string moved = names[best];
            names.erase(names.begin() + long(best));
            names.insert(names.begin() + long(i), moved);
        }
    }
}

void MpvPlayer::setShuffle(bool on) {
    const char* args[] = {on ? "playlist-shuffle" : "playlist-unshuffle", nullptr};
    command(args);
    setString(m_mpv, "shuffle", on ? "yes" : "no");
}

void MpvPlayer::setRepeat(bool file, bool playlist) {
    setString(m_mpv, "loop-file", file ? "inf" : "no");
    setString(m_mpv, "loop-playlist", playlist ? "inf" : "no");
}

bool MpvPlayer::playNext() {
    if (m_state.playlistPos < 0 || m_state.playlistPos + 1 >= int(m_state.playlist.size())) return false;
    playIndex(m_state.playlistPos + 1);
    return true;
}

void MpvPlayer::seekChapter(int index) {
    setInt(m_mpv, "chapter", index);
}

void MpvPlayer::readTrackList(const mpv_node& list) {
    m_state.tracks.clear();
    if (list.format != MPV_FORMAT_NODE_ARRAY) return;
    for (int i = 0; i < list.u.list->num; ++i) {
        const mpv_node& item = list.u.list->values[i];
        if (item.format != MPV_FORMAT_NODE_MAP) continue;
        Track t;
        for (int k = 0; k < item.u.list->num; ++k) {
            const std::string key = item.u.list->keys[k];
            const mpv_node& v = item.u.list->values[k];
            if      (key == "id"       && v.format == MPV_FORMAT_INT64)  t.id = v.u.int64;
            else if (key == "type"     && v.format == MPV_FORMAT_STRING) t.type = v.u.string;
            else if (key == "title"    && v.format == MPV_FORMAT_STRING) t.title = v.u.string;
            else if (key == "lang"     && v.format == MPV_FORMAT_STRING) t.lang = v.u.string;
            else if (key == "selected" && v.format == MPV_FORMAT_FLAG)   t.selected = v.u.flag;
            else if (key == "external" && v.format == MPV_FORMAT_FLAG)   t.external = v.u.flag;
            else if (key == "ff-index" && v.format == MPV_FORMAT_INT64)  t.ffIndex = v.u.int64;
            else if (key == "codec"    && v.format == MPV_FORMAT_STRING) t.codec = v.u.string;
            else if (key == "external-filename" && v.format == MPV_FORMAT_STRING) t.externalFile = v.u.string;
            else if (key == "default"  && v.format == MPV_FORMAT_FLAG)   t.isDefault = v.u.flag;
            else if (key == "forced"   && v.format == MPV_FORMAT_FLAG)   t.forced = v.u.flag;
            else if (key == "demux-channel-count" && v.format == MPV_FORMAT_INT64) t.channels = int(v.u.int64);
            else if (key == "demux-w"  && v.format == MPV_FORMAT_INT64)  t.width = int(v.u.int64);
            else if (key == "demux-h"  && v.format == MPV_FORMAT_INT64)  t.height = int(v.u.int64);
            else if (key == "demux-fps" && v.format == MPV_FORMAT_DOUBLE) t.fps = v.u.double_;
        }
        m_state.tracks.push_back(std::move(t));
    }
}

bool MpvPlayer::processEvents(bool& fileLoaded) {
    while (true) {
        mpv_event* ev = mpv_wait_event(m_mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;

        switch (ev->event_id) {
        case MPV_EVENT_SHUTDOWN:
            return false;
        case MPV_EVENT_FILE_LOADED:
            fileLoaded = true;
            break;
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto* prop = static_cast<mpv_event_property*>(ev->data);
            const bool has = prop->format != MPV_FORMAT_NONE && prop->data;
            switch (ev->reply_userdata) {
            case kTimePos:  m_state.timePos  = has ? *static_cast<double*>(prop->data) : 0.0; break;
            case kDuration: m_state.duration = has ? *static_cast<double*>(prop->data) : 0.0; break;
            case kVolume:   m_state.volume   = has ? *static_cast<double*>(prop->data) : 100.0; break;
            case kPause:    m_state.paused   = has && *static_cast<int*>(prop->data); break;
            case kMute:     m_state.muted    = has && *static_cast<int*>(prop->data); break;
            case kIdle:     m_state.idle     = has && *static_cast<int*>(prop->data); break;
            case kTitle:    m_state.title    = has ? *static_cast<char**>(prop->data) : ""; break;
            case kDWidth:   m_state.videoW   = has ? int(*static_cast<int64_t*>(prop->data)) : 0; break;
            case kDHeight:  m_state.videoH   = has ? int(*static_cast<int64_t*>(prop->data)) : 0; break;
            case kPath:     m_state.path     = has ? *static_cast<char**>(prop->data) : ""; break;
            case kSubVisible: m_state.subVisible = !has || *static_cast<int*>(prop->data); break;
            case kPlaylist:
                m_state.playlist.clear();
                if (has) readPlaylist(*static_cast<mpv_node*>(prop->data));
                break;
            case kPlaylistPos: m_state.playlistPos = has ? int(*static_cast<int64_t*>(prop->data)) : -1; break;
            case kChapterList:
                m_state.chapters.clear();
                if (has) readChapters(*static_cast<mpv_node*>(prop->data));
                break;
            case kChapter:  m_state.chapter = has ? int(*static_cast<int64_t*>(prop->data)) : -1; break;
            case kLoopFile:
                m_state.loopFile = has && std::string(*static_cast<char**>(prop->data)) != "no";
                break;
            case kLoopPlaylist:
                m_state.loopPlaylist = has && std::string(*static_cast<char**>(prop->data)) != "no";
                break;
            case kShuffle:  m_state.shuffled = has && *static_cast<int*>(prop->data); break;
            case kSpeed:      m_state.speed = has ? *static_cast<double*>(prop->data) : 1.0; break;
            case kRotate:     m_state.rotate = has ? int(*static_cast<int64_t*>(prop->data)) : 0; break;
            case kHwdec:      m_state.hwdec = !has || std::string(*static_cast<char**>(prop->data)) != "no"; break;
            case kBrightness: m_state.brightness = has ? int(*static_cast<int64_t*>(prop->data)) : 0; break;
            case kContrast:   m_state.contrast = has ? int(*static_cast<int64_t*>(prop->data)) : 0; break;
            case kSaturation: m_state.saturation = has ? int(*static_cast<int64_t*>(prop->data)) : 0; break;
            case kGamma:      m_state.gamma = has ? int(*static_cast<int64_t*>(prop->data)) : 0; break;
            case kHue:        m_state.hue = has ? int(*static_cast<int64_t*>(prop->data)) : 0; break;
            case kAudioDelay: m_state.audioDelay = has ? *static_cast<double*>(prop->data) : 0.0; break;
            case kSubDelay:   m_state.subDelay = has ? *static_cast<double*>(prop->data) : 0.0; break;
            case kSecondaryVisible: m_state.secondarySubVisible = !has || *static_cast<int*>(prop->data); break;
            case kSecondarySid: {
                // "no", "auto" or a number
                const char* v = has ? *static_cast<char**>(prop->data) : nullptr;
                m_state.secondarySid = (v && *v >= '0' && *v <= '9') ? std::atoll(v) : -1;
                break;
            }
            case kTrackList:
                if (has) readTrackList(*static_cast<mpv_node*>(prop->data));
                else     m_state.tracks.clear();
                break;
            }
            break;
        }
        default:
            break;
        }
    }
    return true;
}

bool MpvPlayer::hasNewFrame() {
    return (mpv_render_context_update(m_render) & MPV_RENDER_UPDATE_FRAME) != 0;
}

void MpvPlayer::render(int fbo, int width, int height) {
    mpv_opengl_fbo target{fbo, width, height, 0};
    int flipY = 1;  // render to the default framebuffer, which is bottom-up
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(m_render, params);
}

void MpvPlayer::reportSwap() {
    mpv_render_context_report_swap(m_render);
}

std::string MpvPlayer::propertyString(const char* name) const {
    if (!m_mpv) return {};
    char* v = mpv_get_property_string(m_mpv, name);
    if (!v) return {};
    std::string out = v;
    mpv_free(v);
    return out;
}
