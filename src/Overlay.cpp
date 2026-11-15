#include "Overlay.h"
#include "tolga/AirPlayApi.h"
#include "MpvPlayer.h"
#include "OpenSubtitles.h"
#include "Thumbnailer.h"

#include <SDL3/SDL_opengl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr uint64_t kHideAfterMs = 2000;
constexpr float    kFadeSeconds = 0.22f;
constexpr float    kPanelH = 92.0f;
constexpr float    kPanelBottom = 22.0f;

// Colours are always multiplied by the overlay alpha so everything fades together.
ImU32 col(float r, float g, float b, float a, float alpha) {
    return ImGui::GetColorU32(ImVec4(r, g, b, a * alpha));
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
    const ImU32 col = active ? ImGui::GetColorU32(ImVec4(0.35f, 0.62f, 1.0f, 1.0f)) : color;
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
    const ImU32 col = active ? ImGui::GetColorU32(ImVec4(0.35f, 0.62f, 1.0f, 1.0f)) : color;
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
    const ImU32 col = active ? ImGui::GetColorU32(ImVec4(0.35f, 0.62f, 1.0f, 1.0f)) : color;
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

}  // namespace

// ================================================================== setup

bool Overlay::init(SDL_Window* window, SDL_GLContext gl) {
    m_window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the binary

    if (const char* font = findSystemFont())
        io.Fonts->AddFontFromFileTTF(font, 15.0f);
    else
        io.Fonts->AddFontDefault();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = {0, 0};
    style.WindowRounding = 16.0f;
    style.Colors[ImGuiCol_Text] = ImVec4(1, 1, 1, 0.95f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.09f, 0.11f, 0.94f);
    style.Colors[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
    style.Colors[ImGuiCol_Header] = ImVec4(1, 1, 1, 0.08f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.12f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.18f);
    style.Colors[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.10f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(1, 1, 1, 0.45f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1, 1, 1, 0.95f);
    style.PopupRounding = 12.0f;
    style.PopupBorderSize = 1.0f;
    style.ItemSpacing = {8, 4};

    if (!ImGui_ImplSDL3_InitForOpenGL(window, gl)) return false;
#ifdef __APPLE__
    if (!ImGui_ImplOpenGL3_Init("#version 150")) return false;
#else
    if (!ImGui_ImplOpenGL3_Init("#version 330")) return false;
#endif
    m_lastActivity = m_lastFrame = SDL_GetTicks();
    return true;
}

void Overlay::shutdown() {
    if (m_thumbTex) {
        GLuint tex = m_thumbTex;
        glDeleteTextures(1, &tex);
        m_thumbTex = 0;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void Overlay::processEvent(const SDL_Event& e) {
    ImGui_ImplSDL3_ProcessEvent(&e);
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
        else if (m_titlebarOverlay) drawTitleShade();
        drawControls(st, fullscreen, airplay, out);
    } else {
        m_hoveringPanel = false;
        m_menuOpen = false;
    }

    if (m_playlistAnim > 0.0f) drawPlaylist(st, playlist, out);
    if (m_panelAnim > 0.0f && !st.idle) drawSidePanel(st, airplay, online, out);
    if (m_resultsOpen && !st.idle) drawResultsDialog(online, out);

    const bool wantPin = cast && (cast->phase == CastStatus::Phase::NeedPin ||
                                  cast->phase == CastStatus::Phase::Pairing);
    if (wantPin) drawPinDialog(*cast, out);
    else         m_pinOpen = false;
    if (m_licenseOpen && !wantPin) drawLicenseDialog(airplay, out);
    drawToast();
    return out;
}

bool Overlay::isAnimating() const {
    const bool panelMoving = (m_panelOpen ? m_panelAnim < 1.0f : m_panelAnim > 0.0f) ||
                             (m_playlistOpen ? m_playlistAnim < 1.0f : m_playlistAnim > 0.0f);
    return m_alpha != m_targetAlpha || m_pinOpen || m_licenseOpen || panelMoving || SDL_GetTicks() < m_toastUntil;
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
        IM_COL32(24, 26, 34, 255), IM_COL32(24, 26, 34, 255),
        IM_COL32(10, 10, 13, 255), IM_COL32(10, 10, 13, 255));

    const ImVec2 c{size.x * 0.5f, size.y * 0.5f - 46.0f};
    iconAirPlay(bg, c, 76, IM_COL32(255, 255, 255, 215), true);

    using P = CastStatus::Phase;
    const int dots = int(ImGui::GetTime() * 3.0) % 4;
    std::string line;
    switch (cast.phase) {
    case P::Preparing: {
        char pct[16];
        std::snprintf(pct, sizeof pct, " %d%%", int(cast.progress * 100.0));
        line = "Preparing for " + cast.deviceName + "..." + pct;
        break;
    }
    case P::Connecting: line = "Connecting to " + cast.deviceName + std::string(dots, '.'); break;
    case P::NeedPin:
    case P::Pairing:    line = "Pairing with " + cast.deviceName; break;
    case P::Loading:    line = "Loading on " + cast.deviceName + std::string(dots, '.'); break;
    case P::Paused:     line = "Paused on " + cast.deviceName; break;
    case P::Ended:      line = "Finished on " + cast.deviceName; break;
    default:            line = "Playing on " + cast.deviceName; break;
    }
    ImFont* font = ImGui::GetFont();
    // keep the text centred while the dots animate
    const bool animated = cast.phase == P::Connecting || cast.phase == P::Loading;
    const std::string widest = animated ? line.substr(0, line.size() - dots) + "..." : line;
    const float ls = 22.0f;
    const float lw = font->CalcTextSizeA(ls, FLT_MAX, 0, widest.c_str()).x;
    bg->AddText(font, ls, {c.x - lw * 0.5f, c.y + 52}, IM_COL32(255, 255, 255, 235), line.c_str());

    if (!st.title.empty()) {
        const float ts = 15.0f;
        ImVec2 tw = font->CalcTextSizeA(ts, FLT_MAX, 0, st.title.c_str());
        bg->AddText(font, ts, {c.x - tw.x * 0.5f, c.y + 84}, IM_COL32(255, 255, 255, 120), st.title.c_str());
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.45f));
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.12f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.10f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22, 20});
    ImGui::Begin("##pin", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);
    m_hoveringPanel = true;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string title = "Pair with " + cast.deviceName;
    dl->AddText(ImGui::GetFont(), 19.0f, ImGui::GetCursorScreenPos(), IM_COL32(255, 255, 255, 245), title.c_str());
    ImGui::Dummy({0, 28});
    if (cast.pinRejected)
        ImGui::TextColored({1.0f, 0.55f, 0.5f, 1.0f}, "That code didn't work. Enter the new one.");
    else
        ImGui::TextDisabled("Enter the code shown on your TV.");
    ImGui::Dummy({0, 6});

    // big centred digits field
    const float fieldW = 150.0f;
    ImGui::SetCursorPosX((dlg.x - fieldW) * 0.5f);
    ImGui::PushItemWidth(fieldW);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12, 8});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.07f));
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
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.20f));
    if (ImGui::Button("Cancel", bsz) || ImGui::IsKeyPressed(ImGuiKey_Escape)) out.cancelPin = true;
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 10);
    const bool ready = std::strlen(m_pinBuf) == 4 && !checking;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.52f, 0.98f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.58f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.45f, 0.90f, 1.0f));
    ImGui::BeginDisabled(!ready);
    if ((ImGui::Button(checking ? "Checking..." : "Pair", bsz) || enter) && ready) {
        out.submitPin = true;
        out.pin = m_pinBuf;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ------------------------------------------------------------- license

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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.45f));
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.12f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.10f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22, 20});
    ImGui::Begin("##license", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);
    m_hoveringPanel = true;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImGui::GetFont(), 19.0f, ImGui::GetCursorScreenPos(), IM_COL32(255, 255, 255, 245), "Tolga Player License");
    ImGui::Dummy({0, 28});
    const float wrap = dlg.x - 44.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    if (licensed) {
        if (lic->state == S::Licensed) ImGui::Text("Licensed to %s. Thank you for supporting Tolga!", lic->email.c_str());
        else ImGui::TextUnformatted("This is a community build of Tolga. Everything is unlocked.");
    } else if (lic->state == S::Trial) {
        ImGui::Text("AirPlay is free to try for %d more day%s. A license unlocks it for good; "
                    "playing on this computer is always free.", lic->trialDaysLeft, lic->trialDaysLeft == 1 ? "" : "s");
    } else {
        ImGui::TextUnformatted("The AirPlay trial has ended. Playing on this computer stays free; "
                               "a license unlocks AirPlay again.");
    }
    ImGui::PopTextWrapPos();

    bool enter = false;
    if (!licensed) {
        ImGui::Dummy({0, 8});
        ImGui::PushItemWidth(wrap);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 7});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.07f));
        if (first) ImGui::SetKeyboardFocusHere();
        enter = ImGui::InputTextWithHint("##licensekey", "Paste your license key (TOLGA1-...)", m_licenseBuf,
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
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.20f));
    if (!licensed && !lic->buyUrl.empty()) {
        if (ImGui::Button("Buy License", bsz)) out.openBuyPage = true;
        ImGui::SameLine(0, 10);
    }
    if (ImGui::Button(licensed ? "Close" : "Not Now", bsz) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        out.closeLicense = true;
    ImGui::PopStyleColor(3);
    if (!licensed) {
        ImGui::SameLine(0, 10);
        const bool ready = m_licenseBuf[0] != 0;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.52f, 0.98f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.58f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.45f, 0.90f, 1.0f));
        ImGui::BeginDisabled(!ready);
        if ((ImGui::Button("Activate", bsz) || enter) && ready) {
            out.activateLicense = true;
            out.licenseKey = m_licenseBuf;
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
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
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ------------------------------------------------------------- idle screen

void Overlay::drawIdleScreen(OverlayActions& out) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();

    // soft vertical gradient instead of flat black
    bg->AddRectFilledMultiColor({0, 0}, size,
        IM_COL32(28, 29, 36, 255), IM_COL32(28, 29, 36, 255),
        IM_COL32(12, 12, 15, 255), IM_COL32(12, 12, 15, 255));

    ImFont* font = ImGui::GetFont();
    const ImVec2 c{size.x * 0.5f, size.y * 0.5f};

    const char* title = "Tolga Player";
    const float titleSize = 40.0f;
    ImVec2 ts = font->CalcTextSizeA(titleSize, FLT_MAX, 0, title);
    bg->AddText(font, titleSize, {c.x - ts.x * 0.5f, c.y - 70}, IM_COL32(255, 255, 255, 235), title);

    const char* hint = "Drop a video here, or";
    ImVec2 hs = font->CalcTextSizeA(15.0f, FLT_MAX, 0, hint);
    bg->AddText(font, 15.0f, {c.x - hs.x * 0.5f, c.y - 12}, IM_COL32(255, 255, 255, 120), hint);

    // "Open File…" pill button
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##idle", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    const char* label = "Open File...";
    ImVec2 ls = ImGui::CalcTextSize(label);
    ImVec2 bsize{ls.x + 40, 36};
    ImVec2 bpos{c.x - bsize.x * 0.5f, c.y + 22};
    ImGui::SetCursorScreenPos(bpos);
    if (ImGui::InvisibleButton("##open", bsize)) out.openFile = true;
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bpos, {bpos.x + bsize.x, bpos.y + bsize.y},
                      hov ? IM_COL32(255, 255, 255, 46) : IM_COL32(255, 255, 255, 26), bsize.y * 0.5f);
    dl->AddText({bpos.x + 20, bpos.y + (bsize.y - ls.y) * 0.5f}, IM_COL32(255, 255, 255, 235), label);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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

// ------------------------------------------------------------- control bar

void Overlay::drawControls(const PlayerState& st, bool fullscreen, const AirPlayView& airplay, OverlayActions& out) {
    ImVec2 size = ImGui::GetIO().DisplaySize;
    size.x -= m_panelVisibleW + m_leftVisibleW;  // keep the bar between the playlist and the side panel
    const float a = m_alpha;

    const float panelW = std::clamp(size.x - 48.0f, 340.0f, 760.0f);
    const float panelH = kPanelH;
    const ImVec2 p0{m_leftVisibleW + (size.x - panelW) * 0.5f, size.y - panelH - kPanelBottom};
    const ImVec2 p1{p0.x + panelW, p0.y + panelH};

    ImGui::SetNextWindowPos(p0);
    ImGui::SetNextWindowSize({panelW, panelH});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.09f, 0.72f * a));
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
    const bool compact = panelW < 600.0f;  // no volume slider
    const ImU32 white = col(1, 1, 1, 0.95f, a);
    const ImU32 dimmed = col(1, 1, 1, cast ? 0.30f : 0.95f, a);

    // ---- right cluster (from the right edge): settings, fullscreen, AirPlay
    const float step = 38.0f;
    const ImVec2 gearC{p1.x - pad - 10, row1};
    if (iconButton("##settings", gearC, 15, a)) togglePanel(m_panelTab);
    iconGear(dl, gearC, 18, white, m_panelOpen);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Settings");

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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Playlist");

    if (!tiny) {
        const ImVec2 spk{listC.x + step, row1};
        ImGui::BeginDisabled(cast != nullptr);  // the TV's own remote sets the volume
        if (iconButton("##mute", spk, 15, a)) out.toggleMute = true;
        const float volX = spk.x + 22;
        const float volW = std::min(110.0f, mid.x - 56 - 30 - volX);
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
                                col(1, 1, 1, 1, a), 6.0f);
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
        ImGui::MenuItem("This Computer", nullptr, true);
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 250);
        ImGui::TextDisabled("Playing on an Apple TV is part of the official Tolga Player app.");
        ImGui::PopTextWrapPos();
        if (ImGui::MenuItem("Get Tolga Player...")) out.openBuyPage = true;
        ImGui::EndPopup();
        ImGui::PopStyleVar();
        return;
    }
    if (ImGui::MenuItem("This Computer", nullptr, airplay.castTarget.empty())) {
        out.selectCastTarget = true;
        out.castTarget.clear();
    }

    const size_t count = airplay.devices ? airplay.devices->size() : 0;
    for (size_t i = 0; i < count; ++i) {
        const AirPlayTarget& d = (*airplay.devices)[i];
        ImGui::PushID(int(i));
        const bool selected = !d.deviceId.empty() && d.deviceId == airplay.castTarget;
        const bool video = d.video;
        // name on the left, model in grey on the right
        if (ImGui::MenuItem(d.name.c_str(), d.model.c_str(), selected, video)) {
            out.selectCastTarget = true;
            out.castTarget = d.deviceId;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s%s", d.details.c_str(), video ? "" : "\nAudio only - can't play video");
        }
        ImGui::PopID();
    }

    if (airplay.searching || count == 0) {
        // animated dots while the network is being scanned
        const int dots = int(ImGui::GetTime() * 3.0) % 4;
        std::string text = airplay.searching ? "Searching" + std::string(dots, '.') : "No AirPlay devices found";
        ImGui::Spacing();
        ImGui::TextDisabled("%s", text.c_str());
    }

    // official builds: trial / license
    if (const LicenseInfo* lic = airplay.license) {
        using S = LicenseInfo::State;
        if (lic->state == S::Trial || lic->state == S::Expired) {
            ImGui::Separator();
            if (lic->state == S::Trial)
                ImGui::TextDisabled("Free trial: %d day%s left", lic->trialDaysLeft, lic->trialDaysLeft == 1 ? "" : "s");
            else
                ImGui::TextDisabled("The AirPlay trial has ended");
            if (ImGui::MenuItem(lic->state == S::Trial ? "Enter License..." : "Unlock AirPlay..."))
                out.openLicense = true;
        }
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

// ================================================================== side panel

namespace {

constexpr ImU32 kAccent = IM_COL32(64, 132, 255, 255);

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
        if (l == k) return n;
    if (l.empty() || l == "und") return {};
    return code;
}

std::string fileName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "No Title (mov_text, Default)" / "Turkish.srt (subrip, External)" / "English (aac, 5.1, Default)"
std::string trackText(const Track& t) {
    std::string name = !t.title.empty() ? t.title : t.external ? fileName(t.externalFile) : "No Title";
    std::vector<std::string> info;
    if (!t.codec.empty()) info.push_back(t.codec);
    if (t.type == "audio" && t.channels > 0)
        info.push_back(t.channels == 6 ? "5.1" : t.channels == 8 ? "7.1" : t.channels == 2 ? "Stereo"
                                                                    : t.channels == 1 ? "Mono"
                                                                    : std::to_string(t.channels) + " ch");
    if (t.type == "video" && t.width > 0) info.push_back(std::to_string(t.width) + "x" + std::to_string(t.height));
    if (t.type == "video" && t.fps > 0) {
        char f[24];
        std::snprintf(f, sizeof f, "%.3gfps", t.fps);
        info.push_back(f);
    }
    if (t.isDefault) info.push_back("Default");
    if (t.forced) info.push_back("Forced");
    if (t.external) info.push_back("External");
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
    dl->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, on ? kAccent : IM_COL32(255, 255, 255, 45), size.y * 0.5f);
    const float kx = on ? pos.x + size.x - size.y * 0.5f : pos.x + size.y * 0.5f;
    dl->AddCircleFilled({kx, pos.y + size.y * 0.5f}, size.y * 0.5f - 3, IM_COL32(255, 255, 255, 250), 24);
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
    dl->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, IM_COL32(255, 255, 255, hov ? 40 : 26), 8.0f);
    dl->AddText({pos.x + 20, pos.y + (size.y - ts.y) * 0.5f}, IM_COL32(255, 255, 255, 240), label);
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
    const ImU32 hole = IM_COL32(40, 40, 46, 255);
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
    dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 10), 10.0f);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 22), 10.0f);

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
        if (hov) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 14), 7.0f);
        if (r > 0) dl->AddLine({a.x + 10, a.y}, {b.x - 10, a.y}, IM_COL32(255, 255, 255, 18));
        if (tid == selected) dl->AddRectFilled({a.x + 8, a.y + 9}, {a.x + 11, b.y - 9}, kAccent, 1.5f);

        const float ty = a.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        float right = b.x - 10;
        if (t) {
            const std::string lang = languageName(t->lang);
            if (!lang.empty()) {
                const ImVec2 ls = ImGui::CalcTextSize(lang.c_str());
                const ImVec2 ba{right - ls.x - 12, a.y + 8}, bb{right, b.y - 8};
                dl->AddRectFilled(ba, bb, IM_COL32(255, 255, 255, 34), 5.0f);
                dl->AddText({ba.x + 6, ty}, IM_COL32(255, 255, 255, 220), lang.c_str());
                right = ba.x - 8;
            }
            const std::string text = trackText(*t);
            dl->PushClipRect({a.x + 20, a.y}, {right, b.y}, true);
            dl->AddText({a.x + 22, ty}, IM_COL32(255, 255, 255, 240), text.c_str());
            dl->PopClipRect();
        } else {
            dl->AddText({a.x + 22, ty}, IM_COL32(255, 255, 255, 110), "<None>");
        }
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
        dl->AddRectFilled({x, p.y + 4}, {x + 18, p.y + 22}, IM_COL32(255, 255, 255, 60), 4.0f);
        const ImVec2 ns = ImGui::CalcTextSize(number);
        dl->AddText({x + 9 - ns.x * 0.5f, p.y + 13 - ns.y * 0.5f}, IM_COL32(20, 20, 24, 255), number);
        x += 28;
    }
    dl->AddText(ImGui::GetFont(), 17.0f, {x, p.y + 3}, IM_COL32(255, 255, 255, 245), title);
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
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1, 1, 1, 0.12f));
    ImGui::SetNextItemWidth(width);
    const bool enter = hint ? ImGui::InputTextWithHint(id, hint, buf, size, flags | ImGuiInputTextFlags_EnterReturnsTrue)
                            : ImGui::InputText(id, buf, size, flags | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    return enter;
}

void label(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.55f));
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
    dl->AddLine({c.x + 4, c.y - 7}, {c.x - 3, c.y}, IM_COL32(255, 255, 255, 230), 2.0f);
    dl->AddLine({c.x - 3, c.y}, {c.x + 4, c.y + 7}, IM_COL32(255, 255, 255, 230), 2.0f);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dl->AddText(ImGui::GetFont(), 17.0f, {p.x + 30, p.y + 4}, IM_COL32(255, 255, 255, 245), title);
    ImGui::SetCursorScreenPos({p.x, p.y + 40});
    return back;
}

}  // namespace

void Overlay::drawAccountForm(const OnlineView& online, float inner, OverlayActions& out) {
    // With a key built into the app only the (optional) login is shown here.
    const bool builtIn = online.settings && online.settings->builtInKey;
    if (backHeader("OpenSubtitles Account")) m_accountOpen = false;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    if (builtIn)
        label("Optional: log in with your opensubtitles.com account to raise the daily download limit.");
    else
        label("Searching needs a free API key. Create one at opensubtitles.com/en/consumers "
              "and paste it below. An account is optional; logging in raises the daily download limit.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 10});

    if (!builtIn) {
        label("API Key");
        textField("##key", m_keyBuf, sizeof m_keyBuf, inner);
        ImGui::Dummy({0, 6});
    }
    label(builtIn ? "User Name" : "User Name (optional)");
    textField("##user", m_userBuf, sizeof m_userBuf, inner);
    ImGui::Dummy({0, 6});
    label(builtIn ? "Password" : "Password (optional)");
    const bool enter = textField("##pass", m_passBuf, sizeof m_passBuf, inner, ImGuiInputTextFlags_Password);
    ImGui::Dummy({0, 14});

    const ImVec2 bp = ImGui::GetCursorScreenPos();
    if (pillButton("Save", bp) || enter) {
        out.onlineSaveSettings = true;
        out.onlineApiKey = m_keyBuf;
        out.onlineUser = m_userBuf;
        out.onlinePassword = m_passBuf;
        m_accountOpen = false;
    }
    ImGui::Dummy({0, 40});
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    ImGui::TextDisabled("Stored on this computer, readable only by your user account.");
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
    dl->AddRectFilled(p, {p.x + width, p.y + height}, IM_COL32(255, 255, 255, 10), 10.0f);
    dl->AddRect(p, {p.x + width, p.y + height}, IM_COL32(255, 255, 255, 22), 10.0f);
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
    const ImU32 white = IM_COL32(255, 255, 255, 240), grey = IM_COL32(255, 255, 255, 140);
    iconClock(dl, {p.x + 24, p.y + 24}, white);
    dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 42, p.y + 15}, white, "Delay");

    bool edited = false;
    const float fieldW = 72.0f;
    const float x0 = p.x + 22, x1 = p.x + inner - fieldW - 34;
    const float ty = p.y + 70;
    const float range = 5.0f;
    // ticks every 0.5 s
    for (int i = 0; i <= 20; ++i) {
        const float x = x0 + (x1 - x0) * i / 20.0f;
        dl->AddCircleFilled({x, ty + 9}, i % 10 == 0 ? 1.4f : 0.9f, IM_COL32(255, 255, 255, 70));
    }
    dl->AddLine({x0, ty}, {x1, ty}, IM_COL32(255, 255, 255, 45), 3.0f);

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
    dl->AddRectFilled({kx - 4, ty - 10}, {kx + 4, ty + 10}, IM_COL32(255, 255, 255, 250), 4.0f);

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
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.07f));
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
    const ImU32 white = IM_COL32(255, 255, 255, 240), grey = IM_COL32(255, 255, 255, 140);
    iconSliders(dl, {p.x + 24, p.y + 24}, white);
    dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 42, p.y + 15}, white, "Equalizer");

    bool changed = false;
    // ---- preset menu
    ImGui::SetCursorScreenPos({p.x + 18, p.y + 46});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 7});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6, 6});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.09f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 0.13f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.0f));
    ImGui::SetNextItemWidth(inner - 36 - 70);
    if (ImGui::BeginCombo("##preset", kEqPresets[m_eqPreset].name, ImGuiComboFlags_HeightLargest)) {
        m_menuOpen = true;
        for (int i = 1; i < int(std::size(kEqPresets)); ++i) {
            if (ImGui::Selectable(kEqPresets[i].name, i == m_eqPreset)) {
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
    const ImVec2 rs = ImGui::CalcTextSize("Reset");
    dl->AddText({p.x + inner - 18 - 30 - rs.x * 0.5f, p.y + 46 + (31 - rs.y) * 0.5f},
                IM_COL32(255, 255, 255, rh ? 230 : 140), "Reset");

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
            dl->AddCircleFilled({x + 7, top + (bottom - top) * t / 8.0f}, 0.9f, IM_COL32(255, 255, 255, 70));
        dl->AddLine({x, top}, {x, bottom}, IM_COL32(255, 255, 255, 45), 3.0f);
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
        dl->AddRectFilled({x - 9, ky - 4}, {x + 9, ky + 4}, IM_COL32(255, 255, 255, 250), 4.0f);
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
        widths.push_back(ImGui::CalcTextSize(labels[i]).x + padX * 2);
        total += widths.back();
    }
    const float scale = total > maxW ? maxW / total : 1.0f;  // squeeze when the panel is narrow
    dl->AddRectFilled(pos, {pos.x + total * scale, pos.y + h}, IM_COL32(255, 255, 255, 22), 7.0f);
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
        if (i == selected)  dl->AddRectFilled({x, pos.y}, {x + w, pos.y + h}, IM_COL32(64, 132, 255, 255), 7.0f);
        else if (hov)       dl->AddRectFilled({x, pos.y}, {x + w, pos.y + h}, IM_COL32(255, 255, 255, 18), 7.0f);
        if (i > 0 && i != selected && i - 1 != selected)
            dl->AddLine({x, pos.y + 8}, {x, pos.y + h - 8}, IM_COL32(255, 255, 255, 50));
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText({x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f}, IM_COL32(255, 255, 255, 240), labels[i]);
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
    const ImU32 white = IM_COL32(255, 255, 255, 240), grey = IM_COL32(255, 255, 255, 140);

    // ---- aspect / crop / rotation
    {
        const float h = 236.0f;
        const ImVec2 p = card(inner, h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float segW = inner - 36;
        iconAspect(dl, {p.x + 26, p.y + 24}, white);
        dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 44, p.y + 15}, white, "Aspect Ratio");
        int c = segmented("aspect", kAspects, 6, m_aspectIdx, {p.x + 18, p.y + 46}, segW);
        if (c >= 0) { m_aspectIdx = c; out.setAspect = true; out.aspect = c == 0 ? "" : kAspects[c]; }

        iconCrop(dl, {p.x + 26, p.y + 100}, white);
        dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 44, p.y + 91}, white, "Crop");
        c = segmented("crop", kCrops, 6, m_cropIdx, {p.x + 18, p.y + 122}, segW);
        if (c >= 0) { m_cropIdx = c; out.setCrop = true; out.crop = c == 0 ? "" : kCrops[c]; }

        iconRotate(dl, {p.x + 26, p.y + 176}, white);
        dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 44, p.y + 167}, white, "Rotation");
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
        dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 44, p.y + 15}, white, "Speed");
        const float fieldW = 72.0f;
        const float x0 = p.x + 22, x1 = p.x + inner - fieldW - 34;
        const float ty = p.y + 70;
        auto toX = [&](double sp) { return x0 + (x1 - x0) * float((std::log2(sp) + 2.0) / 6.0); };
        for (int i = 0; i <= 24; ++i)
            dl->AddCircleFilled({x0 + (x1 - x0) * i / 24.0f, ty + 9}, i % 8 == 0 ? 1.4f : 0.9f, IM_COL32(255, 255, 255, 70));
        dl->AddLine({x0, ty}, {x1, ty}, IM_COL32(255, 255, 255, 45), 3.0f);
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
        dl->AddRectFilled({kx - 4, ty - 10}, {kx + 4, ty + 10}, IM_COL32(255, 255, 255, 250), 4.0f);
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
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.07f));
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
        dl->AddText(ImGui::GetFont(), 16.0f, {p.x + 44, p.y + 17}, white, "Hardware Decoding");
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
        const Row rows[] = {{"Brightness", "brightness", st.brightness}, {"Contrast", "contrast", st.contrast},
                            {"Saturation", "saturation", st.saturation}, {"Gamma", "gamma", st.gamma},
                            {"Hue", "hue", st.hue}};
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
            dl->AddLine({x0, cy}, {x1, cy}, IM_COL32(255, 255, 255, 45), 3.0f);
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
            dl->AddCircleFilled({kx, cy}, 8.0f, IM_COL32(255, 255, 255, 245), 24);
            // reset button
            const ImVec2 rc{p.x + inner - 26, cy};
            ImGui::SetCursorScreenPos({rc.x - 11, rc.y - 11});
            if (ImGui::InvisibleButton("##reset", {22, 22})) { out.pictureProperty = r.prop; out.pictureValue = 0; }
            const ImU32 rcol = IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 230 : (v != 0 ? 170 : 90));
            dl->AddCircleFilled(rc, 9.0f, IM_COL32(255, 255, 255, 25), 20);
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
    if (hov) dl->AddCircleFilled(c, 13, IM_COL32(255, 255, 255, 20), 20);
    draw(dl, c, active ? IM_COL32(110, 165, 255, 255) : IM_COL32(255, 255, 255, hov ? 235 : 170));
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 0.99f));
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
    dl->AddLine({x + w - 1, 0}, {x + w - 1, size.y}, IM_COL32(255, 255, 255, 26));
    const ImU32 white = IM_COL32(255, 255, 255, 235);

    // ---- header: [playlist | chapters] tabs, close button on the right
    const float hy = 18.0f + topInset();
    const ImVec2 t0{x + 20, hy};
    dl->AddRectFilled(t0, {t0.x + 96, t0.y + 36}, IM_COL32(255, 255, 255, 18), 18.0f);
    for (int i = 0; i < 2; ++i) {
        const ImVec2 a{t0.x + 4 + i * 44.0f, t0.y + 4};
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##pltab", {44, 28})) m_chaptersTab = i == 1;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool sel = (i == 1) == m_chaptersTab;
        if (sel || hov) dl->AddRectFilled(a, {a.x + 44, a.y + 28}, IM_COL32(255, 255, 255, sel ? 34 : 16), 14.0f);
        const ImVec2 ic{a.x + 22, a.y + 14};
        if (i == 0) iconPlaylist(dl, ic, 18, white, false);
        else        iconChapters(dl, ic, white);
    }
    const ImVec2 closeC{x + w - 38, hy + 18};
    ImGui::SetCursorScreenPos({closeC.x - 18, closeC.y - 18});
    if (ImGui::InvisibleButton("##plclose", {36, 36})) m_playlistOpen = false;
    dl->AddCircleFilled(closeC, 18, IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 36 : 22), 32);
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
            if (sel)      ld->AddRectFilled(a, b, IM_COL32(64, 132, 255, 70));
            else if (hov) ld->AddRectFilled(a, b, IM_COL32(255, 255, 255, 12));
            ld->AddLine({a.x, b.y - 0.5f}, {b.x, b.y - 0.5f}, IM_COL32(255, 255, 255, 14));

            const float ty = a.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f - 2;
            if (e.current) iconPlay(ld, {a.x + 22, a.y + rowH * 0.5f - 2}, 11, white);
            const double dur = i < int(pl.durations.size()) ? pl.durations[size_t(i)] : -1;
            const std::string d = dur > 0 ? clockText(dur) : "";
            const ImVec2 ds = ImGui::CalcTextSize(d.c_str());
            ld->AddText({b.x - 16 - ds.x, ty}, IM_COL32(255, 255, 255, 130), d.c_str());
            const std::string name = displayName(e);
            ld->PushClipRect({a.x + 40, a.y}, {b.x - 26 - ds.x, b.y}, true);
            ld->AddText({a.x + 40, ty}, IM_COL32(255, 255, 255, e.current ? 255 : 220), name.c_str());
            ld->PopClipRect();

            // watched so far (IINA draws it under the row)
            const float prog = i < int(pl.progress.size()) ? pl.progress[size_t(i)] : 0.0f;
            if (prog > 0.0f)
                ld->AddRectFilled({a.x, b.y - 3}, {a.x + rw * std::min(prog, 1.0f), b.y - 1}, IM_COL32(64, 132, 255, 255));

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
                        IM_COL32(64, 132, 255, 255), 2.0f);
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
            ImGui::TextDisabled("Drop videos here or press + to add them.");
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
            if (hov) ld->AddRectFilled(a, b, IM_COL32(255, 255, 255, 12));
            ld->AddLine({a.x, b.y - 0.5f}, {b.x, b.y - 0.5f}, IM_COL32(255, 255, 255, 14));
            const float ty = a.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
            if (i == st.chapter) iconPlay(ld, {a.x + 22, a.y + rowH * 0.5f}, 11, white);
            const std::string t = clockText(c.time);
            const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            ld->AddText({b.x - 16 - ts.x, ty}, IM_COL32(255, 255, 255, 130), t.c_str());
            ld->PushClipRect({a.x + 40, a.y}, {b.x - 26 - ts.x, b.y}, true);
            ld->AddText({a.x + 40, ty}, IM_COL32(255, 255, 255, 230), c.title.c_str());
            ld->PopClipRect();
        }
        if (st.chapters.empty()) {
            ImGui::SetCursorPos({20, 20});
            ImGui::TextDisabled("This video has no chapters.");
        }
    }
    ImGui::EndChild();

    // ---- footer: repeat, shuffle, sort | total | add, remove, clear
    const float fy = size.y - footerH * 0.5f;
    dl->AddLine({x, size.y - footerH}, {x + w, size.y - footerH}, IM_COL32(255, 255, 255, 20));
    if (footerButton("##repeat", {x + 30, fy}, st.loopFile || st.loopPlaylist,
                     st.loopFile ? +[](ImDrawList* d, ImVec2 c, ImU32 col) { iconRepeat(d, c, col, true); }
                                 : +[](ImDrawList* d, ImVec2 c, ImU32 col) { iconRepeat(d, c, col, false); }))
        out.cycleRepeat = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", st.loopFile ? "Repeat: this video" : st.loopPlaylist ? "Repeat: playlist" : "Repeat: off");
    if (footerButton("##shuffle", {x + 62, fy}, st.shuffled, iconShuffle)) out.toggleShuffle = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shuffle");
    if (footerButton("##sort", {x + 94, fy}, false, iconSort)) out.playlistSort = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by name");

    double total = 0;
    for (size_t i = 0; i < st.playlist.size() && i < pl.durations.size(); ++i)
        if (pl.durations[i] > 0) total += pl.durations[i];
    const std::string tt = total > 0 && w >= 360 ? clockText(total) + " in total" : "";
    const ImVec2 ts = ImGui::CalcTextSize(tt.c_str());
    dl->AddText({x + w * 0.5f - ts.x * 0.5f + 20, fy - ts.y * 0.5f}, IM_COL32(255, 255, 255, 130), tt.c_str());

    if (footerButton("##add", {x + w - 94, fy}, false, [](ImDrawList* d, ImVec2 c, ImU32 col) {
            d->AddLine({c.x - 7, c.y}, {c.x + 7, c.y}, col, 1.6f);
            d->AddLine({c.x, c.y - 7}, {c.x, c.y + 7}, col, 1.6f);
        }))
        out.playlistAdd = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add files...");
    if (footerButton("##remove", {x + w - 62, fy}, false, [](ImDrawList* d, ImVec2 c, ImU32 col) {
            d->AddLine({c.x - 7, c.y}, {c.x + 7, c.y}, col, 1.6f);
        }) && m_playlistSelected >= 0 && m_playlistSelected < int(st.playlist.size())) {
        out.playlistRemove = m_playlistSelected;
        m_playlistSelected = -1;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove selected");
    if (footerButton("##clear", {x + w - 30, fy}, false, iconTrash)) out.playlistClear = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear playlist (keeps the playing video)");

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
    return std::to_string(d) + " " + months[m - 1] + " " + std::to_string(y);
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
    const ImU32 c = enabled ? (hov ? IM_COL32(255, 255, 255, 255) : color) : IM_COL32(255, 255, 255, 70);
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.14f, 0.17f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.12f));
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
    const std::string title = searching ? "Searching..." : (st && !st->message.empty() && !downloading && hasResults)
                                                              ? st->message
                                                          : (st && !st->results.empty()) ? std::to_string(st->results.size()) + " subtitles found."
                                                          : (st ? st->message : std::string());
    dl->AddText(ImGui::GetFont(), 24.0f, p, st && st->error ? IM_COL32(255, 140, 128, 255) : IM_COL32(255, 255, 255, 245),
                title.c_str());
    if (hasResults)
        dl->AddText(ImGui::GetFont(), 15.0f, {p.x, p.y + 32}, IM_COL32(255, 255, 255, 110),
                    downloading ? "Downloading the selected subtitle..." : "Please click to select subtitles to download:");
    ImGui::SetCursorScreenPos({p.x, p.y + 58});

    // ---- refine: title + languages + search (in case the file name guess is off)
    const float searchW = ImGui::CalcTextSize("Search").x + 40;
    const float langW = 110.0f;
    bool go = textField("##q", m_queryBuf, sizeof m_queryBuf, inner - langW - searchW - 20, 0, "Movie or episode name");
    ImGui::SameLine(0, 10);
    go |= textField("##l", m_langBuf, sizeof m_langBuf, langW, 0, "tr,en");
    ImGui::SameLine(0, 10);
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(searching || downloading);
    go |= pillButton("Search", sp, 35.0f);
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
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.05f));
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
            if (sel)      ld->AddRectFilled({a.x + 6, a.y}, {b.x - 6, b.y}, IM_COL32(64, 132, 255, 90), 8.0f);
            else if (hov) ld->AddRectFilled({a.x + 6, a.y}, {b.x - 6, b.y}, IM_COL32(255, 255, 255, 12), 8.0f);

            // line 1 (small, grey): language (year) fps  [down-arrow] downloads ... date
            float x = a.x + 24;
            const float y1 = a.y + 12;
            const ImU32 grey = IM_COL32(255, 255, 255, 150);
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
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(13.0f, FLT_MAX, 0, "Exact match");
                ld->AddRectFilled({x, y1 - 1}, {x + ts.x + 10, y1 + 16}, IM_COL32(52, 168, 83, 210), 4.0f);
                ld->AddText(ImGui::GetFont(), 13.0f, {x + 5, y1}, IM_COL32(255, 255, 255, 255), "Exact match");
                x += ts.x + 18;
            }
            if (r.hearingImpaired) small("HI");
            const std::string date = st->downloadingId == r.fileId ? "Downloading..." : prettyDate(r.uploadDate);
            const float dw = ImGui::GetFont()->CalcTextSizeA(14.0f, FLT_MAX, 0, date.c_str()).x;
            ld->AddText(ImGui::GetFont(), 14.0f, {b.x - 24 - dw, y1}, IM_COL32(255, 255, 255, 110), date.c_str());

            // line 2: release name
            ld->PushClipRect({a.x + 20, a.y}, {b.x - 20, b.y}, true);
            ld->AddText(ImGui::GetFont(), 17.0f, {a.x + 24, a.y + 34}, IM_COL32(255, 255, 255, 245), r.release.c_str());
            ld->PopClipRect();
        }
    } else if (!searching && st && !st->message.empty() && !st->error) {
        ImGui::SetCursorPos({24, 16});
        ImGui::TextDisabled("Try another title or language.");
    }
    ImGui::EndChild();

    // ---- footer: Account... on the left, CANCEL / DOWNLOAD on the right
    const ImVec2 f{p.x, ImGui::GetCursorScreenPos().y + 14};
    float tw = 0;
    const bool canDownload = m_selectedResult >= 0 && hasResults && !downloading && !searching;
    if (textButton(downloading ? "DOWNLOADING..." : "DOWNLOAD", {f.x + inner, f.y}, IM_COL32(110, 165, 255, 255),
                   canDownload, &tw))
        out.onlineDownload = m_selectedResult;
    float cw = 0;
    if (textButton("CANCEL", {f.x + inner - tw - 28, f.y}, IM_COL32(255, 255, 255, 190), true, &cw) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape))
        m_resultsOpen = false;
    ImGui::SetCursorScreenPos({f.x - 6, f.y - 6});
    if (ImGui::InvisibleButton("##acct", {ImGui::CalcTextSize("Account...").x + 12, 30})) openAccountForm(online);
    dl->AddText({f.x, f.y}, IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 220 : 120), "Account...");
    ImGui::End();
}

float Overlay::panelWidth(float displayW) { return std::min(420.0f, std::max(300.0f, displayW * 0.42f)); }

void Overlay::drawSidePanel(const PlayerState& st, const AirPlayView& airplay, const OnlineView& online,
                            OverlayActions& out) {
    (void)airplay;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float w = panelWidth(size.x);
    const float x = size.x - m_panelVisibleW;

    ImGui::SetNextWindowPos({x, 0});
    ImGui::SetNextWindowSize({w, size.y});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 0.99f));
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
    dl->AddLine({x, 0}, {x, size.y}, IM_COL32(255, 255, 255, 26));
    const float inner = w - 40.0f;
    const ImVec2 top = ImGui::GetCursorScreenPos();

    // ---- header: close button on the left, tabs on the right
    const ImVec2 closeC{top.x + 18, top.y + 18};
    ImGui::SetCursorScreenPos({closeC.x - 18, closeC.y - 18});
    if (ImGui::InvisibleButton("##close", {36, 36})) m_panelOpen = false;
    dl->AddCircleFilled(closeC, 18, IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 36 : 22), 32);
    dl->AddLine({closeC.x - 3, closeC.y - 6}, {closeC.x + 3, closeC.y}, IM_COL32(255, 255, 255, 230), 2.0f);
    dl->AddLine({closeC.x + 3, closeC.y}, {closeC.x - 3, closeC.y + 6}, IM_COL32(255, 255, 255, 230), 2.0f);

    struct Tab { PanelTab tab; const char* label; };
    const Tab tabs[] = {{PanelTab::Video, "Video"}, {PanelTab::Audio, "Audio"}, {PanelTab::Subtitles, "Subtitles"}};
    float tabsW = 8;
    for (const Tab& t : tabs) tabsW += (t.tab == m_panelTab ? 44 + ImGui::CalcTextSize(t.label).x + 8 : 44);
    float tx = top.x + inner - tabsW;
    const ImVec2 t0{tx, top.y}, t1{tx + tabsW, top.y + 36};
    dl->AddRectFilled(t0, t1, IM_COL32(255, 255, 255, 18), 18.0f);
    tx += 4;
    for (const Tab& t : tabs) {
        const bool sel = t.tab == m_panelTab;
        const float tw = sel ? 44 + ImGui::CalcTextSize(t.label).x + 8 : 44;
        ImGui::SetCursorScreenPos({tx, top.y + 4});
        ImGui::PushID(t.label);
        if (ImGui::InvisibleButton("##tab", {tw, 28})) m_panelTab = t.tab;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (sel || hov) dl->AddRectFilled({tx, top.y + 4}, {tx + tw, top.y + 32}, IM_COL32(255, 255, 255, sel ? 34 : 16), 14.0f);
        const ImVec2 ic{tx + 22, top.y + 18};
        const ImU32 c = IM_COL32(255, 255, 255, 235);
        if (t.tab == PanelTab::Video)      iconPicture(dl, ic, c);
        else if (t.tab == PanelTab::Audio) iconWaveform(dl, ic, c);
        else                               iconBubble(dl, ic, c);
        if (sel) dl->AddText({tx + 38, top.y + 18 - ImGui::GetTextLineHeight() * 0.5f}, c, t.label);
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
        dl->AddRectFilled(a, b, IM_COL32(30, 30, 35, 190), 10.0f);
        const std::string text = note;
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, inner - 40);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {a.x + (inner - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f},
                    IM_COL32(255, 255, 255, 200), text.c_str(), nullptr, inner - 40);
    };
    const std::string notOnTv = "Not available while playing on " + tvName;

    if (m_panelTab == PanelTab::Subtitles && m_accountOpen) {
        drawAccountForm(online, inner, out);
    } else if (m_panelTab == PanelTab::Subtitles) {
        const bool subsOff = casting && !castHls;  // the TV got the plain file: no subtitles there
        const std::string subsNote = "Load a subtitle file or search online to show subtitles on " + tvName;
        section(subsOff, subsNote.c_str(), [&] {
            bool visibleOn = st.subVisible;
            if (sectionHeader(nullptr, "Subtitle Track", inner, &visibleOn)) {
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
            ImGui::TextDisabled("Changes reach %s after a short pause.", tvName.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy({0, 18});
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        if (pillButton("Load Subtitle...", bp)) out.loadSubFile = true;
        const float loadW = ImGui::CalcTextSize("Load Subtitle...").x + 40 + 10;
        if (pillButton("Search Online...", {bp.x + loadW, bp.y})) {
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
            sectionHeader(nullptr, "Audio Track", inner, nullptr);
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
            sectionHeader(nullptr, "Video Track", inner, nullptr);
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
