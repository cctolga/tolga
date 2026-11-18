#include "WindowChrome.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace chrome {

namespace {

struct Hit {
    bool on = false;
    bool fullscreen = false;
    float barH = 32, buttonsW = 138, unit = 1;  // UI units -> window points
} g_hit;

constexpr int kEdge = 6;          // points along the edges that resize the window
constexpr float kRadius = 10.0f;  // corner radius in points

bool maximized(SDL_Window* w) { return (SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED) != 0; }

SDL_HitTestResult hitTest(SDL_Window* window, const SDL_Point* p, void*) {
    if (!g_hit.on || g_hit.fullscreen) return SDL_HITTEST_NORMAL;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (!maximized(window)) {
        const bool left = p->x < kEdge, right = p->x >= w - kEdge, top = p->y < kEdge, bottom = p->y >= h - kEdge;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    }
    // the title strip moves the window (the system does it, which works on Wayland too)
    if (p->y < g_hit.barH * g_hit.unit && p->x < w - g_hit.buttonsW * g_hit.unit) return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

// The video's shape, kept by Tolga itself with the custom frame (see keepAspect).
struct Aspect {
    float ratio = 0;            // width / height, 0 = none
    int w = 0, h = 0;           // the size it last had the right shape at
    uint64_t snapAt = 0;        // when to fix the shape after a resize (ms), 0 = nothing to do
} g_aspect;

}  // namespace

bool customFrame() {
#if defined(__linux__)
    const char* native = std::getenv("TOLGA_NATIVE_FRAME");
    return !(native && *native && *native != '0');
#else
    return false;
#endif
}

uint64_t windowFlags(bool transparent) {
    if (!customFrame()) return 0;
    return SDL_WINDOW_BORDERLESS | (transparent ? SDL_WINDOW_TRANSPARENT : 0);
}

void setup(SDL_Window* window, float barH, float buttonsW) {
    if (!customFrame()) return;
    g_hit.on = true;
    g_hit.barH = barH;
    g_hit.buttonsW = buttonsW;
    SDL_SetWindowHitTest(window, hitTest, nullptr);
}

void update(SDL_Window*, float unitToWindow, bool fullscreen) {
    if (unitToWindow > 0) g_hit.unit = unitToWindow;
    g_hit.fullscreen = fullscreen;
}

void roundCorners(SDL_Window* window, int fbw, int fbh, float pixelsPerPoint, bool fullscreen) {
    if (!customFrame() || fullscreen || maximized(window)) return;
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_TRANSPARENT)) return;
    // Row by row, clear what lies outside a quarter circle in each corner to transparent:
    // a few dozen tiny clears, no shader needed (works with OpenGL 2.1 too).
    const int r = int(std::lround(kRadius * (pixelsPerPoint > 0 ? pixelsPerPoint : 1.0f)));
    if (r <= 0 || fbw < 2 * r || fbh < 2 * r) return;
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    for (int y = 0; y < r; ++y) {
        const float dy = float(r) - (float(y) + 0.5f);
        const int cut = int(std::ceil(float(r) - std::sqrt(std::max(0.0f, float(r * r) - dy * dy))));
        if (cut <= 0) continue;
        // GL rows count from the bottom
        glScissor(0, fbh - 1 - y, cut, 1);       glClear(GL_COLOR_BUFFER_BIT);  // top left
        glScissor(fbw - cut, fbh - 1 - y, cut, 1); glClear(GL_COLOR_BUFFER_BIT);  // top right
        glScissor(0, y, cut, 1);                 glClear(GL_COLOR_BUFFER_BIT);  // bottom left
        glScissor(fbw - cut, y, cut, 1);         glClear(GL_COLOR_BUFFER_BIT);  // bottom right
    }
    glDisable(GL_SCISSOR_TEST);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
}

void keepAspect(SDL_Window* window, float ratio) {
    if (!customFrame()) {
        SDL_SetWindowAspectRatio(window, ratio, ratio);
        return;
    }
    // With a fixed aspect ratio Ubuntu's window manager lets the edges of a frameless window
    // only shrink it. Without one the edges resize freely, and Tolga fixes the shape once
    // the mouse lets go (see tick).
    g_aspect.ratio = ratio;
    SDL_GetWindowSize(window, &g_aspect.w, &g_aspect.h);
    g_aspect.snapAt = 0;
}

void resized(SDL_Window*) {
    if (customFrame() && g_aspect.ratio > 0) g_aspect.snapAt = SDL_GetTicks() + 150;
}

void tick(SDL_Window* window, bool videoOpen) {
    if (!g_aspect.snapAt || SDL_GetTicks() < g_aspect.snapAt) return;
    if (SDL_GetGlobalMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) {  // still dragging
        g_aspect.snapAt = SDL_GetTicks() + 100;
        return;
    }
    g_aspect.snapAt = 0;
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    if (!videoOpen || g_aspect.ratio <= 0 || (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (w <= 0 || h <= 0) return;
    // keep the side the user moved more, fit the other to the video
    const float dw = g_aspect.w > 0 ? std::abs(float(w - g_aspect.w)) / float(g_aspect.w) : 1.0f;
    const float dh = g_aspect.h > 0 ? std::abs(float(h - g_aspect.h)) / float(g_aspect.h) : 0.0f;
    int nw = w, nh = h;
    if (dw >= dh) nh = int(std::lround(float(w) / g_aspect.ratio));
    else          nw = int(std::lround(float(h) * g_aspect.ratio));
    if (std::abs(nw - w) > 1 || std::abs(nh - h) > 1) SDL_SetWindowSize(window, nw, nh);
    g_aspect.w = nw;
    g_aspect.h = nh;
}

void setDarkTitlebar(SDL_Window* window, bool dark) {
#ifdef _WIN32
    static int last = -1;
    if (last == int(dark)) return;
    HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) return;
    const BOOL value = dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 since Windows 10 20H1, 19 before that
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &value, sizeof value)))
        DwmSetWindowAttribute(hwnd, 19, &value, sizeof value);
    last = int(dark);
    // Windows repaints the title bar only when the window is activated again
    if (GetActiveWindow() == hwnd) {
        SendMessageW(hwnd, WM_NCACTIVATE, FALSE, 0);
        SendMessageW(hwnd, WM_NCACTIVATE, TRUE, 0);
    }
#else
    (void)window;
    (void)dark;
#endif
}

}  // namespace chrome
