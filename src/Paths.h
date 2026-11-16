#pragma once
// Paths in Tolga are UTF-8 strings (what SDL, mpv and the file dialogs use). std::filesystem
// and the file streams would read a plain std::string in the Windows code page instead, which
// breaks names with other characters (Türkçe, 日本語...): go through these instead.
#include <filesystem>
#include <string>

inline std::filesystem::path utf8Path(const std::string& s) { return std::filesystem::u8path(s); }
inline std::string utf8String(const std::filesystem::path& p) { return p.u8string(); }
