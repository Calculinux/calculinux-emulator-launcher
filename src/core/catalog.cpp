#include "catalog.h"

namespace cl {

std::vector<CatalogRow> build_catalog(const Index& index, const State& state, bool online) {
    std::vector<CatalogRow> rows;
    for (const auto& entry : index.images) {
        CatalogRow row;
        row.entry = entry;
        std::string installed = state.installed_sha(entry.id);
        if (!state.is_downloaded(entry.id)) {
            if (!online) continue;
            row.status = ImageStatus::NotDownloaded;
        } else if (online && !entry.sha256.empty() && installed != entry.sha256) {
            row.status = ImageStatus::UpdateAvailable;
        } else {
            row.status = ImageStatus::Downloaded;
        }
        row.has_state = state.has_state(entry.id);
        rows.push_back(std::move(row));
    }

    for (const auto& id : state.downloaded_ids()) {
        if (index.find(id)) continue;
        CatalogRow row;
        ImageMeta meta;
        state.load_meta(id, meta);
        row.entry.id = id;
        row.entry.channel = meta.channel;
        if (id == "legacy")
            row.entry.label = "Previously downloaded";
        else
            row.entry.label = meta.label.empty() ? id : meta.label;
        row.entry.version = meta.version;
        row.entry.url = meta.url;
        std::error_code ec;
        auto size = fs::file_size(state.base_path(id), ec);
        row.entry.size = ec ? 0 : (uint64_t)size;
        row.entry.sha256 = state.installed_sha(id);
        row.status = online ? ImageStatus::Unpublished : ImageStatus::Downloaded;
        row.has_state = state.has_state(id);
        rows.push_back(std::move(row));
    }
    return rows;
}

const char* status_text(ImageStatus status) {
    switch (status) {
        case ImageStatus::NotDownloaded: return "Not downloaded";
        case ImageStatus::Downloaded: return "Downloaded";
        case ImageStatus::UpdateAvailable: return "Update available";
        case ImageStatus::Unpublished: return "No longer published";
    }
    return "";
}

int pick_default(const std::vector<CatalogRow>& rows, const std::string& last_id,
                 const std::string& default_id, const std::string& latest_release) {
    for (const std::string* id : {&last_id, &default_id, &latest_release}) {
        if (id->empty()) continue;
        for (size_t i = 0; i < rows.size(); i++)
            if (rows[i].entry.id == *id) return (int)i;
    }
    return rows.empty() ? -1 : 0;
}

}  // namespace cl
