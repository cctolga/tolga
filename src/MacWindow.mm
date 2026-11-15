#include "MacWindow.h"

#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>

static NSWindow* nsWindowOf(SDL_Window* window) {
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    return (NSWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
}

// The view that holds the traffic lights and the title.
static NSView* titlebarViewOf(NSWindow* win) {
    NSButton* close = [win standardWindowButton:NSWindowCloseButton];
    return close ? close.superview : nil;
}

void macSetupWindow(SDL_Window* window) {
    NSWindow* win = nsWindowOf(window);
    if (!win) return;
    win.styleMask |= NSWindowStyleMaskFullSizeContentView;  // content (the video) under the title bar
    win.titlebarAppearsTransparent = YES;
    win.titleVisibility = NSWindowTitleVisible;
    win.backgroundColor = [NSColor blackColor];
}

void macSetTitlebarAlpha(SDL_Window* window, float alpha) {
    NSWindow* win = nsWindowOf(window);
    if (!win) return;
    if (win.styleMask & NSWindowStyleMaskFullScreen) return;  // macOS handles it in fullscreen
    NSView* bar = titlebarViewOf(win);
    if (!bar) return;
    const CGFloat a = alpha < 0.01f ? 0.0 : (alpha > 0.99f ? 1.0 : alpha);
    if (bar.alphaValue != a) bar.alphaValue = a;
    // invisible buttons must not catch clicks either
    const BOOL hide = a <= 0.0;
    if (bar.hidden != hide) bar.hidden = hide;
}
