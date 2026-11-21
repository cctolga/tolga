#pragma once
// Where each video was left, so opening it again continues there. Kept in
// <config dir>/positions.txt, one "seconds<TAB>path" line per file, the most recent first.
// Local files only. A video watched to its end (the last 5%, at least the last 30 s) is
// forgotten, so it starts from the beginning the next time.

#include <string>
#include <utility>
#include <vector>

class ResumePositions {
public:
    static constexpr size_t kMax = 300;  // files remembered; the oldest go first

    explicit ResumePositions(std::string configDir);
    ~ResumePositions() { flush(); }

    // Where `path` was left, or -1 (never opened, not far in, or watched to the end).
    double get(const std::string& path) const;
    // The position now (called often while a file plays). Written to disk now and then.
    void update(const std::string& path, double seconds, double duration);
    void flush();  // writes now if something changed

private:
    void load();
    void save();

    std::string m_file;
    std::vector<std::pair<std::string, double>> m_entries;  // path, seconds: most recent first
    bool m_dirty = false;
    unsigned long long m_savedAt = 0;  // ms
};
