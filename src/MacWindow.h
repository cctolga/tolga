#pragma once
// macOS window look like IINA: the title bar is transparent and the video runs under it;
// the traffic-light buttons and the title fade in and out together with the controls.
// No-ops on other platforms.

struct SDL_Window;

#ifdef __APPLE__
void macSetupWindow(SDL_Window* window);                 // call again after leaving fullscreen
void macSetTitlebarAlpha(SDL_Window* window, float alpha);  // 0 = hidden (and not clickable), 1 = shown
#else
inline void macSetupWindow(SDL_Window*) {}
inline void macSetTitlebarAlpha(SDL_Window*, float) {}
#endif
