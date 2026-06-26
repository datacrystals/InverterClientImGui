#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include <chrono>

struct ConfigInfo {
    std::string path;
    std::string name;
    std::string saved_at;
};

class ConfigManager {
public:
    ConfigManager();

    // Directory where configs live (created on demand).
    std::string configDir() const;
    std::string autosavePath() const;
    std::string recentPath() const;

    // Save / load the three plot sets.
    bool save(const std::string& path,
              const std::string& name,
              const std::unordered_set<std::string> plot_set[3]) const;

    bool load(const std::string& path,
              std::unordered_set<std::string> plot_set[3],
              std::string* out_name = nullptr) const;

    // Recent-config list (most recent first).
    std::vector<ConfigInfo> recentConfigs() const;

    // Add a config file to the recent list and trim to max_recent.
    bool touchRecent(const std::string& path, size_t max_recent = 10);

    // Convenience: save current layout as the automatic startup layout.
    bool saveAutosave(const std::unordered_set<std::string> plot_set[3]) const;
    bool loadAutosave(std::unordered_set<std::string> plot_set[3]) const;

    // Convenience: save a named layout and update recent list.
    bool saveNamed(const std::string& name,
                   const std::unordered_set<std::string> plot_set[3]);

private:
    bool ensureConfigDir() const;
    static std::string timestampNow();
};
