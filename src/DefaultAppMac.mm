#include "DefaultApp.h"
#include "I18n.h"

#import <Cocoa/Cocoa.h>
#import <CoreServices/CoreServices.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// LSSetDefaultRoleHandlerForContentType is deprecated in favour of the NSWorkspace call, but
// that one asks the user to confirm every single type (a dozen dialogs). The user already
// asked for this with the button, so the quiet call is the friendlier one.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace {

NSString* bundleId() { return [[NSBundle mainBundle] bundleIdentifier]; }

// The content types of the video extensions Tolga plays, as this Mac knows them.
NSArray<NSString*>* videoTypes() {
    NSMutableOrderedSet<NSString*>* ids = [NSMutableOrderedSet orderedSet];
    for (NSString* ext in @[ @"mkv", @"mp4", @"m4v", @"mov", @"avi", @"webm", @"ts", @"m2ts", @"mts",
                             @"mpg", @"mpeg", @"wmv", @"flv", @"3gp", @"ogv" ]) {
        UTType* t = [UTType typeWithFilenameExtension:ext];
        if (t) [ids addObject:t.identifier];
    }
    return ids.array;
}

bool opensWithUs(NSString* ext) {
    UTType* t = [UTType typeWithFilenameExtension:ext];
    if (!t) return false;
    CFStringRef handler = LSCopyDefaultRoleHandlerForContentType((__bridge CFStringRef)t.identifier, kLSRolesAll);
    if (!handler) return false;
    const bool ours = [(__bridge NSString*)handler caseInsensitiveCompare:bundleId()] == NSOrderedSame;
    CFRelease(handler);
    return ours;
}

}  // namespace

namespace defaultapp {

bool available() { return bundleId().length > 0; }

bool isDefault() {
    @autoreleasepool {
        return available() && opensWithUs(@"mkv") && opensWithUs(@"mp4");
    }
}

std::string makeDefault() {
    @autoreleasepool {
        if (!available()) return T("Only the installed app can be the default player.");
        // make sure Launch Services knows this copy (and its document types) first
        LSRegisterURL((__bridge CFURLRef)[[NSBundle mainBundle] bundleURL], true);
        int failed = 0;
        for (NSString* uti in videoTypes())
            if (LSSetDefaultRoleHandlerForContentType((__bridge CFStringRef)uti, kLSRolesAll,
                                                      (__bridge CFStringRef)bundleId()) != noErr)
                ++failed;
        if (isDefault()) return T("Done: double-clicking a video now opens Tolga Player.");
        if (failed) return T("macOS didn't allow it. In Finder: Get Info on a video > Open with > Tolga Player > Change All.");
        return T("Done. If a video still opens elsewhere, use Get Info > Open with > Change All in Finder.");
    }
}

void refresh() {}

}  // namespace defaultapp
