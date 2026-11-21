#include "ResumePositions.h"
#include "Paths.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
constexpr double kMinDuration = 120.0;  // clips shorter than this just start over
constexpr double kMinPosition = 10.0;   // a few seconds in isn't worth continuing from
constexpr unsigned long long kSaveEvery = 10000;  // ms: often enough for a crash or power cut

bool localFile(const std::string& path) { return !path.empty() && path.find("://") == std::string::npos; }
}  // namespace

ResumePositions::ResumePositions(std::string configDir) {
    if (!configDir.empty()) m_file = configDir + "positions.txt";
    load();
}

void ResumePositions::load() {
    m_entries.clear();
    if (m_file.empty()) return;
    std::ifstream in(utf8Path(m_file));
    std::string line;
    while (std::getline(in, line) && m_entries.size() < kMax) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab + 1 >= line.size()) continue;
        const double seconds = std::atof(line.substr(0, tab).c_str());
        if (seconds > 0) m_entries.emplace_back(line.substr(tab + 1), seconds);
    }
}

void ResumePositions::save() {
    m_savedAt = SDL_GetTicks();
    m_dirty = false;
    if (m_file.empty()) return;
    const std::string tmp = m_file + ".tmp";
    {
        std::ofstream out(utf8Path(tmp), std::ios::trunc);
        for (const auto& [path, seconds] : m_entries) out << int64_t(seconds) << '\t' << path << '\n';
        if (!out) return;
    }
    std::error_code ec;
    std::filesystem::rename(utf8Path(tmp), utf8Path(m_file), ec);
}

void ResumePositions::flush() {
    if (m_dirty) save();
}

double ResumePositions::get(const std::string& path) const {
    for (const auto& [p, seconds] : m_entries)
        if (p == path) return seconds;
    return -1;
}

void ResumePositions::update(const std::string& path, double seconds, double duration) {
    if (!localFile(path) || !(duration >= kMinDuration) || !(seconds >= 0)) return;
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& e) { return e.first == path; });
    const bool atEnd = seconds >= duration - std::max(30.0, duration * 0.05);
    if (atEnd) {
        // watched to the end: next time from the beginning
        if (it != m_entries.end()) {
            m_entries.erase(it);
            m_dirty = true;
        }
    } else if (seconds >= kMinPosition) {
        // (a position near 0 changes nothing: right after opening, before the jump to where it
        // was left, the file is still at 0 for a moment)
        if (it == m_entries.end() || std::abs(it->second - seconds) >= 1.0 || it != m_entries.begin()) {
            if (it != m_entries.end()) m_entries.erase(it);
            m_entries.insert(m_entries.begin(), {path, seconds});
            if (m_entries.size() > kMax) m_entries.resize(kMax);
            m_dirty = true;
        }
    }
    if (m_dirty && SDL_GetTicks() - m_savedAt >= kSaveEvery) save();
}
