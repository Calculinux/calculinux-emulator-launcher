#include "session.h"

namespace cl {

const char* const kUnindexedId = "legacy";
const char* const kCustomId = "custom";

Session::Session(Config cfg) : config(std::move(cfg)), state(config.state_dir) {
    http.bundled_ca = config.ca_bundle;
    http.user_agent = "calculinux-emulator-launcher/" + config.app_version;
}

void Session::load_cached() {
    online = false;
    index = Index();
    std::string text, error;
    if (read_file(state.cached_index_path(), text)) parse_index(text, index, error);
}

void Session::refresh() {
    if (config.index_url.empty()) {
        index_error = "no index URL configured";
        load_cached();
        return;
    }
    std::string text, error;
    HttpResult result = http_get(config.index_url, text, http);
    Index fresh;
    if (!result.ok) {
        index_error = result.error;
        load_cached();
        return;
    }
    if (!parse_index(text, fresh, error)) {
        index_error = error;
        load_cached();
        return;
    }
    index = std::move(fresh);
    online = true;
    index_error.clear();
    write_file_atomic(state.cached_index_path(), text);
    state.adopt_legacy(index);
}

std::vector<CatalogRow> Session::rows() const {
    std::vector<CatalogRow> rows = build_catalog(index, state, online);
    // No index (not published yet, or never reached) and nothing on disk:
    // offer the image this build was made for.
    if (!online && rows.empty() && !config.fallback_url.empty()) {
        CatalogRow row;
        row.entry.id = kUnindexedId;
        row.entry.label = "Default image";
        row.entry.version = config.app_version;
        row.entry.url = config.fallback_url;
        row.status = ImageStatus::NotDownloaded;
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace cl
