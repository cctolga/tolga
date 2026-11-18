#pragma once
// The window's frame outside macOS (macOS has its own, see MacWindow.h).
//
// Linux: the window has no system title bar. Tolga draws the title and the window buttons
// over the video (Overlay::drawWindowChrome), the top strip drags the window and the edges
// resize it (SDL hit test), and the corners are rounded when the desktop can show a
// transparent window. TOLGA_NATIVE_FRAME=1 brings the system frame back.
// Windows: the system title bar stays, in dark or light to match Tolga's appearance.

#include <cstdint>

struct SDL_Window;

namespace chrome {

bool customFrame();  // Linux, unless TOLGA_NATIVE_FRAME=1

// Extra SDL window flags for the custom frame (borderless; transparent for the corners).
uint64_t windowFlags(bool transparent);

// After the window exists: the strip `barH` high drags it, except the `buttonsW` on its
// right; the edges resize it. Both in UI units; `unitToWindow` turns them into window points.
void setup(SDL_Window* window, float barH, float buttonsW);
void update(SDL_Window* window, float unitToWindow, bool fullscreen);

// Cuts the corners round (clears them to transparent) after the frame is drawn.
void roundCorners(SDL_Window* window, int fbw, int fbh, float pixelsPerPoint, bool fullscreen);

// The window keeps the video's shape (`ratio` = width / height). With the custom frame
// Tolga does it itself: `resized` on every resize event, `tick` every turn of the loop.
void keepAspect(SDL_Window* window, float ratio);
void resized(SDL_Window* window);
void tick(SDL_Window* window, bool videoOpen);

// Windows: the title bar in dark (true) or light; no-op elsewhere.
void setDarkTitlebar(SDL_Window* window, bool dark);

}  // namespace chrome
