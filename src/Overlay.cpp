#include "Overlay.h"
#include <cstdlib>
#include <limits>
#include "I18n.h"
#include "Paths.h"
#include "tolga/AirPlayApi.h"
#include "MpvPlayer.h"
#include "OpenSubtitles.h"
#include "Thumbnailer.h"

#include <SDL3/SDL_opengl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <cmath>
#include <functional>
#include <iterator>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kHideAfterMs = 2000;
constexpr float    kFadeSeconds = 0.22f;
constexpr float    kPanelH = 92.0f;
constexpr float    kPanelBottom = 22.0f;

// The light appearance turns the greys around: white text and lines become near-black,
// the dark panels become light. Colours with a hue (turquoise, red) stay as they are.
// TV/TC stand for ImVec4/IM_COL32 throughout this file so every colour follows the theme.
bool g_light = false;

ImVec4 TV(float r, float g, float b, float a) {
    if (!g_light) return ImVec4(r, g, b, a);
    if (std::max({r, g, b}) - std::min({r, g, b}) > 0.07f) {
        // bright accents (turquoise text, the coral error) get darker to read on light grey
        const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        return lum > 0.55f ? ImVec4(r * 0.78f, g * 0.78f, b * 0.78f, a) : ImVec4(r, g, b, a);
    }
    const float v = (r + g + b) / 3.0f;
    const float l = v >= 0.5f ? 1.0f - v * 0.906f : 1.0f - v * 0.35f;  // white -> ink, black -> paper
    return ImVec4(l, l, l, a);
}

ImU32 TC(int r, int g, int b, int a) {
    return ImGui::ColorConvertFloat4ToU32(TV(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f));
}

// Colours are always multiplied by the overlay alpha so everything fades together.
ImU32 col(float r, float g, float b, float a, float alpha) {
    return ImGui::GetColorU32(TV(r, g, b, a * alpha));
}

std::string formatTime(double t) {
    if (!(t >= 0.0)) t = 0.0;
    int s = int(t);
    int h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof buf, "%d:%02d:%02d", h, m, sec);
    else       std::snprintf(buf, sizeof buf, "%d:%02d", m, sec);
    return buf;
}

// ------------------------------------------------------------- icons
// All icons are drawn as vector shapes so they stay sharp on any display.

void iconPlay(ImDrawList* dl, ImVec2 c, float s, ImU32 color) {
    const float h = s * 0.5f;
    dl->AddTriangleFilled({c.x - h * 0.7f, c.y - h}, {c.x - h * 0.7f, c.y + h}, {c.x + h * 0.95f, c.y}, color);
}

void iconPause(ImDrawList* dl, ImVec2 c, float s, ImU32 color) {
    const float h = s * 0.46f, w = s * 0.27f, gap = s * 0.11f;
    dl->AddRectFilled({c.x - gap - w, c.y - h}, {c.x - gap, c.y + h}, color, w * 0.35f);
    dl->AddRectFilled({c.x + gap, c.y - h}, {c.x + gap + w, c.y + h}, color, w * 0.35f);
}

void iconSkip(ImDrawList* dl, ImVec2 c, float s, ImU32 color, bool forward) {
    const float h = s * 0.32f, w = s * 0.42f;
    const float dir = forward ? 1.0f : -1.0f;
    for (int i = 0; i < 2; ++i) {
        float x0 = c.x + dir * (i == 0 ? -w : 0.0f);
        dl->AddTriangleFilled({x0, c.y - h}, {x0, c.y + h}, {x0 + dir * w, c.y}, color);
    }
}

void iconVolume(ImDrawList* dl, ImVec2 c, float s, ImU32 color, double volume, bool muted) {
    const float h = s * 0.5f;
    const float bx = c.x - h * 0.9f;
    // speaker body + cone
    dl->AddRectFilled({bx, c.y - h * 0.32f}, {bx + h * 0.42f, c.y + h * 0.32f}, color, 1.5f);
    dl->AddTriangleFilled({bx + h * 0.30f, c.y}, {bx + h * 0.95f, c.y - h * 0.85f}, {bx + h * 0.95f, c.y + h * 0.85f}, color);
    const float wx = bx + h * 1.15f;
    if (muted || volume <= 0.0) {
        const float r = h * 0.38f, cx = wx + r;
        dl->AddLine({cx - r, c.y - r}, {cx + r, c.y + r}, color, 1.8f);
        dl->AddLine({cx - r, c.y + r}, {cx + r, c.y - r}, color, 1.8f);
        return;
    }
    const int waves = volume > 66 ? 3 : volume > 33 ? 2 : 1;
    for (int i = 0; i < waves; ++i) {
        float r = h * (0.35f + 0.32f * i);
        dl->PathArcTo({wx - h * 0.2f, c.y}, r, -0.9f, 0.9f, 12);
        dl->PathStroke(color, 0, 1.6f);
    }
}

void iconFullscreen(ImDrawList* dl, ImVec2 c, float s, ImU32 color, bool exit) {
    const float h = s * 0.42f, l = s * 0.26f, t = 1.8f;
    // four corner brackets; when `exit` they point inwards
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            ImVec2 corner = exit ? ImVec2(c.x + sx * h * 0.35f, c.y + sy * h * 0.35f)
                                 : ImVec2(c.x + sx * h, c.y + sy * h);
            float dx = (exit ? sx : -sx) * l, dy = (exit ? sy : -sy) * l;
            dl->AddLine(corner, {corner.x + dx, corner.y}, color, t);
            dl->AddLine(corner, {corner.x, corner.y + dy}, color, t);
        }
}

void iconSubtitles(ImDrawList* dl, ImVec2 c, float s, ImU32 color, bool on) {
    const float w = s * 0.55f, h = s * 0.40f;
    dl->AddRect({c.x - w, c.y - h}, {c.x + w, c.y + h}, color, 3.0f, 0, 1.6f);
    // two "text lines" inside the box
    dl->AddLine({c.x - w * 0.6f, c.y - h * 0.05f}, {c.x + w * 0.15f, c.y - h * 0.05f}, color, 1.6f);
    dl->AddLine({c.x + w * 0.35f, c.y - h * 0.05f}, {c.x + w * 0.6f, c.y - h * 0.05f}, color, 1.6f);
    dl->AddLine({c.x - w * 0.6f, c.y + h * 0.42f}, {c.x - w * 0.25f, c.y + h * 0.42f}, color, 1.6f);
    dl->AddLine({c.x - w * 0.05f, c.y + h * 0.42f}, {c.x + w * 0.6f, c.y + h * 0.42f}, color, 1.6f);
    if (!on)  // subtitles hidden: strike through
        dl->AddLine({c.x - w - 2, c.y + h + 2}, {c.x + w + 2, c.y - h - 2}, color, 1.8f);
}

// Filled triangle with softly rounded corners (as in Apple's glyphs).
void roundedTriangle(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, float r, ImU32 color) {
    const ImVec2 pts[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        const ImVec2 p = pts[i], prev = pts[(i + 2) % 3], next = pts[(i + 1) % 3];
        auto norm = [](ImVec2 v) {
            const float l = std::sqrt(v.x * v.x + v.y * v.y);
            return ImVec2(v.x / l, v.y / l);
        };
        const ImVec2 d1 = norm({prev.x - p.x, prev.y - p.y}), d2 = norm({next.x - p.x, next.y - p.y});
        dl->PathLineTo({p.x + d1.x * r, p.y + d1.y * r});
        dl->PathBezierQuadraticCurveTo(p, {p.x + d2.x * r, p.y + d2.y * r}, 6);
    }
    dl->PathFillConvex(color);
}

// AirPlay video glyph: a rounded screen, open at the bottom, with a solid triangle rising into it.
void iconAirPlay(ImDrawList* dl, ImVec2 c, float s, ImU32 color, bool active) {
    const ImU32 col = active ? ImGui::GetColorU32(TV(0.18f, 0.80f, 0.76f, 1.0f)) : color;
    const float w = s * 0.58f, top = c.y - s * 0.46f, bot = c.y + s * 0.20f;
    const float r = s * 0.11f, t = 1.7f, gap = s * 0.30f;
    dl->PathLineTo({c.x - gap, bot});
    dl->PathLineTo({c.x - w + r, bot});
    dl->PathArcTo({c.x - w + r, bot - r}, r, 3.14159265f * 0.5f, 3.14159265f, 6);
    dl->PathArcTo({c.x - w + r, top + r}, r, 3.14159265f, 3.14159265f * 1.5f, 6);
    dl->PathArcTo({c.x + w - r, top + r}, r, 3.14159265f * 1.5f, 3.14159265f * 2.0f, 6);
    dl->PathArcTo({c.x + w - r, bot - r}, r, 0.0f, 3.14159265f * 0.5f, 6);
    dl->PathLineTo({c.x + gap, bot});
    dl->PathStroke(col, 0, t);
    roundedTriangle(dl, {c.x, c.y - s * 0.06f}, {c.x + s * 0.36f, c.y + s * 0.47f}, {c.x - s * 0.36f, c.y + s * 0.47f},
                    s * 0.07f, col);
}

void iconGear(ImDrawList* dl, ImVec2 c, float s, ImU32 color, bool active) {
    const ImU32 col = active ? ImGui::GetColorU32(TV(0.18f, 0.80f, 0.76f, 1.0f)) : color;
    const float r = s * 0.36f, tooth = s * 0.13f;
    for (int i = 0; i < 8; ++i) {  // eight teeth
        const float ang = i * 3.14159265f / 4.0f;
        const ImVec2 d{std::cos(ang), std::sin(ang)};
        dl->AddLine({c.x + d.x * r, c.y + d.y * r}, {c.x + d.x * (r + tooth), c.y + d.y * (r + tooth)}, col, 2.6f);
    }
    dl->AddCircle(c, r, col, 24, 2.0f);
    dl->AddCircle(c, r * 0.42f, col, 16, 1.6f);
}

void iconPlaylist(ImDrawList* dl, ImVec2 c, float s, ImU32 color, bool active) {
    const ImU32 col = active ? ImGui::GetColorU32(TV(0.18f, 0.80f, 0.76f, 1.0f)) : color;
    const float w = s * 0.5f;
    for (int i = 0; i < 3; ++i) {
        const float y = c.y - s * 0.28f + i * s * 0.28f;
        dl->AddCircleFilled({c.x - w + 1.5f, y}, 1.6f, col);
        dl->AddLine({c.x - w + 5.5f, y}, {c.x + w, y}, col, 1.7f);
    }
}

// Round hit area with a soft hover highlight. Returns true when clicked.
bool iconButton(const char* id, ImVec2 center, float radius, float alpha) {
    ImGui::SetCursorScreenPos({center.x - radius, center.y - radius});
    bool clicked = ImGui::InvisibleButton(id, {radius * 2, radius * 2});
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        float a = ImGui::IsItemActive() ? 0.20f : 0.11f;
        ImGui::GetWindowDrawList()->AddCircleFilled(center, radius, col(1, 1, 1, a, alpha), 32);
    }
    return clicked;
}

// Horizontal slider drawn as a thin rounded track that thickens on hover.
// `value` is in [0,1]. Returns true while the user is changing it.
bool thinSlider(const char* id, ImVec2 pos, float width, float& value, float alpha,
                bool* released = nullptr, float* hoverValue = nullptr) {
    const float hitH = 18.0f;
    ImGui::SetCursorScreenPos({pos.x, pos.y - hitH * 0.5f});
    ImGui::InvisibleButton(id, {width, hitH});
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (released) *released = ImGui::IsItemDeactivated();

    const float mx = ImGui::GetIO().MousePos.x;
    const float t = std::clamp((mx - pos.x) / width, 0.0f, 1.0f);
    if (hoverValue) *hoverValue = (hovered || active) ? t : -1.0f;
    if (active) value = t;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float th = (hovered || active) ? 5.0f : 3.0f;
    const ImVec2 a{pos.x, pos.y - th * 0.5f}, b{pos.x + width, pos.y + th * 0.5f};
    dl->AddRectFilled(a, b, col(1, 1, 1, 0.22f, alpha), th);
    dl->AddRectFilled(a, {pos.x + width * value, b.y}, col(1, 1, 1, 0.95f, alpha), th);
    if (hovered || active)
        dl->AddCircleFilled({pos.x + width * value, pos.y}, 6.5f, col(1, 1, 1, 1, alpha), 24);
    return active;
}

const char* findSystemFont() {
    static const char* candidates[] = {
        "/System/Library/Fonts/SFNS.ttf",                       // macOS San Francisco
        "/System/Library/Fonts/Helvetica.ttc",
        "C:\\Windows\\Fonts\\segoeui.ttf",                      // Windows
        "/usr/share/fonts/truetype/inter/Inter-Regular.ttf",    // Linux
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const char* p : candidates)
        if (FILE* f = std::fopen(p, "rb")) { std::fclose(f); return p; }
    return nullptr;
}

// Fonts for the characters the main font lacks (Japanese, Chinese, Korean): in the menus of
// those languages and in file and track names. Glyphs are loaded only when a text needs them.
// Han characters look different in Chinese and Japanese, so the user's language goes first.
std::vector<const char*> findFallbackFonts() {
    const char* const japanese[] = {
        "/System/Library/Fonts/\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W3.ttc",  // Hiragino Kaku Gothic
        "C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\meiryo.ttc"};
    const char* const chinese[] = {
        "/System/Library/Fonts/Hiragino Sans GB.ttc", "C:\\Windows\\Fonts\\msyh.ttc"};
    const char* const korean[] = {
        "/System/Library/Fonts/AppleSDGothicNeo.ttc", "C:\\Windows\\Fonts\\malgun.ttf"};
    const char* const noto[] = {  // Noto Sans CJK has all three
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc"};
    std::vector<const char*> order;
    auto add = [&](const auto& list) { for (const char* p : list) order.push_back(p); };
    const std::string& lang = i18n::language();
    if (lang == "zh")      { add(chinese); add(japanese); add(korean); }
    else if (lang == "ko") { add(korean); add(japanese); add(chinese); }
    else                   { add(japanese); add(chinese); add(korean); }
    add(noto);
    std::vector<const char*> found;
    for (const char* p : order)
        if (FILE* f = std::fopen(p, "rb")) { std::fclose(f); found.push_back(p); }
    return found;
}

}  // namespace

// ================================================================== setup

namespace {

// ImGui's own colours (text, popups, separators...) for the current appearance.
void applyThemeColors() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_Text] = TV(1, 1, 1, 0.95f);
    style.Colors[ImGuiCol_PopupBg] = TV(0.09f, 0.09f, 0.11f, 0.94f);
    style.Colors[ImGuiCol_Border] = TV(1, 1, 1, 0.08f);
    style.Colors[ImGuiCol_Header] = TV(1, 1, 1, 0.08f);
    style.Colors[ImGuiCol_HeaderHovered] = TV(1, 1, 1, 0.12f);
    style.Colors[ImGuiCol_HeaderActive] = TV(1, 1, 1, 0.18f);
    style.Colors[ImGuiCol_Separator] = TV(1, 1, 1, 0.10f);
    style.Colors[ImGuiCol_TextDisabled] = TV(1, 1, 1, 0.45f);
    style.Colors[ImGuiCol_CheckMark] = TV(1, 1, 1, 0.95f);
    // Tolga Player's turquoise instead of ImGui's default blue
    style.Colors[ImGuiCol_SliderGrab] = TV(0.18f, 0.80f, 0.76f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = TV(0.30f, 0.88f, 0.84f, 1.0f);
    style.Colors[ImGuiCol_TextSelectedBg] = TV(0.18f, 0.80f, 0.76f, 0.35f);
    style.Colors[ImGuiCol_NavHighlight] = TV(0.18f, 0.80f, 0.76f, 1.0f);
}

std::string themeFile() {
    std::string path;
    if (char* pref = SDL_GetPrefPath("", "Tolga")) {
        path = std::string(pref) + "theme.txt";
        SDL_free(pref);
    }
    return path;
}

}  // namespace

void Overlay::loadTheme() {
    std::ifstream in(utf8Path(themeFile()));
    std::string word;
    if (in >> word) m_theme = word == "dark" ? Theme::Dark : word == "light" ? Theme::Light : Theme::System;
}

void Overlay::setTheme(Theme theme) {
    m_theme = theme;
    std::ofstream out(utf8Path(themeFile()), std::ios::trunc);
    out << (theme == Theme::System ? "system" : theme == Theme::Light ? "light" : "dark") << '\n';
    poke();
}

bool Overlay::lightTheme() const { return g_light; }

bool Overlay::init(SDL_Window* window, SDL_GLContext gl, const char* glslVersion) {
    m_window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the binary

    if (const char* font = findSystemFont())
        io.Fonts->AddFontFromFileTTF(font, 15.0f);
    else
        io.Fonts->AddFontDefault();
    for (const char* fallback : findFallbackFonts()) {
        ImFontConfig merge;
        merge.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(fallback, 15.0f, &merge);
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = {0, 0};
    style.WindowRounding = 16.0f;
    loadTheme();
    applyThemeColors();
    style.PopupRounding = 12.0f;
    style.PopupBorderSize = 1.0f;
    style.ItemSpacing = {8, 4};

    if (!ImGui_ImplSDL3_InitForOpenGL(window, gl)) return false;
    if (!ImGui_ImplOpenGL3_Init(glslVersion)) return false;
#ifdef _WIN32
    m_mergeDrawLists = std::strcmp(glslVersion, "#version 120") == 0;
#endif
    if (const char* m = std::getenv("TOLGA_GL_MERGE"); m && *m) m_mergeDrawLists = *m != '0';
    m_lastActivity = m_lastFrame = SDL_GetTicks();
    return true;
}

void Overlay::shutdown() {
    if (m_thumbTex) {
        GLuint tex = m_thumbTex;
        glDeleteTextures(1, &tex);
        m_thumbTex = 0;
    }
    if (m_merged) {
        IM_DELETE(m_merged);
        m_merged = nullptr;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void Overlay::processEvent(const SDL_Event& e) {
    if (m_uiScale != 1.0f) {
        // the controls are laid out in scaled units: mouse positions in window pixels -> those
        SDL_Event s = e;
        const float k = 1.0f / m_uiScale;
        switch (s.type) {
        case SDL_EVENT_MOUSE_MOTION:
            s.motion.x *= k; s.motion.y *= k; s.motion.xrel *= k; s.motion.yrel *= k;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            s.button.x *= k; s.button.y *= k;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            s.wheel.mouse_x *= k; s.wheel.mouse_y *= k;
            break;
        default:
            break;
        }
        ImGui_ImplSDL3_ProcessEvent(&s);
    } else {
        ImGui_ImplSDL3_ProcessEvent(&e);
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        e.type == SDL_EVENT_MOUSE_WHEEL)
        poke();
}

float Overlay::controlsHeight() { return kPanelH + kPanelBottom; }

void Overlay::poke() { m_lastActivity = SDL_GetTicks(); }
bool Overlay::wantsMouse() const { return ImGui::GetIO().WantCaptureMouse; }
bool Overlay::wantsKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }

// ================================================================== frame

OverlayActions Overlay::draw(const PlayerState& st, bool fullscreen, const AirPlayView& airplay,
                             const OnlineView& online, const PlaylistView& playlist) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    if (m_fbW > 0 && m_fbH > 0) {  // same size as the video below, even mid-resize
        ImGuiIO& io = ImGui::GetIO();
        const float k = m_fbScale * m_uiScale;
        io.DisplaySize = {m_fbW / k, m_fbH / k};
        io.DisplayFramebufferScale = {k, k};
    }
    // the appearance: chosen in Settings > General, or the system's
    const bool light = m_theme == Theme::Light ||
                       (m_theme == Theme::System && SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT);
    if (light != g_light) {
        g_light = light;
        applyThemeColors();
    }
    ImGui::NewFrame();

    const uint64_t now = SDL_GetTicks();
    const float dt = std::min(0.1f, (now - m_lastFrame) / 1000.0f);
    m_lastFrame = now;

    const CastStatus* cast = airplay.cast && airplay.cast->active() ? airplay.cast : nullptr;

    // Controls stay visible while idle, paused, casting, hovered, dragging or recently active.
    const bool visible = st.idle || st.paused || cast || m_hoveringPanel || m_dragging || m_menuOpen || m_panelOpen ||
                         m_playlistOpen ||
                         (now - m_lastActivity) < kHideAfterMs;
    m_targetAlpha = visible ? 1.0f : 0.0f;
    const float step = dt / kFadeSeconds;
    if (m_alpha < m_targetAlpha) m_alpha = std::min(m_targetAlpha, m_alpha + step);
    else                         m_alpha = std::max(m_targetAlpha, m_alpha - step);

    // side panel slides in over ~0.18 s; the control bar moves into the space left of it
    if (st.idle) m_panelOpen = false;
    m_panelAnim = std::clamp(m_panelAnim + (m_panelOpen ? 1.0f : -1.0f) * dt / 0.18f, 0.0f, 1.0f);
    {
        const float dw = ImGui::GetIO().DisplaySize.x;
        const float ease = 1.0f - (1.0f - m_panelAnim) * (1.0f - m_panelAnim);
        m_panelVisibleW = panelWidth(dw) * ease;
        // the playlist slides in from the left the same way
        m_playlistAnim = std::clamp(m_playlistAnim + (m_playlistOpen ? 1.0f : -1.0f) * dt / 0.18f, 0.0f, 1.0f);
        const float pe = 1.0f - (1.0f - m_playlistAnim) * (1.0f - m_playlistAnim);
        m_leftVisibleW = panelWidth(dw) * pe;
    }

    OverlayActions out;
    if (cast && !st.idle) drawCastScreen(st, *cast);
    if (st.idle) {
        drawIdleScreen(out);
    } else if (m_alpha > 0.0f) {
        m_menuOpen = false;  // set again below by whichever popup is open
        if (fullscreen) drawTitleBar(st);
        else if (m_titlebarOverlay || m_customChrome) drawTitleShade();
        drawControls(st, fullscreen, airplay, out);
    } else {
        m_hoveringPanel = false;
        m_menuOpen = false;
    }

    if (m_customChrome && !fullscreen) drawWindowChrome(st, out);
    if (m_playlistAnim > 0.0f) drawPlaylist(st, playlist, out);
    if (m_panelAnim > 0.0f && !st.idle) drawSidePanel(st, airplay, online, out);
    if (m_resultsOpen && !st.idle) drawResultsDialog(online, out);

    const bool wantPin = cast && (cast->phase == CastStatus::Phase::NeedPin ||
                                  cast->phase == CastStatus::Phase::Pairing);
    if (wantPin) drawPinDialog(*cast, out);
    else         m_pinOpen = false;
    if (m_licenseOpen && !wantPin) drawLicenseDialog(airplay, out);
    if (m_aboutOpen && !wantPin && !m_licenseOpen) drawAbout(airplay, out);
    drawToast();
    return out;
}

bool Overlay::isAnimating() const {
    const bool panelMoving = (m_panelOpen ? m_panelAnim < 1.0f : m_panelAnim > 0.0f) ||
                             (m_playlistOpen ? m_playlistAnim < 1.0f : m_playlistAnim > 0.0f);
    return m_alpha != m_targetAlpha || m_pinOpen || m_licenseOpen || m_aboutOpen || panelMoving || SDL_GetTicks() < m_toastUntil;
}

void Overlay::togglePanel(PanelTab tab) {
    if (m_panelOpen && m_panelTab == tab) m_panelOpen = false;
    else { m_panelOpen = true; m_panelTab = tab; }
    poke();
}

void Overlay::showToast(const std::string& text) {
    m_toast = text;
    m_toastUntil = SDL_GetTicks() + 5000;
}

// ------------------------------------------------------------- casting

void Overlay::drawCastScreen(const PlayerState& st, const CastStatus& cast) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilledMultiColor({0, 0}, size,
        TC(24, 26, 34, 255), TC(24, 26, 34, 255),
        TC(10, 10, 13, 255), TC(10, 10, 13, 255));

    const ImVec2 c{size.x * 0.5f, size.y * 0.5f - 46.0f};
    iconAirPlay(bg, c, 76, TC(255, 255, 255, 215), true);

    using P = CastStatus::Phase;
    const int dots = int(ImGui::GetTime() * 3.0) % 4;
    std::string line;
    switch (cast.phase) {
    case P::Preparing: {
        char pct[16];
        std::snprintf(pct, sizeof pct, " %d%%", int(cast.progress * 100.0));
        line = TF("Preparing for {}...", {cast.deviceName}) + pct;
        break;
    }
    case P::Connecting: line = TF("Connecting to {}", {cast.deviceName}) + std::string(dots, '.'); break;
    case P::NeedPin:
    case P::Pairing:    line = TF("Pairing with {}", {cast.deviceName}); break;
    case P::Loading:    line = TF("Loading on {}", {cast.deviceName}) + std::string(dots, '.'); break;
    case P::Paused:     line = TF("Paused on {}", {cast.deviceName}); break;
    case P::Ended:      line = TF("Finished on {}", {cast.deviceName}); break;
    default:            line = TF("Playing on {}", {cast.deviceName}); break;
    }
    ImFont* font = ImGui::GetFont();
    // keep the text centred while the dots animate
    const bool animated = cast.phase == P::Connecting || cast.phase == P::Loading;
    const std::string widest = animated ? line.substr(0, line.size() - dots) + "..." : line;
    const float ls = 22.0f;
    const float lw = font->CalcTextSizeA(ls, FLT_MAX, 0, widest.c_str()).x;
    bg->AddText(font, ls, {c.x - lw * 0.5f, c.y + 52}, TC(255, 255, 255, 235), line.c_str());

    if (!st.title.empty()) {
        const float ts = 15.0f;
        ImVec2 tw = font->CalcTextSizeA(ts, FLT_MAX, 0, st.title.c_str());
        bg->AddText(font, ts, {c.x - tw.x * 0.5f, c.y + 84}, TC(255, 255, 255, 120), st.title.c_str());
    }
}

void Overlay::drawPinDialog(const CastStatus& cast, OverlayActions& out) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const bool checking = cast.phase == CastStatus::Phase::Pairing;
    if (!m_pinOpen || (!checking && m_pinChecking)) std::memset(m_pinBuf, 0, sizeof m_pinBuf);
    const bool firstFrame = !m_pinOpen || (!checking && m_pinChecking);
    m_pinOpen = true;
    m_pinChecking = checking;

    // dim everything behind the dialog (and block clicks on the panel)
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, TV(0, 0, 0, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##pin_dim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const ImVec2 dlg{340, 200};
    ImGui::SetNextWindowPos({(size.x - dlg.x) * 0.5f, (size.y - dlg.y) * 0.5f});
    ImGui::SetNextWindowSize(dlg);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, TV(0.10f, 0.10f, 0.12f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, TV(1, 1, 1, 0.10f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22, 20});
    ImGui::Begin("##pin", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);
    m_hoveringPanel = true;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string title = TF("Pair with {}", {cast.deviceName});
    dl->AddText(ImGui::GetFont(), 19.0f, ImGui::GetCursorScreenPos(), TC(255, 255, 255, 245), title.c_str());
    ImGui::Dummy({0, 28});
    if (cast.pinRejected)
        ImGui::TextColored({1.0f, 0.55f, 0.5f, 1.0f}, "%s", T("That code didn't work. Enter the new one."));
    else
        ImGui::TextDisabled("%s", T("Enter the code shown on your TV."));
    ImGui::Dummy({0, 6});

    // big centred digits field
    const float fieldW = 150.0f;
    ImGui::SetCursorPosX((dlg.x - fieldW) * 0.5f);
    ImGui::PushItemWidth(fieldW);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12, 8});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, TV(1, 1, 1, 0.07f));
    if (firstFrame) ImGui::SetKeyboardFocusHere();
    ImGui::BeginDisabled(checking);
    const bool enter = ImGui::InputText("##pinfield", m_pinBuf, 5,
        ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopItemWidth();

    ImGui::Dummy({0, 10});
    const ImVec2 bsz{100, 30};
    ImGui::SetCursorPosX(dlg.x - 22 - bsz.x * 2 - 10);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 15.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, TV(1, 1, 1, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TV(1, 1, 1, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, TV(1, 1, 1, 0.20f));
    if (ImGui::Button(T("Cancel"), bsz) || ImGui::IsKeyPressed(ImGuiKey_Escape)) out.cancelPin = true;
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 10);
    const bool ready = std::strlen(m_pinBuf) == 4 && !checking;
    ImGui::PushStyleColor(ImGuiCol_Button, TV(0.07f, 0.62f, 0.60f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TV(0.11f, 0.70f, 0.67f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, TV(0.05f, 0.52f, 0.51f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));  // white on turquoise in both themes
    ImGui::BeginDisabled(!ready);
    if ((ImGui::Button(checking ? T("Checking...") : T("Pair"), bsz) || enter) && ready) {
        out.submitPin = true;
        out.pin = m_pinBuf;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ------------------------------------------------------------- license

namespace {
// The app icon, drawn: a turquoise tile with a white screen, the play mark and the AirPlay
// triangle (same artwork as resources/macos/icon.svg).
void drawAppIcon(ImDrawList* dl, ImVec2 o, float s) {
    auto P = [&](float x, float y) { return ImVec2{o.x + (x - 100.0f) / 824.0f * s, o.y + (y - 100.0f) / 824.0f * s}; };
    const ImU32 top = IM_COL32(94, 240, 220, 255), bottom = IM_COL32(15, 149, 160, 255);
    const float r = s * 0.225f;
    // vertical gradient inside the rounded tile: draw thin rounded bands
    const int bands = 24;
    for (int i = 0; i < bands; ++i) {
        const float t0 = float(i) / bands, t1 = float(i + 1) / bands;
        const ImVec4 a = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
        const float t = (t0 + t1) * 0.5f;
        const ImU32 c = ImGui::GetColorU32(ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1));
        ImDrawFlags fl = ImDrawFlags_RoundCornersNone;
        if (i == 0) fl = ImDrawFlags_RoundCornersTop;
        if (i == bands - 1) fl = ImDrawFlags_RoundCornersBottom;
        const float y0 = o.y + s * t0, y1 = o.y + s * t1 + (i == bands - 1 ? 0.0f : 1.0f);
        dl->AddRectFilled({o.x, y0}, {o.x + s, y1}, c, (i == 0 || i == bands - 1) ? r : 0.0f, fl);
    }
    const ImU32 white = IM_COL32(255, 255, 255, 255), teal = IM_COL32(18, 164, 170, 255), coral = IM_COL32(232, 72, 63, 255);
    // screen (y shifted by 33 like the SVG), with the notch the AirPlay triangle rises into
    dl->AddRectFilled(P(250, 283), P(774, 633), white, s * 0.073f);
    const ImU32 notch = ImGui::GetColorU32(ImVec4(0.17f, 0.70f, 0.71f, 1));
    dl->AddTriangleFilled(P(512, 521), P(612, 640), P(412, 640), notch);
    dl->AddTriangleFilled(P(464, 352), P(602, 437), P(464, 522), teal);
    dl->AddTriangleFilled(P(512, 593), P(630, 741), P(394, 741), coral);
}
}  // namespace

namespace {
// Text files are wrapped at ~80 columns; join the lines of each paragraph so the text wraps
// to the dialog. Paragraphs (blank-line separated) stay apart. `license`: a numbered clause
// or "(a)" item starts a new paragraph. Otherwise table rows (two spaces inside the line) and
// indented commands keep their own lines, as does the line after one ending in ':'.
std::string reflowText(const std::string& text, bool license) {
    std::string out, block;
    bool lineBreak = false;  // next line goes on a new line of the same block
    auto flush = [&] {
        if (block.empty()) return;
        if (!out.empty()) out += "\n\n";
        out += block;
        block.clear();
    };
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) { flush(); lineBreak = false; continue; }
        const size_t e = line.find_last_not_of(" \t\r");
        const std::string t = line.substr(b, e - b + 1);
        if (license) {
            if (!block.empty() && (std::isdigit((unsigned char)t[0]) || t[0] == '(')) flush();
            if (!block.empty()) block += ' ';
            block += t;
            continue;
        }
        const bool row = t.find("  ") != std::string::npos || b >= 4;
        std::string r = t;
        if (row) {  // columns padded for a fixed-width font: two spaces between them is enough here
            r.clear();
            for (char ch : t)
                if (!(ch == ' ' && r.size() >= 2 && r[r.size() - 1] == ' ' && r[r.size() - 2] == ' ')) r += ch;
        }
        if (block.empty())               block = r;
        else if (row || lineBreak)       block += "\n" + r;
        else                             block += ' ' + r;
        lineBreak = row || t.back() == ':';
    }
    flush();
    return out;
}
}  // namespace

void Overlay::setAbout(AboutInfo info) {
    m_about = std::move(info);
    m_licenseReflowed = reflowText(m_about.licenseText, true);
    m_creditsReflowed = reflowText(m_about.creditsText, false);
}

void Overlay::drawAbout(const AirPlayView& airplay, OverlayActions& out) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    // dim the video; a click outside closes
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, TV(0, 0, 0, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##about_dim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_aboutOpen = false;
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) m_aboutOpen = false;
    if (!m_aboutOpen) return;

    const ImVec2 dlg{std::min(720.0f, size.x - 40.0f), std::min(430.0f, size.y - 24.0f)};
    const bool small = dlg.y < 400.0f;  // short window: smaller icon, tighter column
    ImGui::SetNextWindowPos({(size.x - dlg.x) * 0.5f, (size.y - dlg.y) * 0.5f});
    ImGui::SetNextWindowSize(dlg);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, TV(0.10f, 0.10f, 0.12f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, TV(1, 1, 1, 0.10f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::Begin("##about", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);
    m_hoveringPanel = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float leftW = std::min(250.0f, dlg.x * 0.38f);
    ImFont* font = ImGui::GetFont();
    auto centered = [&](const std::string& text, float y, float px, ImU32 color) {
        const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0, text.c_str());
        dl->AddText(font, px, {wp.x + (leftW - ts.x) * 0.5f, wp.y + y}, color, text.c_str());
    };

    // ---- left: icon, name, versions, the buttons
    const float iconS = small ? 60.0f : 96.0f;
    const float y0 = small ? 18.0f : 30.0f;
    drawAppIcon(dl, {wp.x + (leftW - iconS) * 0.5f, wp.y + y0}, iconS);
    float ty = y0 + iconS + (small ? 8.0f : 14.0f);
    centered("Tolga Player", ty, small ? 21.0f : 26.0f, TC(255, 255, 255, 245));
    ty += small ? 28.0f : 36.0f;
    centered(m_about.version.empty() ? std::string() : TF("Version {}", {m_about.version}), ty, 15.0f, TC(255, 255, 255, 190));
    ty += 22.0f;
    std::string libs = m_about.mpvVersion;
    if (!m_about.ffmpegVersion.empty()) libs += (libs.empty() ? "" : "   ") + std::string("FFmpeg ") + m_about.ffmpegVersion;
    centered(libs, ty, 12.5f, TC(255, 255, 255, 120));
    ty += 22.0f;
    centered("\xC2\xA9 2026 Tolga \xC3\x87iftci\xC3\xA7" "elik", ty, 12.5f, TC(255, 255, 255, 120));  // © 2026 Tolga Çiftciçelik
    ty += 22.0f;
    if (const LicenseInfo* lic = airplay.license) {
        std::string line;
        using S = LicenseInfo::State;
        if (lic->state == S::Licensed) line = TF("Licensed to {}", {lic->email});
        else if (lic->state == S::Trial) line = lic->trialDaysLeft == 1 ? std::string(T("AirPlay trial: 1 day left"))
                                                                     : TF("AirPlay trial: {} days left", {std::to_string(lic->trialDaysLeft)});
        else if (lic->state == S::Expired) line = T("AirPlay trial ended");
        if (!line.empty()) centered(line, ty, 12.5f, TC(47, 211, 198, 230));
    }
    ty += 30.0f;

    const ImVec2 bsz{leftW - 70.0f, small ? 26.0f : 30.0f};
    const char* labels[] = {T("License"), T("Credits"), T("Website")};
    const float by = std::max(ty, dlg.y - 3 * (bsz.y + 8) - 18);
    for (int i = 0; i < 3; ++i) {
        ImGui::SetCursorPos({35.0f, by + i * (bsz.y + 8)});
        const bool active = i == m_aboutTab;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? TV(0.07f, 0.62f, 0.60f, 1.0f) : TV(1, 1, 1, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? TV(0.11f, 0.70f, 0.67f, 1.0f) : TV(1, 1, 1, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, TV(0.05f, 0.52f, 0.51f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(1, 1, 1, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text));  // white on turquoise in both themes
        if (ImGui::Button(labels[i], bsz)) {
            if (i == 2) out.openWebsite = true;
            else m_aboutTab = i;
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
    }

    // ---- right: the text, scrolling
    dl->AddLine({wp.x + leftW, wp.y + 16}, {wp.x + leftW, wp.y + dlg.y - 16}, TC(255, 255, 255, 20));
    ImGui::SetCursorPos({leftW + 20.0f, 20.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, TV(0, 0, 0, 0));
    ImGui::BeginChild(m_aboutTab == 0 ? "##about_license" : "##about_credits", {dlg.x - leftW - 36.0f, dlg.y - 40.0f});
    const std::string& text = m_aboutTab == 0 ? m_licenseReflowed : m_creditsReflowed;
    ImGui::PushStyleColor(ImGuiCol_Text, TV(1, 1, 1, 0.78f));
    ImGui::PushTextWrapPos(0.0f);
    if (text.empty()) ImGui::TextUnformatted(m_aboutTab == 0 ? T("Tolga Player is licensed under the Apache License 2.0.")
                                                             : T("The licenses of the libraries Tolga Player uses are in the app bundle."));
    else ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void Overlay::drawLicenseDialog(const AirPlayView& airplay, OverlayActions& out) {
    const LicenseInfo* lic = airplay.license;
    if (!lic) { m_licenseOpen = false; return; }
    using S = LicenseInfo::State;
    const bool licensed = lic->state == S::Licensed || lic->state == S::Community;
    const bool first = m_licenseFirstFrame;
    m_licenseFirstFrame = false;
    if (first) std::memset(m_licenseBuf, 0, sizeof m_licenseBuf);

    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, TV(0, 0, 0, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##license_dim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) out.closeLicense = true;
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const ImVec2 dlg{std::min(460.0f, size.x - 40.0f), licensed ? 170.0f : 262.0f};
    ImGui::SetNextWindowPos({(size.x - dlg.x) * 0.5f, (size.y - dlg.y) * 0.5f});
    ImGui::SetNextWindowSize(dlg);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, TV(0.10f, 0.10f, 0.12f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, TV(1, 1, 1, 0.10f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22, 20});
    ImGui::Begin("##license", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);
    m_hoveringPanel = true;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImGui::GetFont(), 19.0f, ImGui::GetCursorScreenPos(), TC(255, 255, 255, 245), T("Tolga Player License"));
    ImGui::Dummy({0, 28});
    const float wrap = dlg.x - 44.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    if (licensed) {
        if (lic->state == S::Licensed) ImGui::TextUnformatted(TF("Licensed to {}. Thank you for supporting Tolga!", {lic->email}).c_str());
        else ImGui::TextUnformatted(T("This is a community build of Tolga. Everything is unlocked."));
    } else if (lic->state == S::Trial) {
        const std::string text = lic->trialDaysLeft == 1
            ? std::string(T("AirPlay is free to try for 1 more day. A license unlocks it for good; "
                            "playing on this computer is always free."))
            : TF("AirPlay is free to try for {} more days. A license unlocks it for good; "
                 "playing on this computer is always free.", {std::to_string(lic->trialDaysLeft)});
        ImGui::TextUnformatted(text.c_str());
    } else {
        ImGui::TextUnformatted(T("The AirPlay trial has ended. Playing on this computer stays free; "
                                 "a license unlocks AirPlay again."));
    }
    ImGui::PopTextWrapPos();

    bool enter = false;
    if (!licensed) {
        ImGui::Dummy({0, 8});
        ImGui::PushItemWidth(wrap);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 7});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, TV(1, 1, 1, 0.07f));
        if (first) ImGui::SetKeyboardFocusHere();
        enter = ImGui::InputTextWithHint("##licensekey", T("Paste your license key (TOLGA1-...)"), m_licenseBuf,
                                         sizeof m_licenseBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::PopItemWidth();
        if (!airplay.licenseError.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
            ImGui::TextColored({1.0f, 0.55f, 0.5f, 1.0f}, "%s", airplay.licenseError.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // buttons at the bottom right
    const ImVec2 bsz{110, 30};
    const int count = licensed ? 1 : (lic->buyUrl.empty() ? 2 : 3);
    ImGui::SetCursorPos({dlg.x - 22 - bsz.x * count - 10.0f * (count - 1), dlg.y - 20 - bsz.y});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 15.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, TV(1, 1, 1, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TV(1, 1, 1, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, TV(1, 1, 1, 0.20f));
    if (!licensed && !lic->buyUrl.empty()) {
        if (ImGui::Button(T("Buy License"), bsz)) out.openBuyPage = true;
        ImGui::SameLine(0, 10);
    }
    if (ImGui::Button(licensed ? T("Close") : T("Not Now"), bsz) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        out.closeLicense = true;
    ImGui::PopStyleColor(3);
    if (!licensed) {
        ImGui::SameLine(0, 10);
        const bool ready = m_licenseBuf[0] != 0;
        ImGui::PushStyleColor(ImGuiCol_Button, TV(0.07f, 0.62f, 0.60f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TV(0.11f, 0.70f, 0.67f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, TV(0.05f, 0.52f, 0.51f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));  // white on turquoise in both themes
        ImGui::BeginDisabled(!ready);
        if ((ImGui::Button(T("Activate"), bsz) || enter) && ready) {
            out.activateLicense = true;
            out.licenseKey = m_licenseBuf;
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void Overlay::drawToast() {
    const uint64_t now = SDL_GetTicks();
    if (m_toast.empty() || now >= m_toastUntil) return;
    const float left = float(m_toastUntil - now) / 1000.0f;
    const float a = std::min(1.0f, left / 0.3f);  // fade out in the last 0.3 s
    ImVec2 size = ImGui::GetIO().DisplaySize;
    size.x -= m_panelVisibleW;  // centre over the video, not under the side panel
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float maxW = std::min(560.0f, size.x - 60.0f);
    ImVec2 ts = font->CalcTextSizeA(15.0f, FLT_MAX, maxW, m_toast.c_str());
    const ImVec2 p0{(size.x - ts.x) * 0.5f - 18, 24};
    const ImVec2 p1{p0.x + ts.x + 36, p0.y + ts.y + 20};
    fg->AddRectFilled(p0, p1, col(0.10f, 0.10f, 0.12f, 0.94f, a), 12.0f);
    fg->AddRect(p0, p1, col(1, 1, 1, 0.10f, a), 12.0f);
    fg->AddText(font, 15.0f, {p0.x + 18, p0.y + 10}, col(1, 1, 1, 0.95f, a), m_toast.c_str(), nullptr, maxW);
}

void Overlay::render() {
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (m_mergeDrawLists) mergeDrawLists(dd);
    ImGui_ImplOpenGL3_RenderDrawData(dd);
}

// All of this frame's draw lists as one, so the renderer fills one vertex and one index
// buffer (see m_mergeDrawLists). Only while the indices still fit ImDrawIdx.
void Overlay::mergeDrawLists(ImDrawData* dd) {
    if (dd->CmdListsCount < 2) return;
    if (size_t(dd->TotalVtxCount) > size_t(std::numeric_limits<ImDrawIdx>::max()) + 1) return;
    if (!m_merged) m_merged = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
    ImDrawList& m = *m_merged;
    m.CmdBuffer.resize(0);
    m.VtxBuffer.resize(0);
    m.IdxBuffer.resize(0);
    m.VtxBuffer.reserve(dd->TotalVtxCount);
    m.IdxBuffer.reserve(dd->TotalIdxCount);
    // VtxOffset is always 0 here: without glDrawElementsBaseVertex (OpenGL 3.2) the renderer
    // doesn't announce it, and ImGui then starts a new list instead of a vertex offset.
    for (const ImDrawList* list : dd->CmdLists) {
        const unsigned vtxBase = unsigned(m.VtxBuffer.Size);
        const unsigned idxBase = unsigned(m.IdxBuffer.Size);
        for (const ImDrawVert& v : list->VtxBuffer) m.VtxBuffer.push_back(v);
        for (const ImDrawIdx i : list->IdxBuffer) m.IdxBuffer.push_back(ImDrawIdx(i + vtxBase));
        for (const ImDrawCmd& c : list->CmdBuffer) {
            ImDrawCmd n = c;
            n.IdxOffset = idxBase + c.IdxOffset;
            n.VtxOffset = 0;
            m.CmdBuffer.push_back(n);
        }
    }
    dd->CmdLists.resize(0);
    dd->CmdLists.push_back(&m);
    dd->CmdListsCount = 1;
}

// ------------------------------------------------------------- idle screen

void Overlay::drawIdleScreen(OverlayActions& out) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();

    // soft vertical gradient instead of flat black
    bg->AddRectFilledMultiColor({0, 0}, size,
        TC(28, 29, 36, 255), TC(28, 29, 36, 255),
        TC(12, 12, 15, 255), TC(12, 12, 15, 255));

    ImFont* font = ImGui::GetFont();

    // recently opened files under the button, as many as fit
    constexpr float kRowH = 34.0f, kListGap = 36.0f, kHeaderH = 26.0f, kClearH = 40.0f;
    size_t rows = m_recent ? m_recent->size() : 0;
    const float room = size.y - 280.0f;  // title, hint, button, margins and the line at the bottom
    while (rows > 0 && kListGap + kHeaderH + rows * kRowH + kClearH > room) --rows;
    const float listH = rows ? kListGap + kHeaderH + rows * kRowH + kClearH : 0.0f;
    const ImVec2 c{size.x * 0.5f, size.y * 0.5f - listH * 0.5f};

    const char* title = "Tolga Player";
    const float titleSize = 40.0f;
    ImVec2 ts = font->CalcTextSizeA(titleSize, FLT_MAX, 0, title);
    bg->AddText(font, titleSize, {c.x - ts.x * 0.5f, c.y - 70}, TC(255, 255, 255, 235), title);

    const char* hint = T("Drop a video here, or");
    ImVec2 hs = font->CalcTextSizeA(15.0f, FLT_MAX, 0, hint);
    bg->AddText(font, 15.0f, {c.x - hs.x * 0.5f, c.y - 12}, TC(255, 255, 255, 120), hint);

    // "Open File…" pill button
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##idle", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    const char* label = T("Open File...");
    ImVec2 ls = ImGui::CalcTextSize(label);
    ImVec2 bsize{ls.x + 40, 36};
    ImVec2 bpos{c.x - bsize.x * 0.5f, c.y + 22};
    ImGui::SetCursorScreenPos(bpos);
    if (ImGui::InvisibleButton("##open", bsize)) out.openFile = true;
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bpos, {bpos.x + bsize.x, bpos.y + bsize.y},
                      hov ? TC(255, 255, 255, 46) : TC(255, 255, 255, 26), bsize.y * 0.5f);
    dl->AddText({bpos.x + 20, bpos.y + (bsize.y - ls.y) * 0.5f}, TC(255, 255, 255, 235), label);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (rows > 0) {
        const float w = std::min(460.0f, size.x - 60.0f);
        const float x0 = c.x - w * 0.5f;
        float y = bpos.y + bsize.y + kListGap;
        dl->AddText(font, 14.0f, {x0 + 12, y}, TC(255, 255, 255, 110), T("Recent"));
        y += kHeaderH;
        for (size_t i = 0; i < rows; ++i) {
            const std::string& path = (*m_recent)[i];
            const size_t slash = path.find_last_of("/\\");
            const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
            std::string folder;
            if (slash != std::string::npos && slash > 0) {
                const size_t s2 = path.find_last_of("/\\", slash - 1);
                folder = path.substr(s2 == std::string::npos ? 0 : s2 + 1, slash - (s2 == std::string::npos ? 0 : s2 + 1));
            }
            const ImVec2 r0{x0, y}, r1{x0 + w, y + kRowH - 2};
            ImGui::PushID(int(i));
            ImGui::SetCursorScreenPos(r0);
            if (ImGui::InvisibleButton("##recent", {w, kRowH - 2})) out.openPath = path;
            const bool rh = ImGui::IsItemHovered();
            ImGui::PopID();
            if (rh) {
                dl->AddRectFilled(r0, r1, TC(255, 255, 255, 22), 8.0f);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            // folder on the right (dim), the file name takes the rest and is cut with "..."
            const float fw = folder.empty() ? 0.0f : std::min(140.0f, font->CalcTextSizeA(14.0f, FLT_MAX, 0, folder.c_str()).x);
            const float nameMax = w - 24 - (fw > 0 ? fw + 16 : 0);
            std::string shown = name;
            if (font->CalcTextSizeA(15.0f, FLT_MAX, 0, shown.c_str()).x > nameMax) {
                while (!shown.empty() && font->CalcTextSizeA(15.0f, FLT_MAX, 0, (shown + "...").c_str()).x > nameMax) {
                    shown.pop_back();
                    while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80) shown.pop_back();
                }
                shown += "...";
            }
            const float ty = y + (kRowH - 2 - 15.0f) * 0.5f;
            dl->AddText(font, 15.0f, {x0 + 12, ty}, TC(255, 255, 255, rh ? 245 : 215), shown.c_str());
            if (fw > 0) {
                dl->PushClipRect({x0 + w - 12 - fw, y}, {x0 + w - 12, y + kRowH}, true);
                dl->AddText(font, 14.0f, {x0 + w - 12 - fw, ty + 1}, TC(255, 255, 255, 90), folder.c_str());
                dl->PopClipRect();
            }
            y += kRowH;
        }
        // "Clear Recent"
        const char* clearLabel = T("Clear Recent");
        const ImVec2 cs = font->CalcTextSizeA(14.0f, FLT_MAX, 0, clearLabel);
        const ImVec2 cpos{c.x - cs.x * 0.5f, y + 14};
        ImGui::SetCursorScreenPos({cpos.x - 8, cpos.y - 5});
        if (ImGui::InvisibleButton("##clearRecent", {cs.x + 16, cs.y + 10})) out.clearRecent = true;
        const bool ch = ImGui::IsItemHovered();
        if (ch) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddText(font, 14.0f, cpos, TC(255, 255, 255, ch ? 200 : 100), clearLabel);
    }

    // at the bottom: "Make Tolga Player your default video player", or what came of it
    if (!m_defaultNote.empty()) {
        const float w = std::min(560.0f, size.x - 40.0f);
        const ImVec2 ns = font->CalcTextSizeA(14.0f, FLT_MAX, w, m_defaultNote.c_str());
        dl->AddText(font, 14.0f, {c.x - ns.x * 0.5f, size.y - 26 - ns.y}, TC(255, 255, 255, 140),
                    m_defaultNote.c_str(), nullptr, w);
    } else if (m_offerDefault) {
        const char* label = T("Make Tolga Player your default video player");
        const ImVec2 ds = font->CalcTextSizeA(14.0f, FLT_MAX, 0, label);
        const ImVec2 dpos{c.x - ds.x * 0.5f, size.y - 26 - ds.y};
        ImGui::SetCursorScreenPos({dpos.x - 8, dpos.y - 5});
        if (ImGui::InvisibleButton("##makeDefault", {ds.x + 16, ds.y + 10})) out.makeDefaultPlayer = true;
        const bool dh = ImGui::IsItemHovered();
        if (dh) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddText(font, 14.0f, dpos, TC(255, 255, 255, dh ? 210 : 105), label);
    }

    ImGui::End();
}

// ------------------------------------------------------------- title bar

void Overlay::drawTitleBar(const PlayerState& st) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float a = m_alpha;
    dl->AddRectFilledMultiColor({0, 0}, {size.x, 90},
        col(0, 0, 0, 0.55f, a), col(0, 0, 0, 0.55f, a), col(0, 0, 0, 0, a), col(0, 0, 0, 0, a));
    dl->AddText(ImGui::GetFont(), 17.0f, {24, 20}, col(1, 1, 1, 0.92f, a), st.title.c_str());
}

// Windowed on macOS the native title bar is transparent over the video: a soft shade
// keeps the traffic lights and the title readable while they are shown.
void Overlay::drawTitleShade() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float a = m_alpha;
    dl->AddRectFilledMultiColor({0, 0}, {size.x, 56},
        col(0, 0, 0, 0.50f, a), col(0, 0, 0, 0.50f, a), col(0, 0, 0, 0, a), col(0, 0, 0, 0, a));
}

// Linux: the window's title and buttons, drawn by Tolga over the top of the video like the
// macOS title bar: they fade with the controls (always there on the start screen). The
// strip itself drags the window (SDL hit test in main.cpp).
void Overlay::drawWindowChrome(const PlayerState& st, OverlayActions& out) {
    const float a = st.idle ? 1.0f : m_alpha;
    if (a <= 0.01f) return;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float h = kChromeH, bw = kChromeButtonW;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!st.idle && !st.title.empty()) {
        const float maxW = size.x - 3 * bw - 28.0f;
        dl->PushClipRect({14, 0}, {14 + std::max(0.0f, maxW), h}, true);
        dl->AddText(ImGui::GetFont(), 14.0f, {14, (h - 14.0f) * 0.5f}, col(1, 1, 1, 0.88f, a), st.title.c_str());
        dl->PopClipRect();
    }

    ImGui::SetNextWindowPos({size.x - 3 * bw, 0});
    ImGui::SetNextWindowSize({3 * bw, h});
    ImGui::Begin("##chrome", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    for (int i = 0; i < 3; ++i) {
        const ImVec2 p0{size.x - (3 - i) * bw, 0}, p1{p0.x + bw, h};
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##winbtn", {bw, h});
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) {
            m_hoveringPanel = true;
            // close turns red like everywhere else; the others get a light wash
            dl->AddRectFilled(p0, p1, i == 2 ? IM_COL32(196, 43, 28, int(255 * a)) : col(1, 1, 1, 0.12f, a));
        }
        const ImU32 c = i == 2 && hov ? IM_COL32(255, 255, 255, int(255 * a)) : col(1, 1, 1, 0.90f, a);
        const ImVec2 m{(p0.x + p1.x) * 0.5f, h * 0.5f};
        const float r = 5.0f;
        if (i == 0) {
            dl->AddLine({m.x - r, m.y}, {m.x + r, m.y}, c, 1.2f);
        } else if (i == 1 && !m_maximized) {
            dl->AddRect({m.x - r, m.y - r}, {m.x + r, m.y + r}, c, 0.0f, 0, 1.2f);
        } else if (i == 1) {  // restore: a square with another one peeking out behind it
            dl->AddRect({m.x - r, m.y - r + 2}, {m.x + r - 2, m.y + r}, c, 0.0f, 0, 1.2f);
            dl->AddLine({m.x - r + 2, m.y - r + 2}, {m.x - r + 2, m.y - r}, c, 1.2f);
            dl->AddLine({m.x - r + 2, m.y - r}, {m.x + r, m.y - r}, c, 1.2f);
            dl->AddLine({m.x + r, m.y - r}, {m.x + r, m.y + r - 2}, c, 1.2f);
            dl->AddLine({m.x + r, m.y + r - 2}, {m.x + r - 2, m.y + r - 2}, c, 1.2f);
        } else {
            dl->AddLine({m.x - r, m.y - r}, {m.x + r, m.y + r}, c, 1.2f);
            dl->AddLine({m.x - r, m.y + r}, {m.x + r, m.y - r}, c, 1.2f);
        }
        if (clicked) {
            if (i == 0) out.minimizeWindow = true;
            if (i == 1) out.toggleMaximize = true;
            if (i == 2) out.closeWindow = true;
        }
    }
    ImGui::End();
}

// ------------------------------------------------------------- control bar

void Overlay::drawControls(const PlayerState& st, bool fullscreen, const AirPlayView& airplay, OverlayActions& out) {
    ImVec2 size = ImGui::GetIO().DisplaySize;
    size.x -= m_panelVisibleW + m_leftVisibleW;  // keep the bar between the playlist and the side panel
    const float a = m_alpha;

    // A fixed width, like IINA and QuickTime: the bar keeps its size while the window is
    // resized and only gets narrower when the window itself is too narrow for it.
    constexpr float kBarW = 540.0f;
    const float panelW = std::max(340.0f, std::min(kBarW, size.x - 32.0f));
    const float panelH = kPanelH;
    const ImVec2 p0{m_leftVisibleW + (size.x - panelW) * 0.5f, size.y - panelH - kPanelBottom};
    const ImVec2 p1{p0.x + panelW, p0.y + panelH};

    ImGui::SetNextWindowPos(p0);
    ImGui::SetNextWindowSize({panelW, panelH});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, TV(0.07f, 0.07f, 0.09f, (g_light ? 0.86f : 0.72f) * a));  // light: less of the video through
    ImGui::Begin("##controls", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // hairline border gives the "glass" panel some definition
    dl->AddRect(p0, p1, col(1, 1, 1, 0.08f, a), 16.0f, 0, 1.0f);

    m_hoveringPanel = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // While casting the panel is a remote for the Apple TV: time comes from the TV.
    // Volume stays with the TV's own remote, so that slider is disabled.
    const CastStatus* cast = airplay.cast && airplay.cast->active() ? airplay.cast : nullptr;
    const double timePos  = cast ? cast->position : st.timePos;
    const double duration = cast ? cast->duration : st.duration;
    const bool   paused   = cast ? cast->phase == CastStatus::Phase::Paused ||
                                   cast->phase == CastStatus::Phase::Ended
                                 : st.paused;

    // IINA-style layout:
    //   row 1: playlist + volume on the left, transport in the middle, AirPlay / fullscreen / settings on the right
    //   row 2: elapsed time | seek bar | duration
    const float pad = 20.0f;
    const float row1 = p0.y + 32.0f;
    const float rowY = p0.y + 70.0f;
    const bool tiny = panelW < 480.0f;     // side panels open / small window: fewer things
    const bool compact = panelW < 500.0f;  // no volume slider
    const ImU32 white = col(1, 1, 1, 0.95f, a);
    const ImU32 dimmed = col(1, 1, 1, cast ? 0.30f : 0.95f, a);

    // ---- right cluster (from the right edge): settings, fullscreen, AirPlay
    const float step = 38.0f;
    const ImVec2 gearC{p1.x - pad - 10, row1};
    if (iconButton("##settings", gearC, 15, a)) togglePanel(m_panelTab);
    iconGear(dl, gearC, 18, white, m_panelOpen);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", T("Settings"));

    const ImVec2 fsC{gearC.x - step, row1};
    if (iconButton("##fs", fsC, 15, a)) out.toggleFullscreen = true;
    iconFullscreen(dl, fsC, 18, white, fullscreen);

    const ImVec2 apC{fsC.x - step, row1};
    if (iconButton("##airplay", apC, 15, a)) {
        ImGui::OpenPopup("##airplay_menu");
        out.airplayMenuOpened = true;
    }
    iconAirPlay(dl, apC, 20, white, !airplay.castTarget.empty());
    ImGui::SetNextWindowPos({apC.x + 14, p0.y - 8}, ImGuiCond_Always, {1.0f, 1.0f});
    drawAirPlayMenu(airplay, out);

    // ---- transport in the middle (moves left when the bar is narrow)
    const float rightEdge = apC.x - 16;
    float midX = (p0.x + p1.x) * 0.5f;
    if (midX + 56 + 18 > rightEdge) midX = rightEdge - 56 - 18;
    const ImVec2 mid{midX, row1};
    if (iconButton("##back", {mid.x - 56, row1}, 17, a)) out.seekRelative = -10;
    iconSkip(dl, {mid.x - 56, row1}, 22, white, false);
    if (iconButton("##play", mid, 22, a)) out.togglePause = true;
    if (paused) iconPlay(dl, {mid.x + 1, mid.y}, 24, white);
    else        iconPause(dl, mid, 24, white);
    if (iconButton("##fwd", {mid.x + 56, row1}, 17, a)) out.seekRelative = 10;
    iconSkip(dl, {mid.x + 56, row1}, 22, white, true);

    // ---- playlist, then volume, on the left
    const ImVec2 listC{p0.x + pad + 10, row1};
    if (iconButton("##playlist", listC, 15, a)) togglePlaylist();
    iconPlaylist(dl, listC, 18, white, m_playlistOpen);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", T("Playlist"));

    if (!tiny) {
        const ImVec2 spk{listC.x + step, row1};
        ImGui::BeginDisabled(cast != nullptr);  // the TV's own remote sets the volume
        if (iconButton("##mute", spk, 15, a)) out.toggleMute = true;
        const float volX = spk.x + 22;
        const float volW = std::min(90.0f, mid.x - 56 - 48 - volX);  // keep clear of the transport
        float vol = st.muted ? 0.0f : float(st.volume / 100.0);
        if (!compact && volW > 40 && thinSlider("##vol", {volX, row1}, volW, vol, a)) {
            out.setVolume = true;
            out.volume = vol * 100.0;
        }
        ImGui::EndDisabled();
        iconVolume(dl, spk, 18, dimmed, st.volume, st.muted);
    }

    // ---- row 2: elapsed | seek bar | duration
    const double shownTime = m_dragging ? m_dragPos : timePos;
    const std::string cur = formatTime(shownTime);
    const std::string dur = formatTime(duration);
    const float labelW = std::max(ImGui::CalcTextSize("00:00:00").x, 44.0f);
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 cs = ImGui::CalcTextSize(cur.c_str());
    dl->AddText({p0.x + pad + labelW - cs.x, rowY - lh * 0.5f}, col(1, 1, 1, 0.85f, a), cur.c_str());
    dl->AddText({p1.x - pad - labelW, rowY - lh * 0.5f}, col(1, 1, 1, 0.55f, a), dur.c_str());

    const float barX = p0.x + pad + labelW + 14.0f;
    const float barW = (p1.x - pad - labelW - 14.0f) - barX;
    float frac = duration > 0 ? float(shownTime / duration) : 0.0f;
    bool released = false;
    float hoverFrac = -1.0f;
    const bool active = thinSlider("##seek", {barX, rowY}, barW, frac, a, &released, &hoverFrac);

    if (active && duration > 0) {
        m_dragging = true;
        m_dragPos = frac * duration;
        // live scrubbing on keyframes, throttled so we don't flood mpv
        const uint64_t now = SDL_GetTicks();
        if (std::abs(m_dragPos - m_lastSentSeek) > 0.2 && now - m_lastSeekTime > 60) {
            out.seekAbsolute = true;
            out.seekExact = false;
            out.seekTo = m_dragPos;
            m_lastSentSeek = m_dragPos;
            m_lastSeekTime = now;
        }
    }
    if (released && m_dragging) {
        out.seekAbsolute = true;
        out.seekExact = true;   // land exactly where the user let go
        out.seekTo = m_dragPos;
        m_dragging = false;
        m_lastSentSeek = -1.0;
    }
    if (hoverFrac >= 0.0f && duration > 0) {
        // preview picture above the bar (IINA style), then the time bubble on its lower edge
        const double hoverTime = hoverFrac * duration;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        float bubbleY = rowY - 24.0f;
        int tw = 0, th = 0;
        const int idx = m_thumbs ? m_thumbs->frameAt(hoverTime, m_thumbPixels, tw, th) : -1;
        if (idx >= 0 && tw > 0 && th > 0) {
            if (!m_thumbTex) {
                GLuint tex = 0;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                m_thumbTex = tex;
                m_thumbIndex = -1;
            }
            if (idx != m_thumbIndex) {
                glBindTexture(GL_TEXTURE_2D, m_thumbTex);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_thumbPixels.data());
                m_thumbIndex = idx;
            }
            const float iw = 200.0f, ih = iw * th / tw;
            const float cx = std::clamp(barX + barW * hoverFrac, p0.x + iw * 0.5f + 6, p1.x - iw * 0.5f - 6);
            const ImVec2 i1{cx - iw * 0.5f, rowY - 30.0f - ih}, i2{cx + iw * 0.5f, rowY - 30.0f};
            fg->AddRectFilled({i1.x - 2, i1.y - 2}, {i2.x + 2, i2.y + 2}, col(0.05f, 0.05f, 0.06f, 0.95f, a), 8.0f);
            fg->AddImageRounded(ImTextureRef(ImTextureID(intptr_t(m_thumbTex))), i1, i2, {0, 0}, {1, 1},
                                IM_COL32(255, 255, 255, int(255 * a)), 6.0f);  // the picture itself, untinted
            fg->AddRect({i1.x - 2, i1.y - 2}, {i2.x + 2, i2.y + 2}, col(1, 1, 1, 0.18f, a), 8.0f);
            bubbleY = i2.y - 2.0f;
        }
        const std::string t = formatTime(hoverTime);
        ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        ImVec2 c{barX + barW * hoverFrac, bubbleY};
        fg->AddRectFilled({c.x - ts.x * 0.5f - 8, c.y - ts.y * 0.5f - 4},
                          {c.x + ts.x * 0.5f + 8, c.y + ts.y * 0.5f + 4}, col(0.05f, 0.05f, 0.06f, 0.9f, a), 6.0f);
        fg->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, col(1, 1, 1, 1, a), t.c_str());
    }

    ImGui::End();
}

void Overlay::drawAirPlayMenu(const AirPlayView& airplay, OverlayActions& out) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10, 10});
    ImGui::SetNextWindowSizeConstraints({260, 0}, {420, 480});
    const bool open = ImGui::BeginPopup("##airplay_menu");
    if (!open) {
        ImGui::PopStyleVar();
        return;
    }
    m_menuOpen = true;
    if (ImGui::IsWindowHovered()) m_hoveringPanel = true;

    ImGui::TextDisabled("AIRPLAY");
    if (!airplay.available) {
        // built from source without the AirPlay module
        ImGui::MenuItem(T("This Computer"), nullptr, true);
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 250);
        ImGui::TextDisabled("%s", T("Playing on an Apple TV is part of the official Tolga Player app."));
        ImGui::PopTextWrapPos();
        if (ImGui::MenuItem(T("Get Tolga Player..."))) out.openBuyPage = true;
        ImGui::EndPopup();
        ImGui::PopStyleVar();
        return;
    }
    if (ImGui::MenuItem(T("This Computer"), nullptr, airplay.castTarget.empty())) {
        out.selectCastTarget = true;
        out.castTarget.clear();
    }

    const size_t count = airplay.devices ? airplay.devices->size() : 0;
    for (size_t i = 0; i < count; ++i) {
        const AirPlayTarget& d = (*airplay.devices)[i];
        ImGui::PushID(int(i));
        const bool selected = !d.deviceId.empty() && d.deviceId == airplay.castTarget;
        const bool video = d.video;
        // name on the left, model in grey on the right; greyed out when this video can't go to a TV
        if (ImGui::MenuItem(d.name.c_str(), d.model.c_str(), selected, video && airplay.castBlocked.empty())) {
            out.selectCastTarget = true;
            out.castTarget = d.deviceId;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) {
            const std::string tip = video ? d.details : d.details + "\n" + T("Audio only - can't play video");
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImGui::PopID();
    }

    if (!airplay.castBlocked.empty() && count > 0) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 250);
        ImGui::TextDisabled("%s", airplay.castBlocked.c_str());
        ImGui::PopTextWrapPos();
    }

    if (airplay.searching || count == 0) {
        // animated dots while the network is being scanned
        const int dots = int(ImGui::GetTime() * 3.0) % 4;
        std::string text = airplay.searching ? T("Searching") + std::string(dots, '.') : T("No AirPlay devices found");
        ImGui::Spacing();
        ImGui::TextDisabled("%s", text.c_str());
    }

    // official builds: trial / license
    if (const LicenseInfo* lic = airplay.license) {
        using S = LicenseInfo::State;
        if (lic->state == S::Trial || lic->state == S::Expired) {
            ImGui::Separator();
            if (lic->state == S::Trial)
                ImGui::TextDisabled("%s", lic->trialDaysLeft == 1 ? T("Free trial: 1 day left")
                                     : TF("Free trial: {} days left", {std::to_string(lic->trialDaysLeft)}).c_str());
            else
                ImGui::TextDisabled("%s", T("The AirPlay trial has ended"));
            if (ImGui::MenuItem(lic->state == S::Trial ? T("Enter License...") : T("Unlock AirPlay...")))
                out.openLicense = true;
        }
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

// ================================================================== side panel

namespace {

constexpr ImU32 kAccent = IM_COL32(34, 196, 186, 255);

// ISO 639-2 codes (what files and mpv use) -> English names for the language badges.
std::string languageName(const std::string& code) {
    static const std::pair<const char*, const char*> names[] = {
        {"tur", "Turkish"}, {"tr", "Turkish"}, {"eng", "English"}, {"en", "English"}, {"ger", "German"},
        {"deu", "German"}, {"de", "German"}, {"fre", "French"}, {"fra", "French"}, {"fr", "French"},
        {"spa", "Spanish"}, {"es", "Spanish"}, {"ita", "Italian"}, {"it", "Italian"}, {"rus", "Russian"},
        {"ru", "Russian"}, {"jpn", "Japanese"}, {"ja", "Japanese"}, {"chi", "Chinese"}, {"zho", "Chinese"},
        {"zh", "Chinese"}, {"kor", "Korean"}, {"ko", "Korean"}, {"ara", "Arabic"}, {"ar", "Arabic"},
        {"por", "Portuguese"}, {"pt", "Portuguese"}, {"dut", "Dutch"}, {"nld", "Dutch"}, {"nl", "Dutch"},
        {"pol", "Polish"}, {"pl", "Polish"}, {"swe", "Swedish"}, {"sv", "Swedish"}, {"gre", "Greek"},
        {"ell", "Greek"}, {"el", "Greek"}, {"aze", "Azerbaijani"}, {"az", "Azerbaijani"}, {"per", "Persian"},
        {"fas", "Persian"}, {"heb", "Hebrew"}, {"hin", "Hindi"}, {"ukr", "Ukrainian"}, {"dan", "Danish"},
        {"fin", "Finnish"}, {"nor", "Norwegian"}, {"cze", "Czech"}, {"ces", "Czech"}, {"hun", "Hungarian"},
        {"rum", "Romanian"}, {"ron", "Romanian"}, {"bul", "Bulgarian"},
    };
    std::string l = code;
    for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& [k, n] : names)
        if (l == k) return T(n);
    if (l.empty() || l == "und") return {};
    return code;
}

std::string fileName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Whether the controls can show `text` properly: every character is in the font, and none
// belongs to a script that needs shaping or right-to-left layout (Arabic, Hebrew, Persian,
// Indic, Thai...), which ImGui draws as separate, reversed letters.
bool displayable(const std::string& text) {
    ImFont* font = ImGui::GetFont();
    const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
    const auto* end = p + text.size();
    while (p < end) {
        unsigned int c = *p;
        int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || p + n > end) return false;  // not UTF-8
        if (n > 1) c &= 0xFFu >> (n + 1);
        for (int i = 1; i < n; ++i) {
            if ((p[i] & 0xC0) != 0x80) return false;
            c = (c << 6) | (p[i] & 0x3F);
        }
        p += n;
        if (c > 0xFFFF && sizeof(ImWchar) == 2) return false;  // beyond what this ImGui build stores
        if ((c >= 0x0590 && c <= 0x0DFF) || (c >= 0x0E00 && c <= 0x0EFF) ||
            (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF))
            return false;
        if (c > 0x7F && font && !font->IsGlyphInFont(ImWchar(c)))
            return false;
    }
    return true;
}

// "No Title (mov_text, Default)" / "Turkish.srt (subrip, External)" / "English (aac, 5.1, Default)"
std::string trackText(const Track& t) {
    std::string name = !t.title.empty() ? t.title : t.external ? fileName(t.externalFile) : T("No Title");
    if (!displayable(name)) {  // e.g. an Arabic title: show "Arabic" from the language code instead
        const std::string lang = languageName(t.lang);
        name = !lang.empty() ? lang : TF("Track {}", {std::to_string(t.id)});
    }
    std::vector<std::string> info;
    if (!t.codec.empty()) info.push_back(t.codec);
    if (t.type == "audio" && t.channels > 0)
        info.push_back(t.channels == 6 ? "5.1" : t.channels == 8 ? "7.1" : t.channels == 2 ? T("Stereo")
                                                                    : t.channels == 1 ? T("Mono")
                                                                    : TF("{} ch", {std::to_string(t.channels)}));
    if (t.type == "video" && t.width > 0) info.push_back(std::to_string(t.width) + "x" + std::to_string(t.height));
    if (t.type == "video" && t.fps > 0) {
        char f[24];
        std::snprintf(f, sizeof f, "%.3gfps", t.fps);
        info.push_back(f);
    }
    if (t.isDefault) info.push_back(T("Default"));
    if (t.forced) info.push_back(T("Forced"));
    if (t.external) info.push_back(T("External"));
    std::string out = name;
    if (!info.empty()) {
        out += " (";
        for (size_t i = 0; i < info.size(); ++i) out += (i ? ", " : "") + info[i];
        out += ")";
    }
    return out;
}

// iOS-style switch; returns true when clicked.
bool toggleSwitch(const char* id, ImVec2 pos, bool on) {
    const ImVec2 size{44, 24};
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, on ? kAccent : TC(255, 255, 255, 45), size.y * 0.5f);
    const float kx = on ? pos.x + size.x - size.y * 0.5f : pos.x + size.y * 0.5f;
    dl->AddCircleFilled({kx, pos.y + size.y * 0.5f}, size.y * 0.5f - 3, TC(255, 255, 255, 250), 24);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

// Pill button with text; returns true when clicked.
bool pillButton(const char* label, ImVec2 pos, float height = 34.0f) {
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 size{ts.x + 40, height};
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(label);
    const bool clicked = ImGui::InvisibleButton("##pill", size);
    ImGui::PopID();
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, TC(255, 255, 255, hov ? 40 : 26), 8.0f);
    dl->AddText({pos.x + 20, pos.y + (size.y - ts.y) * 0.5f}, TC(255, 255, 255, 240), label);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

// Small tab icons
void iconPicture(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddRect({c.x - 9, c.y - 7}, {c.x + 9, c.y + 7}, col, 2.5f, 0, 1.6f);
    dl->AddTriangleFilled({c.x - 6, c.y + 4}, {c.x - 1, c.y - 2}, {c.x + 3, c.y + 4}, col);
    dl->AddTriangleFilled({c.x + 1, c.y + 4}, {c.x + 4, c.y}, {c.x + 7, c.y + 4}, col);
    dl->AddCircleFilled({c.x + 4, c.y - 3}, 1.6f, col);
}

void iconWaveform(ImDrawList* dl, ImVec2 c, ImU32 col) {
    const float h[] = {4, 9, 13, 7, 11, 5, 3};
    for (int i = 0; i < 7; ++i) {
        const float x = c.x - 9 + i * 3.0f;
        dl->AddLine({x, c.y - h[i] * 0.5f}, {x, c.y + h[i] * 0.5f}, col, 1.6f);
    }
}

void iconBubble(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddRectFilled({c.x - 9, c.y - 7}, {c.x + 9, c.y + 5}, col, 3.0f);
    dl->AddTriangleFilled({c.x - 6, c.y + 4}, {c.x - 6, c.y + 9}, {c.x - 1, c.y + 4}, col);
    const ImU32 hole = TC(40, 40, 46, 255);
    dl->AddLine({c.x - 5, c.y - 3}, {c.x + 2, c.y - 3}, hole, 1.5f);
    dl->AddLine({c.x + 4, c.y - 3}, {c.x + 5, c.y - 3}, hole, 1.5f);
    dl->AddLine({c.x - 5, c.y + 1}, {c.x - 2, c.y + 1}, hole, 1.5f);
    dl->AddLine({c.x, c.y + 1}, {c.x + 5, c.y + 1}, hole, 1.5f);
}

// A rounded list of tracks with "<None>" on top. `selected` is the chosen track ID (-1 = none).
// Returns true and sets `picked` when a row was clicked.
bool trackList(const char* id, const std::vector<const Track*>& tracks, int64_t selected, bool withNone,
               float width, int64_t& picked) {
    const float rowH = 38.0f;
    const int rows = int(tracks.size()) + (withNone ? 1 : 0);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1{p0.x + width, p0.y + std::max(rows, 1) * rowH + 8};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, TC(255, 255, 255, 10), 10.0f);
    dl->AddRect(p0, p1, TC(255, 255, 255, 22), 10.0f);

    bool clicked = false;
    ImGui::PushID(id);
    for (int r = 0; r < rows; ++r) {
        const Track* t = withNone ? (r == 0 ? nullptr : tracks[size_t(r - 1)]) : tracks[size_t(r)];
        const int64_t tid = t ? t->id : -1;
        const ImVec2 a{p0.x + 4, p0.y + 4 + r * rowH};
        const ImVec2 b{p1.x - 4, a.y + rowH};
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(r);
        if (ImGui::InvisibleButton("##row", {b.x - a.x, rowH})) {
            picked = tid;
            clicked = true;
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) dl->AddRectFilled(a, b, TC(255, 255, 255, 14), 7.0f);
        if (r > 0) dl->AddLine({a.x + 10, a.y}, {b.x - 10, a.y}, TC(255, 255, 255, 18));
        if (tid == selected) dl->AddRectFilled({a.x + 8, a.y + 9}, {a.x + 11, b.y - 9}, kAccent, 1.5f);

        const float ty = a.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        float right = b.x - 10;
        if (t) {
            const std::string lang = languageName(t->lang);
            if (!lang.empty()) {
                const ImVec2 ls = ImGui::CalcTextSize(lang.c_str());
                const ImVec2 ba{right - ls.x - 12, a.y + 8}, bb{right, b.y - 8};
                dl->AddRectFilled(ba, bb, TC(255, 255, 255, 34), 5.0f);
                dl->AddText({ba.x + 6, ty}, TC(255, 255, 255, 220), lang.c_str());
                right = ba.x - 8;
            }
            const std::string text = trackText(*t);
            dl->PushClipRect({a.x + 20, a.y}, {right, b.y}, true);
            dl->AddText({a.x + 22, ty}, TC(255, 255, 255, 240), text.c_str());
            dl->PopClipRect();
        } else {
            dl->AddText({a.x + 22, ty}, TC(255, 255, 255, 110), T("<None>"));
        }
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos({p0.x, p1.y});
    ImGui::Dummy({width, 0});
    return clicked;
}

// A rounded list of choices like trackList's; returns true and sets `picked` when a row was clicked.
bool choiceList(const char* id, const std::vector<std::string>& labels, int selected, float width, int& picked) {
    const float rowH = 38.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1{p0.x + width, p0.y + std::max<int>(int(labels.size()), 1) * rowH + 8};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, TC(255, 255, 255, 10), 10.0f);
    dl->AddRect(p0, p1, TC(255, 255, 255, 22), 10.0f);
    bool clicked = false;
    ImGui::PushID(id);
    for (int r = 0; r < int(labels.size()); ++r) {
        const ImVec2 a{p0.x + 4, p0.y + 4 + r * rowH};
        const ImVec2 b{p1.x - 4, a.y + rowH};
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(r);
        if (ImGui::InvisibleButton("##row", {b.x - a.x, rowH})) {
            picked = r;
            clicked = true;
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) dl->AddRectFilled(a, b, TC(255, 255, 255, 14), 7.0f);
        if (r > 0) dl->AddLine({a.x + 10, a.y}, {b.x - 10, a.y}, TC(255, 255, 255, 18));
        if (r == selected) dl->AddRectFilled({a.x + 8, a.y + 9}, {a.x + 11, b.y - 9}, kAccent, 1.5f);
        const float ty = a.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        dl->PushClipRect({a.x + 20, a.y}, {b.x - 10, b.y}, true);
        dl->AddText({a.x + 22, ty}, TC(255, 255, 255, 240), labels[size_t(r)].c_str());
        dl->PopClipRect();
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos({p0.x, p1.y});
    ImGui::Dummy({width, 0});
    return clicked;
}

// "1  Primary ............ [switch]" section header; returns true if the switch was clicked.
bool sectionHeader(const char* number, const char* title, float width, const bool* toggle) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = p.x;
    if (number) {
        dl->AddRectFilled({x, p.y + 4}, {x + 18, p.y + 22}, TC(255, 255, 255, 60), 4.0f);
        const ImVec2 ns = ImGui::CalcTextSize(number);
        dl->AddText({x + 9 - ns.x * 0.5f, p.y + 13 - ns.y * 0.5f}, TC(20, 20, 24, 255), number);
        x += 28;
    }
    dl->AddText(ImGui::GetFont(), 17.0f, {x, p.y + 3}, TC(255, 255, 255, 245), title);
    bool clicked = false;
    if (toggle) {
        ImGui::PushID(title);
        clicked = toggleSwitch("##sw", {p.x + width - 44, p.y + 1}, *toggle);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos({p.x, p.y + 36});
    ImGui::Dummy({width, 0});
    return clicked;
}

}  // namespace

namespace {

// Rounded text field; returns true when Enter was pressed.
bool textField(const char* id, char* buf, size_t size, float width, ImGuiInputTextFlags flags = 0,
               const char* hint = nullptr) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 8});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, TV(1, 1, 1, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, TV(1, 1, 1, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, TV(1, 1, 1, 0.12f));
    ImGui::SetNextItemWidth(width);
    const bool enter = hint ? ImGui::InputTextWithHint(id, hint, buf, size, flags | ImGuiInputTextFlags_EnterReturnsTrue)
                            : ImGui::InputText(id, buf, size, flags | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    return enter;
}

void label(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, TV(1, 1, 1, 0.55f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// "‹ Title" header row; returns true when the back button was clicked.
bool backHeader(const char* title) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(p);
    const bool back = ImGui::InvisibleButton("##back", {30, 28});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c{p.x + 10, p.y + 14};
    dl->AddLine({c.x + 4, c.y - 7}, {c.x - 3, c.y}, TC(255, 255, 255, 230), 2.0f);
    dl->AddLine({c.x - 3, c.y}, {c.x + 4, c.y + 7}, TC(255, 255, 255, 230), 2.0f);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dl->AddText(ImGui::GetFont(), 17.0f, {p.x + 30, p.y + 4}, TC(255, 255, 255, 245), title);
    ImGui::SetCursorScreenPos({p.x, p.y + 40});
    return back;
}

}  // namespace

void Overlay::drawAccountForm(const OnlineView& online, float inner, OverlayActions& out) {
    // With a key built into the app only the (optional) login is shown here.
    const bool builtIn = online.settings && online.settings->builtInKey;
    if (backHeader(T("OpenSubtitles Account"))) m_accountOpen = false;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    if (builtIn)
        label(T("Optional: log in with your opensubtitles.com account to raise the daily download limit."));
    else
        label(T("Searching needs a free API key. Create one at opensubtitles.com/en/consumers "
                "and paste it below. An account is optional; logging in raises the daily download limit."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 10});

    if (!builtIn) {
        label(T("API Key"));
        textField("##key", m_keyBuf, sizeof m_keyBuf, inner);
        ImGui::Dummy({0, 6});
    }
    label(builtIn ? T("User Name") : T("User Name (optional)"));
    textField("##user", m_userBuf, sizeof m_userBuf, inner);
    ImGui::Dummy({0, 6});
    label(builtIn ? T("Password") : T("Password (optional)"));
    const bool enter = textField("##pass", m_passBuf, sizeof m_passBuf, inner, ImGuiInputTextFlags_Password);
    ImGui::Dummy({0, 14});

    const ImVec2 bp = ImGui::GetCursorScreenPos();
    if (pillButton(T("Save"), bp) || enter) {
        out.onlineSaveSettings = true;
        out.onlineApiKey = m_keyBuf;
        out.onlineUser = m_userBuf;
        out.onlinePassword = m_passBuf;
        m_accountOpen = false;
    }
    ImGui::Dummy({0, 40});
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    ImGui::TextDisabled("%s", T("Stored on this computer, readable only by your user account."));
    ImGui::PopTextWrapPos();
}

// ------------------------------------------------------------- delay / equalizer

namespace {

void iconClock(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddCircle(c, 7.5f, col, 24, 1.6f);
    dl->AddLine(c, {c.x, c.y - 4.5f}, col, 1.6f);
    dl->AddLine(c, {c.x + 3.5f, c.y + 1.5f}, col, 1.6f);
}

void iconSliders(ImDrawList* dl, ImVec2 c, ImU32 col) {
    const float ys[] = {-2.5f, 3.0f, -4.0f};
    for (int i = 0; i < 3; ++i) {
        const float x = c.x - 5 + i * 5.0f;
        dl->AddLine({x, c.y - 7}, {x, c.y + 7}, col, 1.4f);
        dl->AddRectFilled({x - 2.2f, c.y + ys[i] - 1.6f}, {x + 2.2f, c.y + ys[i] + 1.6f}, col, 1.0f);
    }
}

// Rounded card background from the cursor down to `height`; returns its top-left corner.
ImVec2 card(float width, float height) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + width, p.y + height}, TC(255, 255, 255, 10), 10.0f);
    dl->AddRect(p, {p.x + width, p.y + height}, TC(255, 255, 255, 22), 10.0f);
    return p;
}

struct EqPreset { const char* name; float gains[10]; };
const EqPreset kEqPresets[] = {
    {"Manual",         {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Flat",           {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Bass Booster",   {6, 5, 4, 2, 1, 0, 0, 0, 0, 0}},
    {"Bass Reducer",   {-6, -5, -4, -2, -1, 0, 0, 0, 0, 0}},
    {"Treble Booster", {0, 0, 0, 0, 0, 1, 2, 4, 5, 6}},
    {"Treble Reducer", {0, 0, 0, 0, 0, -1, -2, -4, -5, -6}},
    {"Vocal Booster",  {-2, -3, -3, 1, 4, 4, 3, 1, 0, -2}},
    {"Spoken Word",    {-3, -1, 0, 1, 3, 4, 4, 3, 1, -2}},
    {"Loudness",       {6, 4, 0, 0, -2, 0, -1, -5, 5, 1}},
    {"Rock",           {5, 4, 3, 1, -1, -1, 1, 3, 4, 5}},
    {"Pop",            {-1, 0, 2, 4, 4, 2, 0, -1, -1, -1}},
    {"Jazz",           {4, 3, 1, 2, -2, -2, 0, 1, 3, 4}},
    {"Classical",      {5, 4, 3, 2, -1, -1, 0, 2, 3, 4}},
    {"Electronic",     {4, 4, 1, 0, -2, 2, 1, 1, 4, 5}},
};

}  // namespace

// "Delay" card: a -5..+5 s slider with a number field; returns true (and `changed`) on edits.
bool Overlay::delayCard(const char* id, double value, float inner, double& changed) {
    ImGui::PushID(id);
    const float h = 118.0f;
    const ImVec2 p = card(inner, h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 white = TC(255, 255, 255, 240), grey = TC(255, 255, 255, 140);
    iconClock(dl, {p.x + 24, p.y + 24}, white);
    dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 42, p.y + 15}, white, T("Delay"));

    bool edited = false;
    const float fieldW = 72.0f;
    const float x0 = p.x + 22, x1 = p.x + inner - fieldW - 34;
    const float ty = p.y + 70;
    const float range = 5.0f;
    // ticks every 0.5 s
    for (int i = 0; i <= 20; ++i) {
        const float x = x0 + (x1 - x0) * i / 20.0f;
        dl->AddCircleFilled({x, ty + 9}, i % 10 == 0 ? 1.4f : 0.9f, TC(255, 255, 255, 70));
    }
    dl->AddLine({x0, ty}, {x1, ty}, TC(255, 255, 255, 45), 3.0f);

    ImGui::SetCursorScreenPos({x0 - 8, ty - 14});
    ImGui::InvisibleButton("##track", {x1 - x0 + 16, 28});
    if (ImGui::IsItemActive()) {
        double v = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0) * 2 * range - range;
        v = std::clamp(v, -double(range), double(range));
        v = std::round(v * 10.0) / 10.0;
        if (std::abs(v) < 0.15) v = 0.0;  // easy to get back to 0
        if (v != value) { changed = v; edited = true; }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { changed = 0.0; edited = true; }
    const double shown = edited ? changed : value;
    const float kx = x0 + (x1 - x0) * float((std::clamp(shown, -double(range), double(range)) + range) / (2 * range));
    dl->AddRectFilled({kx - 4, ty - 10}, {kx + 4, ty + 10}, TC(255, 255, 255, 250), 4.0f);

    char buf[32];
    std::snprintf(buf, sizeof buf, "%gs", std::round(shown * 10.0) / 10.0);
    const ImVec2 vs = ImGui::CalcTextSize(buf);
    dl->AddText({kx - vs.x * 0.5f, ty - 30}, grey, buf);
    const char* labels[] = {"-5s", "0s", "+5s"};
    const float lx[] = {x0, (x0 + x1) * 0.5f, x1};
    for (int i = 0; i < 3; ++i) {
        const ImVec2 ls = ImGui::CalcTextSize(labels[i]);
        dl->AddText({lx[i] - ls.x * 0.5f, ty + 18}, grey, labels[i]);
    }

    // exact value
    ImGui::SetCursorScreenPos({p.x + inner - fieldW - 18, ty - 16});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8, 7});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, TV(1, 1, 1, 0.07f));
    ImGui::SetNextItemWidth(fieldW);
    double fieldValue = shown;
    if (ImGui::InputDouble("##value", &fieldValue, 0, 0, "%.1f", ImGuiInputTextFlags_EnterReturnsTrue)) {
        changed = std::clamp(fieldValue, -60.0, 60.0);
        edited = true;
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImGui::SetCursorScreenPos({p.x, p.y + h});
    ImGui::Dummy({inner, 0});
    ImGui::PopID();
    return edited;
}

void Overlay::equalizerCard(float inner, OverlayActions& out) {
    const float h = 316.0f;
    const ImVec2 p = card(inner, h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 white = TC(255, 255, 255, 240), grey = TC(255, 255, 255, 140);
    iconSliders(dl, {p.x + 24, p.y + 24}, white);
    dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 42, p.y + 15}, white, T("Equalizer"));

    bool changed = false;
    // ---- preset menu
    ImGui::SetCursorScreenPos({p.x + 18, p.y + 46});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 7});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6, 6});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, TV(1, 1, 1, 0.09f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, TV(1, 1, 1, 0.13f));
    ImGui::PushStyleColor(ImGuiCol_Button, TV(1, 1, 1, 0.0f));
    ImGui::SetNextItemWidth(inner - 36 - 70);
    if (ImGui::BeginCombo("##preset", T(kEqPresets[m_eqPreset].name), ImGuiComboFlags_HeightLargest)) {
        m_menuOpen = true;
        for (int i = 1; i < int(std::size(kEqPresets)); ++i) {
            if (ImGui::Selectable(T(kEqPresets[i].name), i == m_eqPreset)) {
                m_eqPreset = i;
                std::copy(std::begin(kEqPresets[i].gains), std::end(kEqPresets[i].gains), m_eqGains);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);
    // reset
    ImGui::SameLine(0, 10);
    ImGui::SetCursorScreenPos({p.x + inner - 18 - 60, p.y + 46});
    ImGui::PushID("eqreset");
    if (ImGui::InvisibleButton("##reset", {60, 31})) {
        std::fill(std::begin(m_eqGains), std::end(m_eqGains), 0.0f);
        m_eqPreset = 1;  // Flat
        changed = true;
    }
    const bool rh = ImGui::IsItemHovered();
    ImGui::PopID();
    const ImVec2 rs = ImGui::CalcTextSize(T("Reset"));
    dl->AddText({p.x + inner - 18 - 30 - rs.x * 0.5f, p.y + 46 + (31 - rs.y) * 0.5f},
                TC(255, 255, 255, rh ? 230 : 140), T("Reset"));

    // ---- 10 vertical band sliders, -12..+12 dB
    const float top = p.y + 104, bottom = p.y + 266;
    const float labelW = 70.0f;
    const float x0 = p.x + 30, x1 = p.x + inner - labelW - 10;
    const float step = (x1 - x0) / 9.0f;
    const float zeroY = (top + bottom) * 0.5f;
    bool dragging = false;
    for (int b = 0; b < 10; ++b) {
        const float x = x0 + b * step;
        for (int t = 0; t <= 8; ++t)  // dots every 3 dB
            dl->AddCircleFilled({x + 7, top + (bottom - top) * t / 8.0f}, 0.9f, TC(255, 255, 255, 70));
        dl->AddLine({x, top}, {x, bottom}, TC(255, 255, 255, 45), 3.0f);
        ImGui::SetCursorScreenPos({x - 12, top - 8});
        ImGui::PushID(b);
        ImGui::InvisibleButton("##band", {24, bottom - top + 16});
        if (ImGui::IsItemActive()) {
            dragging = true;
            float g = (zeroY - ImGui::GetIO().MousePos.y) / ((bottom - top) * 0.5f) * 12.0f;
            g = std::clamp(std::round(g * 2.0f) / 2.0f, -12.0f, 12.0f);  // 0.5 dB steps
            if (std::abs(g) < 0.6f) g = 0.0f;
            if (g != m_eqGains[b]) {
                m_eqGains[b] = g;
                m_eqPreset = 0;  // Manual
                changed = true;
            }
        }
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDoubleClicked(0)) { m_eqGains[b] = 0; m_eqPreset = 0; changed = true; }
            ImGui::SetTooltip("%s Hz: %+.1f dB",
                              b < 5 ? std::to_string(int(MpvPlayer::kEqFrequencies[b])).c_str()
                                    : (std::to_string(int(MpvPlayer::kEqFrequencies[b] / 1000)) + "k").c_str(),
                              m_eqGains[b]);
        }
        ImGui::PopID();
        const float ky = zeroY - m_eqGains[b] / 12.0f * (bottom - top) * 0.5f;
        dl->AddRectFilled({x - 9, ky - 4}, {x + 9, ky + 4}, TC(255, 255, 255, 250), 4.0f);
        // frequency label
        const float f = MpvPlayer::kEqFrequencies[b];
        const std::string fl = f < 1000 ? std::to_string(int(f)) : std::to_string(int(f / 1000)) + "k";
        const ImVec2 fs = ImGui::GetFont()->CalcTextSizeA(13.0f, FLT_MAX, 0, fl.c_str());
        dl->AddText(ImGui::GetFont(), 13.0f, {x - fs.x * 0.5f, bottom + 14}, grey, fl.c_str());
    }
    const char* dbl[] = {"+12 dB", "0 dB", "-12 dB"};
    const float dby[] = {top, zeroY, bottom};
    for (int i = 0; i < 3; ++i) {
        const ImVec2 ds = ImGui::CalcTextSize(dbl[i]);
        dl->AddText({p.x + inner - 16 - ds.x, dby[i] - ds.y * 0.5f}, grey, dbl[i]);
    }

    // Rebuilding the filter is not free: while dragging send at most ~7 times a second,
    // and always once more when the drag ends.
    const uint64_t now = SDL_GetTicks();
    const bool released = m_eqDragging && !dragging;
    m_eqDragging = dragging;
    if ((changed && (!dragging || now - m_lastEqSent > 140)) || released) {
        out.setEqualizer = true;
        std::copy(std::begin(m_eqGains), std::end(m_eqGains), out.eqGains);
        m_lastEqSent = now;
    }

    ImGui::SetCursorScreenPos({p.x, p.y + h});
    ImGui::Dummy({inner, 0});
}

// ------------------------------------------------------------- video settings

namespace {

void iconAspect(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddRect({c.x - 8, c.y - 6}, {c.x + 8, c.y + 6}, col, 2.0f, 0, 1.5f);
    dl->AddLine({c.x - 4, c.y - 6}, {c.x - 4, c.y + 6}, col, 1.2f);
    dl->AddLine({c.x - 8, c.y}, {c.x - 4, c.y}, col, 1.2f);
}
void iconCrop(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddLine({c.x - 4, c.y - 8}, {c.x - 4, c.y + 4}, col, 1.6f);
    dl->AddLine({c.x - 8, c.y + 4}, {c.x + 6, c.y + 4}, col, 1.6f);
    dl->AddLine({c.x - 8, c.y - 4}, {c.x + 4, c.y - 4}, col, 1.6f);
    dl->AddLine({c.x + 4, c.y - 4}, {c.x + 4, c.y + 8}, col, 1.6f);
}
void iconRotate(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddRect({c.x - 7, c.y - 3}, {c.x + 4, c.y + 8}, col, 1.5f, 0, 1.5f);
    dl->PathArcTo({c.x - 1, c.y - 2}, 6.0f, 3.6f, 5.6f, 10);
    dl->PathStroke(col, 0, 1.4f);
    dl->AddTriangleFilled({c.x + 4, c.y - 9}, {c.x + 6, c.y - 4}, {c.x + 1, c.y - 5}, col);
}
void iconSpeed(ImDrawList* dl, ImVec2 c, ImU32 col) {
    for (float dx : {-5.0f, 1.0f}) {
        dl->AddLine({c.x + dx - 2, c.y - 6}, {c.x + dx + 3, c.y}, col, 1.6f);
        dl->AddLine({c.x + dx + 3, c.y}, {c.x + dx - 2, c.y + 6}, col, 1.6f);
    }
}
void iconChip(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddRect({c.x - 6, c.y - 6}, {c.x + 6, c.y + 6}, col, 2.0f, 0, 1.4f);
    dl->AddRect({c.x - 2.5f, c.y - 2.5f}, {c.x + 2.5f, c.y + 2.5f}, col, 0.5f, 0, 1.2f);
    for (int i = -1; i <= 1; ++i) {
        const float o = i * 3.5f;
        dl->AddLine({c.x + o, c.y - 9}, {c.x + o, c.y - 6}, col, 1.2f);
        dl->AddLine({c.x + o, c.y + 6}, {c.x + o, c.y + 9}, col, 1.2f);
        dl->AddLine({c.x - 9, c.y + o}, {c.x - 6, c.y + o}, col, 1.2f);
        dl->AddLine({c.x + 6, c.y + o}, {c.x + 9, c.y + o}, col, 1.2f);
    }
}

// Segmented control (IINA style): returns the clicked index or -1.
int segmented(const char* id, const char* const* labels, int count, int selected, ImVec2 pos, float maxW) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float padX = 12.0f, h = 30.0f;
    float total = 0;
    std::vector<float> widths;
    for (int i = 0; i < count; ++i) {
        widths.push_back(ImGui::CalcTextSize(T(labels[i])).x + padX * 2);
        total += widths.back();
    }
    const float scale = total > maxW ? maxW / total : 1.0f;  // squeeze when the panel is narrow
    dl->AddRectFilled(pos, {pos.x + total * scale, pos.y + h}, TC(255, 255, 255, 22), 7.0f);
    int clicked = -1;
    float x = pos.x;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        const float w = widths[size_t(i)] * scale;
        ImGui::SetCursorScreenPos({x, pos.y});
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", {w, h})) clicked = i;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (i == selected)  dl->AddRectFilled({x, pos.y}, {x + w, pos.y + h}, TC(34, 196, 186, 255), 7.0f);
        else if (hov)       dl->AddRectFilled({x, pos.y}, {x + w, pos.y + h}, TC(255, 255, 255, 18), 7.0f);
        if (i > 0 && i != selected && i - 1 != selected)
            dl->AddLine({x, pos.y + 8}, {x, pos.y + h - 8}, TC(255, 255, 255, 50));
        const ImVec2 ts = ImGui::CalcTextSize(T(labels[i]));
        dl->AddText({x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f}, TC(255, 255, 255, 240), T(labels[i]));
        x += w;
    }
    ImGui::PopID();
    return clicked;
}

const char* const kAspects[] = {"Default", "4:3", "16:9", "16:10", "21:9", "5:4"};
const char* const kCrops[]   = {"None", "4:3", "16:9", "16:10", "21:9", "5:4"};
const char* const kRotations[] = {"0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0"};

}  // namespace

void Overlay::videoCards(const PlayerState& st, float inner, OverlayActions& out) {
    const ImU32 white = TC(255, 255, 255, 240), grey = TC(255, 255, 255, 140);

    // ---- aspect / crop / rotation
    {
        const float h = 236.0f;
        const ImVec2 p = card(inner, h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float segW = inner - 36;
        iconAspect(dl, {p.x + 26, p.y + 24}, white);
        dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 44, p.y + 15}, white, T("Aspect Ratio"));
        int c = segmented("aspect", kAspects, 6, m_aspectIdx, {p.x + 18, p.y + 46}, segW);
        if (c >= 0) { m_aspectIdx = c; out.setAspect = true; out.aspect = c == 0 ? "" : kAspects[c]; }

        iconCrop(dl, {p.x + 26, p.y + 100}, white);
        dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 44, p.y + 91}, white, T("Crop"));
        c = segmented("crop", kCrops, 6, m_cropIdx, {p.x + 18, p.y + 122}, segW);
        if (c >= 0) { m_cropIdx = c; out.setCrop = true; out.crop = c == 0 ? "" : kCrops[c]; }

        iconRotate(dl, {p.x + 26, p.y + 176}, white);
        dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 44, p.y + 167}, white, T("Rotation"));
        const int rotIdx = ((st.rotate % 360) + 360) % 360 / 90;
        c = segmented("rot", kRotations, 4, rotIdx, {p.x + 18, p.y + 196}, segW * 0.6f);
        if (c >= 0) { out.setRotate = true; out.rotate = c * 90; }
        ImGui::SetCursorScreenPos({p.x, p.y + h});
        ImGui::Dummy({inner, 0});
    }
    ImGui::Dummy({0, 14});

    // ---- speed: log scale 0.25x .. 16x (1x at one third, like IINA)
    {
        const float h = 118.0f;
        const ImVec2 p = card(inner, h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        iconSpeed(dl, {p.x + 26, p.y + 24}, white);
        dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 44, p.y + 15}, white, T("Speed"));
        const float fieldW = 72.0f;
        const float x0 = p.x + 22, x1 = p.x + inner - fieldW - 34;
        const float ty = p.y + 70;
        auto toX = [&](double sp) { return x0 + (x1 - x0) * float((std::log2(sp) + 2.0) / 6.0); };
        for (int i = 0; i <= 24; ++i)
            dl->AddCircleFilled({x0 + (x1 - x0) * i / 24.0f, ty + 9}, i % 8 == 0 ? 1.4f : 0.9f, TC(255, 255, 255, 70));
        dl->AddLine({x0, ty}, {x1, ty}, TC(255, 255, 255, 45), 3.0f);
        ImGui::SetCursorScreenPos({x0 - 8, ty - 14});
        ImGui::InvisibleButton("##speed", {x1 - x0 + 16, 28});
        double shown = st.speed;
        if (ImGui::IsItemActive()) {
            const double f = std::clamp(double((ImGui::GetIO().MousePos.x - x0) / (x1 - x0)), 0.0, 1.0);
            double sp = std::pow(2.0, f * 6.0 - 2.0);
            sp = sp < 2.0 ? std::round(sp * 20.0) / 20.0 : std::round(sp * 4.0) / 4.0;
            for (double snap : {0.5, 1.0, 1.5, 2.0, 4.0})
                if (std::abs(sp - snap) < snap * 0.04) sp = snap;
            if (std::abs(sp - st.speed) > 1e-6) { out.setSpeed = true; out.speed = sp; }
            shown = sp;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { out.setSpeed = true; out.speed = 1.0; }
        const float kx = toX(std::clamp(shown, 0.25, 16.0));
        dl->AddRectFilled({kx - 4, ty - 10}, {kx + 4, ty + 10}, TC(255, 255, 255, 250), 4.0f);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%gx", std::round(shown * 100.0) / 100.0);
        const ImVec2 vs = ImGui::CalcTextSize(buf);
        dl->AddText({kx - vs.x * 0.5f, ty - 30}, grey, buf);
        const char* labels[] = {"0.25x", "1x", "4x", "16x"};
        const double at[] = {0.25, 1.0, 4.0, 16.0};
        for (int i = 0; i < 4; ++i) {
            const ImVec2 ls = ImGui::CalcTextSize(labels[i]);
            const float lx = std::clamp(toX(at[i]) - ls.x * 0.5f, x0 - 6, x1 - ls.x + 6);
            dl->AddText({lx, ty + 18}, grey, labels[i]);
        }
        ImGui::SetCursorScreenPos({p.x + inner - fieldW - 18, ty - 16});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8, 7});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, TV(1, 1, 1, 0.07f));
        ImGui::SetNextItemWidth(fieldW);
        double field = shown;
        if (ImGui::InputDouble("##speedv", &field, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            out.setSpeed = true;
            out.speed = std::clamp(field, 0.01, 100.0);
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::SetCursorScreenPos({p.x, p.y + h});
        ImGui::Dummy({inner, 0});
    }
    ImGui::Dummy({0, 14});

    // ---- hardware decoding
    {
        const float h = 52.0f;
        const ImVec2 p = card(inner, h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        iconChip(dl, {p.x + 26, p.y + 26}, white);
        dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 44, p.y + 17}, white, T("Hardware Decoding"));
        if (toggleSwitch("##hwdec", {p.x + inner - 18 - 44, p.y + 14}, st.hwdec)) {
            out.setHwdec = true;
            out.hwdec = !st.hwdec;
        }
        ImGui::SetCursorScreenPos({p.x, p.y + h});
        ImGui::Dummy({inner, 0});
    }
    ImGui::Dummy({0, 14});

    // ---- picture adjustments, -100..100 with a reset button each
    {
        struct Row { const char* label; const char* prop; int value; };
        const Row rows[] = {{T("Brightness"), "brightness", st.brightness}, {T("Contrast"), "contrast", st.contrast},
                            {T("Saturation"), "saturation", st.saturation}, {T("Gamma"), "gamma", st.gamma},
                            {T("Hue"), "hue", st.hue}};
        const float rowH = 34.0f;
        const float h = 14 + rowH * 5 + 8;
        const ImVec2 p = card(inner, h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float labelW = 92.0f;
        const float x0 = p.x + 18 + labelW, x1 = p.x + inner - 52;
        for (int i = 0; i < 5; ++i) {
            const Row& r = rows[i];
            const float cy = p.y + 14 + rowH * i + rowH * 0.5f;
            dl->AddText({p.x + 18, cy - ImGui::GetTextLineHeight() * 0.5f}, white, r.label);
            dl->AddLine({x0, cy}, {x1, cy}, TC(255, 255, 255, 45), 3.0f);
            ImGui::SetCursorScreenPos({x0 - 8, cy - 12});
            ImGui::PushID(r.prop);
            ImGui::InvisibleButton("##pic", {x1 - x0 + 16, 24});
            int v = r.value;
            if (ImGui::IsItemActive()) {
                v = int(std::round((ImGui::GetIO().MousePos.x - x0) / (x1 - x0) * 200.0f - 100.0f));
                v = std::clamp(v, -100, 100);
                if (std::abs(v) <= 2) v = 0;
                if (v != r.value) { out.pictureProperty = r.prop; out.pictureValue = v; }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%d", v);
                if (ImGui::IsMouseDoubleClicked(0)) { out.pictureProperty = r.prop; out.pictureValue = 0; }
            }
            const float kx = x0 + (x1 - x0) * (v + 100) / 200.0f;
            dl->AddCircleFilled({kx, cy}, 8.0f, TC(255, 255, 255, 245), 24);
            // reset button
            const ImVec2 rc{p.x + inner - 26, cy};
            ImGui::SetCursorScreenPos({rc.x - 11, rc.y - 11});
            if (ImGui::InvisibleButton("##reset", {22, 22})) { out.pictureProperty = r.prop; out.pictureValue = 0; }
            const ImU32 rcol = TC(255, 255, 255, ImGui::IsItemHovered() ? 230 : (v != 0 ? 170 : 90));
            dl->AddCircleFilled(rc, 9.0f, TC(255, 255, 255, 25), 20);
            dl->PathArcTo(rc, 5.0f, 0.6f, 5.6f, 14);
            dl->PathStroke(rcol, 0, 1.4f);
            dl->AddTriangleFilled({rc.x + 3.5f, rc.y - 5.5f}, {rc.x + 6.5f, rc.y - 1.0f}, {rc.x + 1.5f, rc.y - 1.0f}, rcol);
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos({p.x, p.y + h});
        ImGui::Dummy({inner, 0});
    }

}

// ------------------------------------------------------------- playlist

namespace {

std::string clockText(double t) {
    if (!(t >= 0)) return "--:--";
    const int s = int(t + 0.5), h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    char b[32];
    if (h > 0) std::snprintf(b, sizeof b, "%d:%02d:%02d", h, m, sec);
    else       std::snprintf(b, sizeof b, "%02d:%02d", m, sec);
    return b;
}

void iconRepeat(ImDrawList* dl, ImVec2 c, ImU32 col, bool one) {
    dl->AddLine({c.x - 7, c.y + 1}, {c.x - 7, c.y - 3}, col, 1.5f);
    dl->AddLine({c.x - 7, c.y - 3}, {c.x + 6, c.y - 3}, col, 1.5f);
    dl->AddTriangleFilled({c.x + 4, c.y - 6}, {c.x + 8, c.y - 3}, {c.x + 4, c.y}, col);
    dl->AddLine({c.x + 7, c.y - 1}, {c.x + 7, c.y + 3}, col, 1.5f);
    dl->AddLine({c.x + 7, c.y + 3}, {c.x - 6, c.y + 3}, col, 1.5f);
    dl->AddTriangleFilled({c.x - 4, c.y}, {c.x - 8, c.y + 3}, {c.x - 4, c.y + 6}, col);
    if (one) dl->AddText(ImGui::GetFont(), 10.0f, {c.x + 6, c.y - 13}, col, "1");
}

void iconShuffle(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddBezierCubic({c.x - 8, c.y - 4}, {c.x - 1, c.y - 4}, {c.x - 1, c.y + 4}, {c.x + 6, c.y + 4}, col, 1.5f);
    dl->AddBezierCubic({c.x - 8, c.y + 4}, {c.x - 1, c.y + 4}, {c.x - 1, c.y - 4}, {c.x + 6, c.y - 4}, col, 1.5f);
    dl->AddTriangleFilled({c.x + 5, c.y - 7}, {c.x + 9, c.y - 4}, {c.x + 5, c.y - 1}, col);
    dl->AddTriangleFilled({c.x + 5, c.y + 1}, {c.x + 9, c.y + 4}, {c.x + 5, c.y + 7}, col);
}

void iconSort(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddLine({c.x - 4, c.y + 7}, {c.x - 4, c.y - 6}, col, 1.5f);
    dl->AddTriangleFilled({c.x - 8, c.y - 3}, {c.x - 4, c.y - 8}, {c.x, c.y - 3}, col);
    dl->AddLine({c.x + 4, c.y - 7}, {c.x + 4, c.y + 6}, col, 1.5f);
    dl->AddTriangleFilled({c.x, c.y + 3}, {c.x + 4, c.y + 8}, {c.x + 8, c.y + 3}, col);
}

void iconTrash(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddLine({c.x - 7, c.y - 5}, {c.x + 7, c.y - 5}, col, 1.5f);
    dl->AddLine({c.x - 2, c.y - 7}, {c.x + 2, c.y - 7}, col, 1.5f);
    dl->AddRect({c.x - 5, c.y - 5}, {c.x + 5, c.y + 7}, col, 1.5f, 0, 1.5f);
    dl->AddLine({c.x - 1.5f, c.y - 2}, {c.x - 1.5f, c.y + 4}, col, 1.2f);
    dl->AddLine({c.x + 1.5f, c.y - 2}, {c.x + 1.5f, c.y + 4}, col, 1.2f);
}

void iconChapters(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddRect({c.x - 8, c.y - 7}, {c.x + 8, c.y + 7}, col, 2.0f, 0, 1.4f);
    for (int i = 0; i < 3; ++i) {
        const float y = c.y - 3.5f + i * 3.5f;
        dl->AddLine({c.x - 5, y}, {c.x - 2, y}, col, 1.2f);
        dl->AddLine({c.x, y}, {c.x + 5, y}, col, 1.2f);
    }
}

// Footer icon button; returns true when clicked.
bool footerButton(const char* id, ImVec2 c, bool active, void (*draw)(ImDrawList*, ImVec2, ImU32)) {
    ImGui::SetCursorScreenPos({c.x - 13, c.y - 13});
    const bool clicked = ImGui::InvisibleButton(id, {26, 26});
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddCircleFilled(c, 13, TC(255, 255, 255, 20), 20);
    draw(dl, c, active ? TC(110, 165, 255, 255) : TC(255, 255, 255, hov ? 235 : 170));
    return clicked;
}

std::string displayName(const PlaylistEntry& e) {
    if (!e.title.empty()) return e.title;
    const size_t slash = e.path.find_last_of("/\\");
    return slash == std::string::npos ? e.path : e.path.substr(slash + 1);
}

}  // namespace

void Overlay::drawPlaylist(const PlayerState& st, const PlaylistView& pl, OverlayActions& out) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float w = panelWidth(size.x);
    const float x = m_leftVisibleW - w;

    ImGui::SetNextWindowPos({x, 0});
    ImGui::SetNextWindowSize({w, size.y});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, TV(0.12f, 0.12f, 0.14f, 0.99f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin("##playlist", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    m_menuOpen = true;
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (hovered) m_hoveringPanel = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine({x + w - 1, 0}, {x + w - 1, size.y}, TC(255, 255, 255, 26));
    const ImU32 white = TC(255, 255, 255, 235);

    // ---- header: [playlist | chapters] tabs, close button on the right
    const float hy = 18.0f + topInset();
    const ImVec2 t0{x + 20, hy};
    dl->AddRectFilled(t0, {t0.x + 96, t0.y + 36}, TC(255, 255, 255, 18), 18.0f);
    for (int i = 0; i < 2; ++i) {
        const ImVec2 a{t0.x + 4 + i * 44.0f, t0.y + 4};
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##pltab", {44, 28})) m_chaptersTab = i == 1;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool sel = (i == 1) == m_chaptersTab;
        if (sel || hov) dl->AddRectFilled(a, {a.x + 44, a.y + 28}, TC(255, 255, 255, sel ? 34 : 16), 14.0f);
        const ImVec2 ic{a.x + 22, a.y + 14};
        if (i == 0) iconPlaylist(dl, ic, 18, white, false);
        else        iconChapters(dl, ic, white);
    }
    const ImVec2 closeC{x + w - 38, hy + 18};
    ImGui::SetCursorScreenPos({closeC.x - 18, closeC.y - 18});
    if (ImGui::InvisibleButton("##plclose", {36, 36})) m_playlistOpen = false;
    dl->AddCircleFilled(closeC, 18, TC(255, 255, 255, ImGui::IsItemHovered() ? 36 : 22), 32);
    dl->AddLine({closeC.x + 3, closeC.y - 6}, {closeC.x - 3, closeC.y}, white, 2.0f);
    dl->AddLine({closeC.x - 3, closeC.y}, {closeC.x + 3, closeC.y + 6}, white, 2.0f);

    const float listTop = hy + 54;
    const float footerH = 52.0f;
    const float listH = size.y - listTop - footerH;
    const float rowH = 54.0f;

    ImGui::SetCursorScreenPos({x, listTop});
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
    ImGui::BeginChild("##pllist", {w - 1, listH}, ImGuiChildFlags_None);
    ImGui::PopStyleVar();
    ImDrawList* ld = ImGui::GetWindowDrawList();
    const float rw = ImGui::GetContentRegionAvail().x;

    if (!m_chaptersTab) {
        const int n = int(st.playlist.size());
        int dropAt = -1;
        for (int i = 0; i < n; ++i) {
            const PlaylistEntry& e = st.playlist[size_t(i)];
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 b{a.x + rw, a.y + rowH};
            ImGui::PushID(i);
            ImGui::InvisibleButton("##row", {rw, rowH});
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) m_playlistSelected = i;
            if (hov && ImGui::IsMouseDoubleClicked(0)) out.playlistPlay = i;
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 6.0f)) m_dragFrom = i;
            ImGui::PopID();
            const bool sel = m_playlistSelected == i;
            if (sel)      ld->AddRectFilled(a, b, TC(34, 196, 186, 70));
            else if (hov) ld->AddRectFilled(a, b, TC(255, 255, 255, 12));
            ld->AddLine({a.x, b.y - 0.5f}, {b.x, b.y - 0.5f}, TC(255, 255, 255, 14));

            const float ty = a.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f - 2;
            if (e.current) iconPlay(ld, {a.x + 22, a.y + rowH * 0.5f - 2}, 11, white);
            const double dur = i < int(pl.durations.size()) ? pl.durations[size_t(i)] : -1;
            const std::string d = dur > 0 ? clockText(dur) : "";
            const ImVec2 ds = ImGui::CalcTextSize(d.c_str());
            ld->AddText({b.x - 16 - ds.x, ty}, TC(255, 255, 255, 130), d.c_str());
            const std::string name = displayName(e);
            ld->PushClipRect({a.x + 40, a.y}, {b.x - 26 - ds.x, b.y}, true);
            ld->AddText({a.x + 40, ty}, TC(255, 255, 255, e.current ? 255 : 220), name.c_str());
            ld->PopClipRect();

            // watched so far (IINA draws it under the row)
            const float prog = i < int(pl.progress.size()) ? pl.progress[size_t(i)] : 0.0f;
            if (prog > 0.0f)
                ld->AddRectFilled({a.x, b.y - 3}, {a.x + rw * std::min(prog, 1.0f), b.y - 1}, TC(34, 196, 186, 255));

            // where a dragged row would land
            if (m_dragFrom >= 0) {
                const float my = ImGui::GetIO().MousePos.y;
                if (my >= a.y && my < b.y) dropAt = my < (a.y + b.y) * 0.5f ? i : i + 1;
            }
        }
        if (m_dragFrom >= 0) {
            const float my = ImGui::GetIO().MousePos.y;
            if (dropAt < 0) dropAt = my < ImGui::GetWindowPos().y + 4 ? 0 : n;
            const float ly = ImGui::GetWindowPos().y + dropAt * rowH - ImGui::GetScrollY();
            ld->AddLine({ImGui::GetWindowPos().x + 8, ly}, {ImGui::GetWindowPos().x + rw - 8, ly},
                        TC(34, 196, 186, 255), 2.0f);
            if (!ImGui::IsMouseDown(0)) {
                if (dropAt != m_dragFrom && dropAt != m_dragFrom + 1) {
                    out.playlistMoveFrom = m_dragFrom;
                    out.playlistMoveTo = dropAt;
                    m_playlistSelected = dropAt > m_dragFrom ? dropAt - 1 : dropAt;
                }
                m_dragFrom = -1;
            }
        }
        if (n == 0) {
            ImGui::SetCursorPos({20, 20});
            ImGui::TextDisabled("%s", T("Drop videos here or press + to add them."));
        }
    } else {
        for (int i = 0; i < int(st.chapters.size()); ++i) {
            const Chapter& c = st.chapters[size_t(i)];
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 b{a.x + rw, a.y + rowH};
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##ch", {rw, rowH})) out.seekChapter = i;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hov) ld->AddRectFilled(a, b, TC(255, 255, 255, 12));
            ld->AddLine({a.x, b.y - 0.5f}, {b.x, b.y - 0.5f}, TC(255, 255, 255, 14));
            const float ty = a.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
            if (i == st.chapter) iconPlay(ld, {a.x + 22, a.y + rowH * 0.5f}, 11, white);
            const std::string t = clockText(c.time);
            const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            ld->AddText({b.x - 16 - ts.x, ty}, TC(255, 255, 255, 130), t.c_str());
            ld->PushClipRect({a.x + 40, a.y}, {b.x - 26 - ts.x, b.y}, true);
            ld->AddText({a.x + 40, ty}, TC(255, 255, 255, 230), c.title.c_str());
            ld->PopClipRect();
        }
        if (st.chapters.empty()) {
            ImGui::SetCursorPos({20, 20});
            ImGui::TextDisabled("%s", T("This video has no chapters."));
        }
    }
    ImGui::EndChild();

    // ---- footer: repeat, shuffle, sort | total | add, remove, clear
    const float fy = size.y - footerH * 0.5f;
    dl->AddLine({x, size.y - footerH}, {x + w, size.y - footerH}, TC(255, 255, 255, 20));
    if (footerButton("##repeat", {x + 30, fy}, st.loopFile || st.loopPlaylist,
                     st.loopFile ? +[](ImDrawList* d, ImVec2 c, ImU32 col) { iconRepeat(d, c, col, true); }
                                 : +[](ImDrawList* d, ImVec2 c, ImU32 col) { iconRepeat(d, c, col, false); }))
        out.cycleRepeat = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", st.loopFile ? T("Repeat: this video") : st.loopPlaylist ? T("Repeat: playlist") : T("Repeat: off"));
    if (footerButton("##shuffle", {x + 62, fy}, st.shuffled, iconShuffle)) out.toggleShuffle = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Shuffle"));
    if (footerButton("##sort", {x + 94, fy}, false, iconSort)) out.playlistSort = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Sort by name"));

    double total = 0;
    for (size_t i = 0; i < st.playlist.size() && i < pl.durations.size(); ++i)
        if (pl.durations[i] > 0) total += pl.durations[i];
    const std::string tt = total > 0 && w >= 360 ? TF("{} in total", {clockText(total)}) : "";
    const ImVec2 ts = ImGui::CalcTextSize(tt.c_str());
    dl->AddText({x + w * 0.5f - ts.x * 0.5f + 20, fy - ts.y * 0.5f}, TC(255, 255, 255, 130), tt.c_str());

    if (footerButton("##add", {x + w - 94, fy}, false, [](ImDrawList* d, ImVec2 c, ImU32 col) {
            d->AddLine({c.x - 7, c.y}, {c.x + 7, c.y}, col, 1.6f);
            d->AddLine({c.x, c.y - 7}, {c.x, c.y + 7}, col, 1.6f);
        }))
        out.playlistAdd = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Add files..."));
    if (footerButton("##remove", {x + w - 62, fy}, false, [](ImDrawList* d, ImVec2 c, ImU32 col) {
            d->AddLine({c.x - 7, c.y}, {c.x + 7, c.y}, col, 1.6f);
        }) && m_playlistSelected >= 0 && m_playlistSelected < int(st.playlist.size())) {
        out.playlistRemove = m_playlistSelected;
        m_playlistSelected = -1;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Remove selected"));
    if (footerButton("##clear", {x + w - 30, fy}, false, iconTrash)) out.playlistClear = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Clear playlist (keeps the playing video)"));

    ImGui::End();
}

void Overlay::openAccountForm(const OnlineView& online) {
    m_resultsOpen = false;
    m_accountOpen = true;
    m_panelOpen = true;
    m_panelTab = PanelTab::Subtitles;
    if (online.settings) {
        std::snprintf(m_keyBuf, sizeof m_keyBuf, "%s", online.settings->apiKey.c_str());
        std::snprintf(m_userBuf, sizeof m_userBuf, "%s", online.settings->username.c_str());
        std::snprintf(m_passBuf, sizeof m_passBuf, "%s", online.settings->password.c_str());
    }
}

namespace {

// "2016-07-09" -> "9 Jul 2016"
std::string prettyDate(const std::string& iso) {
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (iso.size() < 10) return iso;
    const int y = std::atoi(iso.substr(0, 4).c_str()), m = std::atoi(iso.substr(5, 2).c_str()),
              d = std::atoi(iso.substr(8, 2).c_str());
    if (m < 1 || m > 12) return iso;
    // day, month, year; a language can reorder them ("{2}年{1}{0}日")
    return TF("{0} {1} {2}", {std::to_string(d), T(months[m - 1]), std::to_string(y)});
}

// Upper-case text button (dialog footer); returns true when clicked.
bool textButton(const char* label, ImVec2 rightEdge, ImU32 color, bool enabled, float* widthOut) {
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 pos{rightEdge.x - ts.x, rightEdge.y};
    if (widthOut) *widthOut = ts.x;
    ImGui::SetCursorScreenPos({pos.x - 6, pos.y - 6});
    ImGui::PushID(label);
    const bool clicked = ImGui::InvisibleButton("##tb", {ts.x + 12, ts.y + 12}) && enabled;
    const bool hov = ImGui::IsItemHovered() && enabled;
    ImGui::PopID();
    const ImU32 c = enabled ? (hov ? TC(255, 255, 255, 255) : color) : TC(255, 255, 255, 70);
    ImGui::GetWindowDrawList()->AddText(pos, c, label);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

}  // namespace

void Overlay::drawResultsDialog(const OnlineView& online, OverlayActions& out) {
    const OpenSubtitlesStatus* st = online.status;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float areaW = size.x - m_panelVisibleW;  // the video area left of the side panel
    const float w = std::min(760.0f, areaW - 32.0f);
    // keep clear of the control bar at the bottom
    const float free = size.y - controlsHeight() - 24.0f;
    const float h = std::max(260.0f, std::min(600.0f, free - 24.0f));
    const ImVec2 pos{(areaW - w) * 0.5f, std::max(16.0f, (free - h) * 0.5f)};

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize({w, h});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, TV(0.13f, 0.14f, 0.17f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, TV(1, 1, 1, 0.12f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {26, 20});
    ImGui::Begin("##subresults", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    m_menuOpen = true;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) m_hoveringPanel = true;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float inner = w - 52.0f;
    const bool searching = st && st->searching;
    const bool downloading = st && st->downloadingId != 0;
    const bool hasResults = st && !st->results.empty();

    // ---- title + hint
    ImVec2 p = ImGui::GetCursorScreenPos();
    const std::string title = searching ? T("Searching...") : (st && !st->message.empty() && !downloading && hasResults)
                                                              ? st->message
                                                          : (st && !st->results.empty()) ? TF("{} subtitles found.", {std::to_string(st->results.size())})
                                                          : (st ? st->message : std::string());
    dl->AddText(ImGui::GetFont(), 24.0f, p, st && st->error ? TC(255, 140, 128, 255) : TC(255, 255, 255, 245),
                title.c_str());
    if (hasResults)
        dl->AddText(ImGui::GetFont(), 15.0f, {p.x, p.y + 32}, TC(255, 255, 255, 110),
                    downloading ? T("Downloading the selected subtitle...") : T("Please click to select subtitles to download:"));
    ImGui::SetCursorScreenPos({p.x, p.y + 58});

    // ---- refine: title + languages + search (in case the file name guess is off)
    const float searchW = ImGui::CalcTextSize(T("Search")).x + 40;
    const float langW = 110.0f;
    bool go = textField("##q", m_queryBuf, sizeof m_queryBuf, inner - langW - searchW - 20, 0, T("Movie or episode name"));
    ImGui::SameLine(0, 10);
    go |= textField("##l", m_langBuf, sizeof m_langBuf, langW, 0, "tr,en");
    ImGui::SameLine(0, 10);
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(searching || downloading);
    go |= pillButton(T("Search"), sp, 35.0f);
    ImGui::EndDisabled();
    if (go && !searching && !downloading) {
        out.onlineSearch = true;
        out.onlineQuery = m_queryBuf;
        out.onlineLanguages = m_langBuf;
        m_selectedResult = -1;
    }
    ImGui::SetCursorScreenPos({p.x, sp.y + 48});

    // ---- results
    const float footerH = 44.0f;
    const float listH = std::max(80.0f, ImGui::GetContentRegionAvail().y - footerH);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, TV(1, 1, 1, 0.05f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 6});
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
    ImGui::BeginChild("##list", {inner, listH}, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    if (hasResults) {
        ImDrawList* ld = ImGui::GetWindowDrawList();
        const float rw = ImGui::GetContentRegionAvail().x;
        const float rowH = 66.0f;
        for (size_t i = 0; i < st->results.size(); ++i) {
            const OnlineSubtitle& r = st->results[i];
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 b{a.x + rw, a.y + rowH};
            ImGui::PushID(int(i));
            if (ImGui::InvisibleButton("##row", {rw, rowH}) && !downloading) m_selectedResult = int(i);
            const bool hov = ImGui::IsItemHovered();
            if (hov && ImGui::IsMouseDoubleClicked(0) && !downloading) {
                m_selectedResult = int(i);
                out.onlineDownload = int(i);
            }
            ImGui::PopID();
            const bool sel = m_selectedResult == int(i);
            if (sel)      ld->AddRectFilled({a.x + 6, a.y}, {b.x - 6, b.y}, TC(34, 196, 186, 90), 8.0f);
            else if (hov) ld->AddRectFilled({a.x + 6, a.y}, {b.x - 6, b.y}, TC(255, 255, 255, 12), 8.0f);

            // line 1 (small, grey): language (year) fps  [down-arrow] downloads ... date
            float x = a.x + 24;
            const float y1 = a.y + 12;
            const ImU32 grey = TC(255, 255, 255, 150);
            auto small = [&](const std::string& t) {
                ld->AddText(ImGui::GetFont(), 14.0f, {x, y1}, grey, t.c_str());
                x += ImGui::GetFont()->CalcTextSizeA(14.0f, FLT_MAX, 0, t.c_str()).x + 12;
            };
            small(r.language);
            if (r.year > 0) small("(" + std::to_string(r.year) + ")");
            if (r.fps > 0) {
                char f[16];
                std::snprintf(f, sizeof f, "%.2f fps", r.fps);
                small(f);
            }
            // download count with a small arrow icon
            ld->AddLine({x + 4, y1 + 2}, {x + 4, y1 + 12}, grey, 1.6f);
            ld->AddTriangleFilled({x, y1 + 9}, {x + 8, y1 + 9}, {x + 4, y1 + 14}, grey);
            x += 11;
            small(std::to_string(r.downloads));
            if (r.hashMatch) {
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(13.0f, FLT_MAX, 0, T("Exact match"));
                ld->AddRectFilled({x, y1 - 1}, {x + ts.x + 10, y1 + 16}, TC(52, 168, 83, 210), 4.0f);
                ld->AddText(ImGui::GetFont(), 13.0f, {x + 5, y1}, TC(255, 255, 255, 255), T("Exact match"));
                x += ts.x + 18;
            }
            if (r.hearingImpaired) small("HI");
            const std::string date = st->downloadingId == r.fileId ? T("Downloading...") : prettyDate(r.uploadDate);
            const float dw = ImGui::GetFont()->CalcTextSizeA(14.0f, FLT_MAX, 0, date.c_str()).x;
            ld->AddText(ImGui::GetFont(), 14.0f, {b.x - 24 - dw, y1}, TC(255, 255, 255, 110), date.c_str());

            // line 2: release name
            ld->PushClipRect({a.x + 20, a.y}, {b.x - 20, b.y}, true);
            ld->AddText(ImGui::GetFont(), 17.0f, {a.x + 24, a.y + 34}, TC(255, 255, 255, 245), r.release.c_str());
            ld->PopClipRect();
        }
    } else if (!searching && st && !st->message.empty() && !st->error) {
        ImGui::SetCursorPos({24, 16});
        ImGui::TextDisabled("%s", T("Try another title or language."));
    }
    ImGui::EndChild();

    // ---- footer: Account... on the left, CANCEL / DOWNLOAD on the right
    const ImVec2 f{p.x, ImGui::GetCursorScreenPos().y + 14};
    float tw = 0;
    const bool canDownload = m_selectedResult >= 0 && hasResults && !downloading && !searching;
    if (textButton(downloading ? T("DOWNLOADING...") : T("DOWNLOAD"), {f.x + inner, f.y}, TC(110, 165, 255, 255),
                   canDownload, &tw))
        out.onlineDownload = m_selectedResult;
    float cw = 0;
    if (textButton(T("CANCEL"), {f.x + inner - tw - 28, f.y}, TC(255, 255, 255, 190), true, &cw) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape))
        m_resultsOpen = false;
    ImGui::SetCursorScreenPos({f.x - 6, f.y - 6});
    if (ImGui::InvisibleButton("##acct", {ImGui::CalcTextSize(T("Account...")).x + 12, 30})) openAccountForm(online);
    dl->AddText({f.x, f.y}, TC(255, 255, 255, ImGui::IsItemHovered() ? 220 : 120), T("Account..."));
    ImGui::End();
}

float Overlay::panelWidth(float displayW) { return std::min(420.0f, std::max(300.0f, displayW * 0.42f)); }


// The settings that are not about the video playing: the language and the default player.
void Overlay::drawGeneralTab(float inner, OverlayActions& out) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- language: the system's, or one of ours (each in its own name)
    sectionHeader(nullptr, T("Language"), inner, nullptr);
    {
        const auto& langs = i18n::languages();
        const std::string& chosen = i18n::choice();
        std::string current = "English";
        for (const auto& l : langs) if (l.code == i18n::language()) current = l.name;
        std::vector<std::string> labels{chosen.empty() ? TF("System language ({})", {current})
                                                       : std::string(T("System language"))};
        int selected = 0;
        for (size_t i = 0; i < langs.size(); ++i) {
            labels.push_back(langs[i].name);
            if (!chosen.empty() && langs[i].code == chosen) selected = int(i) + 1;
        }
        int picked = -1;
        if (choiceList("##language", labels, selected, inner, picked) && picked != selected)
            i18n::choose(picked == 0 ? std::string() : langs[size_t(picked - 1)].code);
    }
    ImGui::Dummy({0, 18});

    // ---- appearance: dark, light or the system's
    sectionHeader(nullptr, T("Appearance"), inner, nullptr);
    {
        const std::vector<std::string> labels{T("System"), T("Dark"), T("Light")};
        const Theme themes[] = {Theme::System, Theme::Dark, Theme::Light};
        int selected = 0;
        for (int i = 0; i < 3; ++i) if (themes[i] == m_theme) selected = i;
        int picked = -1;
        if (choiceList("##appearance", labels, selected, inner, picked) && picked != selected) setTheme(themes[picked]);
    }
    ImGui::Dummy({0, 18});

    // ---- default player: where double-clicking a video goes
    if (m_defaultAvailable) {
        sectionHeader(nullptr, T("Default Player"), inner, nullptr);
        const std::string text = !m_defaultNote.empty() ? m_defaultNote
                               : m_offerDefault ? std::string(T("Double-clicking a video opens it in another app."))
                                                : std::string(T("Double-clicking a video opens Tolga Player."));
        const float wrap = inner - 36.0f;
        ImFont* font = ImGui::GetFont();
        const ImVec2 ts = font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, wrap, text.c_str());
        const float btnH = 32.0f;
        const float h = 16 + ts.y + (m_offerDefault ? 14 + btnH : 0) + 16;
        const ImVec2 p = card(inner, h);
        dl->AddText(font, ImGui::GetFontSize(), {p.x + 18, p.y + 16}, TC(255, 255, 255, 200), text.c_str(),
                    nullptr, wrap);
        if (m_offerDefault) {
            ImGui::SetCursorScreenPos({p.x + 18, p.y + 16 + ts.y + 14});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {14, 7});
            ImGui::PushStyleColor(ImGuiCol_Button, TV(0.07f, 0.62f, 0.60f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TV(0.11f, 0.70f, 0.67f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, TV(0.05f, 0.52f, 0.51f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));  // white on turquoise in both themes
            if (ImGui::Button(T("Make Default"), {0, btnH})) out.makeDefaultPlayer = true;
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }
        ImGui::SetCursorScreenPos({p.x, p.y + h});
        ImGui::Dummy({inner, 0});
        ImGui::Dummy({0, 24});
    }
}

void Overlay::drawSidePanel(const PlayerState& st, const AirPlayView& airplay, const OnlineView& online,
                            OverlayActions& out) {
    (void)airplay;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float w = panelWidth(size.x);
    const float x = size.x - m_panelVisibleW;

    ImGui::SetNextWindowPos({x, 0});
    ImGui::SetNextWindowSize({w, size.y});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, TV(0.12f, 0.12f, 0.14f, 0.99f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 18 + topInset()});
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
    ImGui::Begin("##sidepanel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    m_menuOpen = true;
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (hovered) m_hoveringPanel = true;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine({x, 0}, {x, size.y}, TC(255, 255, 255, 26));
    const float inner = w - 40.0f;
    const ImVec2 top = ImGui::GetCursorScreenPos();

    // ---- header: close button on the left, tabs on the right
    const ImVec2 closeC{top.x + 18, top.y + 18};
    ImGui::SetCursorScreenPos({closeC.x - 18, closeC.y - 18});
    if (ImGui::InvisibleButton("##close", {36, 36})) m_panelOpen = false;
    dl->AddCircleFilled(closeC, 18, TC(255, 255, 255, ImGui::IsItemHovered() ? 36 : 22), 32);
    dl->AddLine({closeC.x - 3, closeC.y - 6}, {closeC.x + 3, closeC.y}, TC(255, 255, 255, 230), 2.0f);
    dl->AddLine({closeC.x + 3, closeC.y}, {closeC.x - 3, closeC.y + 6}, TC(255, 255, 255, 230), 2.0f);
#ifndef __APPLE__
    // About Tolga Player (macOS has it in the app menu)
    {
        const ImVec2 c{closeC.x + 44, closeC.y};
        ImGui::SetCursorScreenPos({c.x - 18, c.y - 18});
        if (ImGui::InvisibleButton("##about", {36, 36})) openAbout();
        const bool hov = ImGui::IsItemHovered();
        dl->AddCircleFilled(c, 18, TC(255, 255, 255, hov ? 36 : 22), 32);
        dl->AddCircleFilled({c.x, c.y - 6.0f}, 1.8f, TC(255, 255, 255, 230), 12);
        dl->AddLine({c.x, c.y - 1.5f}, {c.x, c.y + 7.0f}, TC(255, 255, 255, 230), 2.2f);
    }
#endif

    struct Tab { PanelTab tab; const char* label; };
    const Tab tabs[] = {{PanelTab::Video, T("Video")}, {PanelTab::Audio, T("Audio")}, {PanelTab::Subtitles, T("Subtitles")},
                         {PanelTab::General, T("General")}};
    // In a narrow panel the tabs give up the selected one's label, then get narrower, so
    // they stay clear of the buttons on the left.
#ifdef __APPLE__
    const float leftEnd = closeC.x + 18 + 10;
#else
    const float leftEnd = closeC.x + 44 + 18 + 10;
#endif
    const float avail = top.x + inner - leftEnd;
    const float selLabelW = ImGui::CalcTextSize(
        std::find_if(std::begin(tabs), std::end(tabs), [&](const Tab& t) { return t.tab == m_panelTab; })->label).x + 8;
    const float n = float(std::size(tabs));
    const bool withLabel = 8 + n * 44 + selLabelW <= avail;
    const float tabW = withLabel || 8 + n * 44 <= avail ? 44.0f : 36.0f;
    const float tabsW = 8 + n * tabW + (withLabel ? selLabelW : 0.0f);
    float tx = top.x + inner - tabsW;
    const ImVec2 t0{tx, top.y}, t1{tx + tabsW, top.y + 36};
    dl->AddRectFilled(t0, t1, TC(255, 255, 255, 18), 18.0f);
    tx += 4;
    for (const Tab& t : tabs) {
        const bool sel = t.tab == m_panelTab;
        const float tw = sel && withLabel ? tabW + selLabelW : tabW;
        ImGui::SetCursorScreenPos({tx, top.y + 4});
        ImGui::PushID(t.label);
        if (ImGui::InvisibleButton("##tab", {tw, 28})) m_panelTab = t.tab;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (sel || hov) dl->AddRectFilled({tx, top.y + 4}, {tx + tw, top.y + 32}, TC(255, 255, 255, sel ? 34 : 16), 14.0f);
        const ImVec2 ic{tx + tabW * 0.5f, top.y + 18};
        const ImU32 c = TC(255, 255, 255, 235);
        if (t.tab == PanelTab::Video)      iconPicture(dl, ic, c);
        else if (t.tab == PanelTab::Audio) iconWaveform(dl, ic, c);
        else if (t.tab == PanelTab::General) iconGear(dl, ic, 15.0f, c, false);
        else                               iconBubble(dl, ic, c);
        if (sel && withLabel) dl->AddText({tx + 38, top.y + 18 - ImGui::GetTextLineHeight() * 0.5f}, c, t.label);
        if (!(sel && withLabel) && hov) ImGui::SetTooltip("%s", t.label);
        tx += tw;
    }
    ImGui::SetCursorScreenPos({top.x, top.y + 58});

    // ---- tracks by type
    std::vector<const Track*> videos, audios, subs;
    int64_t vid = -1, aid = -1, sid = -1;
    for (const Track& t : st.tracks) {
        if (t.type == "video") { videos.push_back(&t); if (t.selected) vid = t.id; }
        if (t.type == "audio") { audios.push_back(&t); if (t.selected) aid = t.id; }
        if (t.type == "sub")   { subs.push_back(&t);   if (t.selected) sid = t.id; }
    }
    int64_t picked = 0;

    // While casting, what the Apple TV can't do is greyed out. Track switching needs the
    // file to be sent as HLS (several tracks or subtitles); delay/equalizer are local only.
    const bool casting = airplay.cast && airplay.cast->active();
    const bool castHls = casting && airplay.cast->hls;
    const std::string tvName = casting ? airplay.cast->deviceName : std::string();
    // Draws `body` and, when `off`, disables it and covers it with a dimmed note.
    auto section = [&](bool off, const char* note, const std::function<void()>& body) {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(off);
        body();
        ImGui::EndDisabled();
        if (!off) return;
        const ImVec2 b{a.x + inner, ImGui::GetCursorScreenPos().y};
        dl->AddRectFilled(a, b, TC(30, 30, 35, 190), 10.0f);
        const std::string text = note;
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, inner - 40);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {a.x + (inner - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f},
                    TC(255, 255, 255, 200), text.c_str(), nullptr, inner - 40);
    };
    const std::string notOnTv = TF("Not available while playing on {}", {tvName});

    if (m_panelTab == PanelTab::General) {
        drawGeneralTab(inner, out);
    } else if (m_panelTab == PanelTab::Subtitles && m_accountOpen) {
        drawAccountForm(online, inner, out);
    } else if (m_panelTab == PanelTab::Subtitles) {
        const bool subsOff = casting && !castHls;  // the TV got the plain file: no subtitles there
        const std::string subsNote = TF("Load a subtitle file or search online to show subtitles on {}", {tvName});
        section(subsOff, subsNote.c_str(), [&] {
            bool visibleOn = st.subVisible;
            if (sectionHeader(nullptr, T("Subtitle Track"), inner, &visibleOn)) {
                out.setSubVisible = true;
                out.subVisible = !st.subVisible;
            }
            if (trackList("##subs", subs, sid, true, inner, picked)) { out.selectSub = true; out.subId = picked; }
        });
        ImGui::Dummy({0, 18});
        section(subsOff, notOnTv.c_str(), [&] {
            double d = 0;
            if (delayCard("subs", st.subDelay, inner, d)) { out.setSubDelay = true; out.subDelay = d; }
        });
        if (castHls) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
            ImGui::TextDisabled("%s", TF("Changes reach {} after a short pause.", {tvName}).c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy({0, 18});
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        if (pillButton(T("Load Subtitle..."), bp)) out.loadSubFile = true;
        const float loadW = ImGui::CalcTextSize(T("Load Subtitle...")).x + 40 + 10;
        if (pillButton(T("Search Online..."), {bp.x + loadW, bp.y})) {
            if (m_queryFor != online.defaultQuery) {
                // new file: start with its title and the saved languages
                m_queryFor = online.defaultQuery;
                std::snprintf(m_queryBuf, sizeof m_queryBuf, "%s", online.defaultQuery.c_str());
            }
            if (online.settings && !m_langBuf[0])
                std::snprintf(m_langBuf, sizeof m_langBuf, "%s", online.settings->languages.c_str());
            if (online.settings && !online.settings->hasApiKey()) {
                openAccountForm(online);
            } else {
                // search right away with the file's title, like IINA, and show the results dialog
                out.onlineSearch = true;
                out.onlineQuery = m_queryBuf;
                out.onlineLanguages = m_langBuf;
                m_resultsOpen = true;
                m_selectedResult = -1;
            }
        }
        ImGui::Dummy({0, 40});
    } else if (m_panelTab == PanelTab::Audio) {
        section(casting && !castHls, notOnTv.c_str(), [&] {
            sectionHeader(nullptr, T("Audio Track"), inner, nullptr);
            if (trackList("##audio", audios, aid, true, inner, picked)) { out.selectAudio = true; out.audioId = picked; }
        });
        ImGui::Dummy({0, 18});
        section(casting, notOnTv.c_str(), [&] {
            double d = 0;
            if (delayCard("audio", st.audioDelay, inner, d)) { out.setAudioDelay = true; out.audioDelay = d; }
        });
        ImGui::Dummy({0, 14});
        section(casting, notOnTv.c_str(), [&] { equalizerCard(inner, out); });
        ImGui::Dummy({0, 24});
    } else {
        section(casting, notOnTv.c_str(), [&] {
            sectionHeader(nullptr, T("Video Track"), inner, nullptr);
            if (trackList("##video", videos, vid, true, inner, picked)) { out.selectVideo = true; out.videoId = picked; }
        });
        ImGui::Dummy({0, 18});
        section(casting, notOnTv.c_str(), [&] { videoCards(st, inner, out); });
        ImGui::Dummy({0, 24});
    }

    ImGui::End();

    // A click on the video (outside the panel and the control bar) closes the panel.
    if (m_panelOpen && !m_resultsOpen && ImGui::IsMouseClicked(0) && !hovered && !m_hoveringPanel &&
        ImGui::GetIO().MousePos.x < x)
        m_panelOpen = false;
}
