// Config.h — tiny INI reader for config/coop.ini (pure C++, header-only).
#pragma once
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <cstdint>

namespace crc {

class Config {
public:
    bool load(const std::string& path) {
        std::ifstream f(path);
        if (!f) return false;
        std::string line, section;
        while (std::getline(f, line)) {
            auto s = trim(line);
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            if (s.front() == '[' && s.back() == ']') { section = lower(s.substr(1, s.size() - 2)); continue; }
            auto eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string key = lower(trim(s.substr(0, eq)));
            std::string val = trim(s.substr(eq + 1));
            m_kv[section + "." + key] = val;
        }
        return true;
    }

    std::string getStr(const std::string& sec, const std::string& key, const std::string& def = "") const {
        auto it = m_kv.find(lower(sec) + "." + lower(key));
        return it == m_kv.end() ? def : it->second;
    }
    int getInt(const std::string& sec, const std::string& key, int def = 0) const {
        auto v = getStr(sec, key);
        if (v.empty()) return def;
        try { return std::stoi(v); } catch (...) { return def; }
    }
    uint64_t getU64(const std::string& sec, const std::string& key, uint64_t def = 0) const {
        auto v = getStr(sec, key);
        if (v.empty()) return def;
        try { return std::stoull(v); } catch (...) { return def; }
    }

private:
    std::unordered_map<std::string, std::string> m_kv;
    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }
    static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
};

} // namespace crc
