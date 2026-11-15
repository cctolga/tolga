#pragma once
// Finding and calling the ffmpeg / ffprobe programs (thumbnails, file lengths).
#include <string>

// Path of a program: the copy next to Tolga's executable (the app bundle ships its own
// LGPL ffmpeg/ffprobe), then PATH, then the Homebrew locations. "" if missing.
std::string findProgram(const std::string& name);

// Path of ffmpeg ($TOLGA_FFMPEG wins), or "" if missing.
std::string findFfmpeg();

// Length of a media file in seconds using ffprobe; -1 if unknown.
double probeDuration(const std::string& path);
