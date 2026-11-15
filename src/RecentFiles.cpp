#include "RecentFiles.h"
#include "MacWindow.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

RecentFiles::RecentFiles(std::string configDir) {
    if (!configDir.empty()) m_file = configDir + "recent.txt";
    load();
}

void RecentFiles::load() {
    m_paths.clear();
    if (m_file.empty()) return;
    std::ifstream in(m_file);
    std::string line;
    while (std::getline(in, line) && m_paths.size() < kMax) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) m_paths.push_back(line);
    }
}

void RecentFiles::save() const {
    if (m_file.empty()) return;
    const std::string tmp = m_file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const std::string& p : m_paths) out << p << '\n';
        if (!out) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, m_file, ec);
}

void RecentFiles::add(const std::string& path) {
    if (path.empty() || path.find("://") != std::string::npos) return;  // streams are not files
    m_paths.erase(std::remove(m_paths.begin(), m_paths.end(), path), m_paths.end());
    m_paths.insert(m_paths.begin(), path);
    if (m_paths.size() > kMax) m_paths.resize(kMax);
    m_checkedAt = 0;
    save();
    macNoteRecentFile(path);
}

void RecentFiles::clear() {
    m_paths.clear();
    m_existing.clear();
    save();
    macClearRecentFiles();
}

const std::vector<std::string>& RecentFiles::existing() {
    // checked at most every few seconds: the idle screen asks every frame
    const unsigned long long now = SDL_GetTicks();
    if (m_checkedAt == 0 || now - m_checkedAt > 3000) {
        m_checkedAt = now ? now : 1;
        m_existing.clear();
        for (const std::string& p : m_paths) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(std::filesystem::path(p), ec)) m_existing.push_back(p);
        }
    }
    return m_existing;
}
