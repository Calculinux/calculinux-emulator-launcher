#include "http.h"

#include <cstdio>
#include <cstdlib>

#include <curl/curl.h>

namespace cl {

namespace {

// Where distributions keep their CA bundle.
const char* const kHostCaFiles[] = {
    "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Arch, Gentoo
    "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL
    "/etc/ssl/ca-bundle.pem",              // openSUSE
    "/etc/ssl/cert.pem",                   // Alpine
};

bool readable(const std::string& path) {
    if (path.empty()) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

size_t write_string(char* data, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

size_t write_file(char* data, size_t size, size_t n, void* user) {
    return fwrite(data, size, n, static_cast<FILE*>(user)) * size;
}

struct Transfer {
    Progress* progress;
    uint64_t resume_from;
};

int on_progress(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    Transfer* t = static_cast<Transfer*>(user);
    if (t->progress->cancel.load()) return 1;
    t->progress->now = t->resume_from + (uint64_t)dlnow;
    if (dltotal > 0) t->progress->total = t->resume_from + (uint64_t)dltotal;
    return 0;
}

void common_options(CURL* curl, const std::string& url, const HttpOptions& options, char* errbuf) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, options.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,file");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
}

// Run the transfer; if the server's certificate does not verify against the
// host's CA file, try once more with the bundled one. rewind() resets the
// output before the second attempt (nothing has been received by then).
template <typename Rewind>
HttpResult perform(CURL* curl, const HttpOptions& options, char* errbuf, Rewind rewind) {
    std::string ca = pick_ca_file(options);
    if (!ca.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());

    CURLcode code = curl_easy_perform(curl);
    bool cert_problem = code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE;
    if (cert_problem && ca != options.bundled_ca && readable(options.bundled_ca)) {
        rewind();
        curl_easy_setopt(curl, CURLOPT_CAINFO, options.bundled_ca.c_str());
        code = curl_easy_perform(curl);
    }

    HttpResult result;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    result.ok = code == CURLE_OK;
    result.cancelled = code == CURLE_ABORTED_BY_CALLBACK;
    result.range_unsupported = code == CURLE_RANGE_ERROR;
    if (!result.ok) result.error = errbuf[0] ? errbuf : curl_easy_strerror(code);
    return result;
}

}  // namespace

void http_global_init() { curl_global_init(CURL_GLOBAL_DEFAULT); }

std::string pick_ca_file(const HttpOptions& options) {
    for (const char* var : {"SSL_CERT_FILE", "CURL_CA_BUNDLE"}) {
        const char* value = getenv(var);
        if (value && readable(value)) return value;
    }
    for (const char* path : kHostCaFiles)
        if (readable(path)) return path;
    if (readable(options.bundled_ca)) return options.bundled_ca;
    return "";
}

HttpResult http_get(const std::string& url, std::string& body, const HttpOptions& options, long timeout_seconds) {
    CURL* curl = curl_easy_init();
    if (!curl) return HttpResult{false, false, false, 0, "cannot initialise libcurl"};
    char errbuf[CURL_ERROR_SIZE] = "";
    common_options(curl, url, options, errbuf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_slist* headers = curl_slist_append(nullptr, "Cache-Control: no-cache");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    body.clear();
    HttpResult result = perform(curl, options, errbuf, [&] { body.clear(); });
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

HttpResult http_download(const std::string& url, const std::filesystem::path& path, uint64_t resume_from,
                         Progress& progress, const HttpOptions& options) {
    FILE* out = fopen(path.c_str(), resume_from ? "ab" : "wb");
    if (!out) return HttpResult{false, false, false, 0, "cannot write " + path.string()};

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(out);
        return HttpResult{false, false, false, 0, "cannot initialise libcurl"};
    }
    char errbuf[CURL_ERROR_SIZE] = "";
    common_options(curl, url, options, errbuf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    // Give up on a stalled transfer, not on a slow one.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resume_from);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    Transfer transfer{&progress, resume_from};
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);

    progress.now = resume_from;
    HttpResult result = perform(curl, options, errbuf, [] {});
    curl_easy_cleanup(curl);
    if (fclose(out) != 0 && result.ok) {
        result.ok = false;
        result.error = "cannot write " + path.string();
    }
    return result;
}

}  // namespace cl
