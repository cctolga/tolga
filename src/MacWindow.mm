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

bool macDrawableSize(SDL_Window* window, int* w, int* h, float* scale) {
    NSWindow* win = nsWindowOf(window);
    NSView* view = win ? win.contentView : nil;
    if (!view) return false;
    const NSRect px = [view convertRectToBacking:view.bounds];
    const int pw = (int)px.size.width, ph = (int)px.size.height;
    if (pw <= 0 || ph <= 0) return false;
    static int lastW = 0, lastH = 0;
    if (pw != lastW || ph != lastH) {  // new size: let the GL drawable follow the view
        lastW = pw;
        lastH = ph;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [[NSOpenGLContext currentContext] update];
#pragma clang diagnostic pop
    }
    *w = pw;
    *h = ph;
    *scale = view.bounds.size.width > 0 ? (float)(px.size.width / view.bounds.size.width) : 1.0f;
    return true;
}

@interface TolgaAboutTarget : NSObject
@property (nonatomic) void (*callback)(void);
- (void)showAbout:(id)sender;
@end
@implementation TolgaAboutTarget
- (void)showAbout:(id)sender { (void)sender; if (self.callback) self.callback(); }
@end

void macHookAboutMenu(void (*onAbout)()) {
    static TolgaAboutTarget* target = nil;  // menu items don't retain their target
    if (!target) target = [TolgaAboutTarget new];
    target.callback = onAbout;
    NSMenu* appMenu = [[[NSApp mainMenu] itemAtIndex:0] submenu];
    for (NSMenuItem* item in appMenu.itemArray) {
        if (item.action == @selector(orderFrontStandardAboutPanel:)) {
            item.target = target;
            item.action = @selector(showAbout:);
            item.title = @"About Tolga Player";
        }
    }
}
