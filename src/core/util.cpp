#include "util.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace cl {

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    out = buf.str();
    return true;
}

bool write_file_atomic(const fs::path& path, const std::string& data) {
    fs::path tmp = path;
    tmp += ".tmp" + std::to_string(getpid());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << data;
        out.flush();
        if (!out) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

std::string first_token(const std::string& s) {
    std::string t = trim(s);
    size_t end = t.find_first_of(" \t\r\n");
    return end == std::string::npos ? t : t.substr(0, end);
}

std::string human_size(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024 * 1024)
        snprintf(buf, sizeof buf, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, sizeof buf, "%.0f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, sizeof buf, "%.0f KB", bytes / 1024.0);
    else
        snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
    return buf;
}

std::string now_iso8601() {
    time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64 || id[0] == '.') return false;
    for (unsigned char c : id) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '+' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool reserved_id(const std::string& id) {
    return id == "local" || id == "custom" || id == "legacy";
}

}  // namespace cl
