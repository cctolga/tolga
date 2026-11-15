#pragma once
// The files opened most recently, newest first, kept in <config dir>/recent.txt
// (one path per line). On macOS they also go to the system's list, which the Dock
// menu of the app shows.

#include <string>
#include <vector>

class RecentFiles {
public:
    static constexpr size_t kMax = 8;

    explicit RecentFiles(std::string configDir);

    void add(const std::string& path);  // local files only; moves an existing entry to the top
    void clear();
    // Entries whose file still exists (moved or deleted files are left out, not forgotten:
    // they come back if the drive is connected again).
    const std::vector<std::string>& existing();

private:
    void load();
    void save() const;

    std::string m_file;
    std::vector<std::string> m_paths;
    std::vector<std::string> m_existing;
    unsigned long long m_checkedAt = 0;  // when m_existing was last refreshed (ms)
};
