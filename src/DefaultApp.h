#pragma once
// "Make Tolga Player the default video player", as far as each system allows it.
//   macOS:   Launch Services makes Tolga the viewer for the video types (MKV, MP4, MOV ...).
//   Windows: programs may not set defaults themselves; Windows' Default apps settings open
//            at Tolga Player, where the user picks it (needs the installed version).
//   Linux:   Tolga's .desktop file and icon go to the user's applications folder (so it is
//            also in the applications menu) and xdg-mime makes it the default for videos.
// Nothing changes unless the user asks for it.

#include <string>

namespace defaultapp {

// Whether this is the Microsoft Store version (an MSIX package); false elsewhere.
bool packaged();

// Whether this copy can offer it at all (macOS: only the app bundle).
bool available();

// Whether double-clicking an MKV or MP4 file opens Tolga Player.
bool isDefault();

// Does what the system allows and returns one short sentence for the user.
std::string makeDefault();

// At start-up. Linux: if Tolga was made the default earlier from a file that is gone (an
// AppImage deleted after a newer one was downloaded), the entry is pointed at this copy, so
// double-clicking videos works again. Nothing else changes; no-op elsewhere.
void refresh();

}  // namespace defaultapp
