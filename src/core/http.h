// libcurl wrapper: small GETs into memory and resumable downloads to a file.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace cl {

struct HttpOptions {
    // CA bundle shipped with the launcher (the AppImage's), used when the
    // host has none or the host's fails to verify the server. May be empty.
    std::string bundled_ca;
    std::string user_agent = "calculinux-emulator-launcher";
};

// Shared with the thread showing progress.
struct Progress {
    std::atomic<uint64_t> now{0};
    std::atomic<uint64_t> total{0};  // 0 while unknown
    std::atomic<bool> cancel{false};
};

struct HttpResult {
    bool ok = false;
    bool cancelled = false;
    bool range_unsupported = false;  // asked to resume, server sent the whole file
    long status = 0;                 // HTTP status, 0 for file://
    std::string error;
};

void http_global_init();

// The CA file requests will use: SSL_CERT_FILE / CURL_CA_BUNDLE, else the
// host's bundle, else the bundled one, else "" (libcurl's built-in default).
std::string pick_ca_file(const HttpOptions& options);

HttpResult http_get(const std::string& url, std::string& body, const HttpOptions& options,
                    long timeout_seconds = 20);

// Append to path from offset resume_from (0 truncates). progress.now counts
// from resume_from; progress.cancel aborts the transfer.
HttpResult http_download(const std::string& url, const std::filesystem::path& path, uint64_t resume_from,
                         Progress& progress, const HttpOptions& options);

}  // namespace cl
