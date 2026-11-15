#pragma once
// macOS window look like IINA: the title bar is transparent and the video runs under it;
// the traffic-light buttons and the title fade in and out together with the controls.
// No-ops on other platforms.

#include <string>

struct SDL_Window;

#ifdef __APPLE__
void macSetupWindow(SDL_Window* window);                 // call again after leaving fullscreen
void macSetTitlebarAlpha(SDL_Window* window, float alpha);  // 0 = hidden (and not clickable), 1 = shown
// The size of the OpenGL drawable in pixels and its pixels per point, read from the view
// itself. While the window zooms or goes fullscreen, SDL's window and pixel sizes can come
// from different moments of the animation; this one always matches what is drawn.
// Also lets the GL context catch up with a new view size.
bool macDrawableSize(SDL_Window* window, int* w, int* h, float* scale);
// "About Tolga Player" in the app menu calls `onAbout` instead of the standard panel.
void macHookAboutMenu(void (*onAbout)());
// Brings the app and the window to the front. Since macOS 14 SDL leaves activation to the
// system, and when Finder opens a file with the app ("Open With") the window then stays
// invisible for several seconds while the movie already plays.
void macBringToFront(SDL_Window* window);
// The system's recent documents (shown in the app's Dock menu).
void macNoteRecentFile(const std::string& path);
void macClearRecentFiles();
#else
inline void macNoteRecentFile(const std::string&) {}
inline void macClearRecentFiles() {}
inline void macBringToFront(SDL_Window*) {}
inline void macHookAboutMenu(void (*)()) {}
inline void macSetupWindow(SDL_Window*) {}
inline void macSetTitlebarAlpha(SDL_Window*, float) {}
inline bool macDrawableSize(SDL_Window*, int*, int*, float*) { return false; }
#endif
