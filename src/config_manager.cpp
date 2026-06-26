#include "config_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>

#ifdef _WIN32
  #include <windows.h>
  #include <shlobj.h>
  #include <direct.h>
  #pragma comment(lib, "shell32.lib")
#else
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace {

static constexpr const char* CONFIG_SUBDIR = "InverterClientImGui";

std::string userConfigRoot() {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        return std::string(path);
    }
    return ".";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return xdg;
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.config";
    return ".";
#endif
}

bool dirExists(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    return _stat(path.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool makeDir(const std::string& path) {
#ifdef _WIN32
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

std::string sanitizeFilename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "layout";
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string sectionName(int idx) {
    switch (idx) {
        case 0: return "[graph1]";
        case 1: return "[graph2]";
        case 2: return "[graph3]";
        default: return "[unknown]";
    }
}

int sectionIndex(const std::string& line) {
    if (line == "[graph1]") return 0;
    if (line == "[graph2]") return 1;
    if (line == "[graph3]") return 2;
    return -1;
}

} // namespace

ConfigManager::ConfigManager() = default;

std::string ConfigManager::configDir() const {
    return userConfigRoot() + "/" + CONFIG_SUBDIR;
}

std::string ConfigManager::autosavePath() const {
    return configDir() + "/autosave.cfg";
}

std::string ConfigManager::recentPath() const {
    return configDir() + "/recent.txt";
}

bool ConfigManager::ensureConfigDir() const {
    std::string root = userConfigRoot();
    if (!dirExists(root)) {
        if (!makeDir(root)) return false;
    }
    std::string dir = configDir();
    if (!dirExists(dir)) {
        if (!makeDir(dir)) return false;
    }
    return true;
}

std::string ConfigManager::timestampNow() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

bool ConfigManager::save(const std::string& path,
                         const std::string& name,
                         const std::unordered_set<std::string> plot_set[3]) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "; InverterClientImGui layout config\n";
    f << "; name: " << name << "\n";
    f << "; saved_at: " << timestampNow() << "\n";
    f << "\n";

    for (int g = 0; g < 3; ++g) {
        f << sectionName(g) << "\n";
        std::vector<std::string> sigs(plot_set[g].begin(), plot_set[g].end());
        std::sort(sigs.begin(), sigs.end());
        for (const auto& s : sigs) {
            f << s << "\n";
        }
        f << "\n";
    }
    return f.good();
}

bool ConfigManager::load(const std::string& path,
                         std::unordered_set<std::string> plot_set[3],
                         std::string* out_name) const {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    for (int g = 0; g < 3; ++g) plot_set[g].clear();

    int section = -1;
    std::string line;
    if (out_name) out_name->clear();

    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == ';' || t[0] == '#') {
            if (out_name && t.rfind("; name:", 0) == 0) {
                *out_name = trim(t.substr(7));
            }
            continue;
        }

        int idx = sectionIndex(t);
        if (idx >= 0) {
            section = idx;
            continue;
        }

        if (section >= 0 && section < 3) {
            plot_set[section].insert(t);
        }
    }
    return true;
}

std::vector<ConfigInfo> ConfigManager::recentConfigs() const {
    std::vector<ConfigInfo> out;
    std::ifstream f(recentPath());
    if (!f.is_open()) return out;

    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;

        ConfigInfo info;
        info.path = t;

        std::ifstream cf(info.path);
        if (cf.is_open()) {
            std::string cline;
            while (std::getline(cf, cline)) {
                std::string ct = trim(cline);
                if (ct.rfind("; name:", 0) == 0) {
                    info.name = trim(ct.substr(7));
                } else if (ct.rfind("; saved_at:", 0) == 0) {
                    info.saved_at = trim(ct.substr(11));
                }
                if (!info.name.empty() && !info.saved_at.empty()) break;
            }
        }
        if (info.name.empty()) {
            size_t s = info.path.find_last_of("/\\");
            info.name = (s == std::string::npos) ? info.path : info.path.substr(s + 1);
        }
        out.push_back(std::move(info));
    }
    return out;
}

bool ConfigManager::touchRecent(const std::string& path, size_t max_recent) {
    if (!ensureConfigDir()) return false;

    auto existing = recentConfigs();
    std::vector<std::string> paths;
    paths.reserve(existing.size() + 1);
    paths.push_back(path);
    for (const auto& e : existing) {
        if (e.path != path) paths.push_back(e.path);
    }
    if (paths.size() > max_recent) {
        paths.resize(max_recent);
    }

    std::ofstream f(recentPath());
    if (!f.is_open()) return false;
    for (const auto& p : paths) {
        f << p << "\n";
    }
    return f.good();
}

bool ConfigManager::saveAutosave(const std::unordered_set<std::string> plot_set[3]) const {
    if (!ensureConfigDir()) return false;
    return save(autosavePath(), "autosave", plot_set);
}

bool ConfigManager::loadAutosave(std::unordered_set<std::string> plot_set[3]) const {
    return load(autosavePath(), plot_set);
}

bool ConfigManager::saveNamed(const std::string& name,
                              const std::unordered_set<std::string> plot_set[3]) {
    if (!ensureConfigDir()) return false;
    std::string filename = configDir() + "/" + sanitizeFilename(name) + ".cfg";
    if (!save(filename, name, plot_set)) return false;
    return touchRecent(filename);
}
