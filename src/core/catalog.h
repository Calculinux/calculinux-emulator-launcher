// What the launcher lists: the published index merged with what is on disk.
#pragma once

#include "index.h"
#include "state.h"

#include <string>
#include <vector>

namespace cl {

enum class ImageStatus {
    NotDownloaded,
    Downloaded,
    UpdateAvailable,  // downloaded, but the server has a newer image under this id
    Unpublished,      // downloaded, and no longer in the index
};

struct CatalogRow {
    IndexEntry entry;  // from the index, or rebuilt from meta.json when unpublished
    ImageStatus status = ImageStatus::NotDownloaded;
    bool has_state = false;  // overlay or U-Boot environment present

    bool downloaded() const { return status != ImageStatus::NotDownloaded; }
};

// Index order, then downloaded images the index does not list. With
// online false (index is a cached copy, the server is unreachable), images
// that are not downloaded are left out and nothing is marked unpublished or
// updatable.
std::vector<CatalogRow> build_catalog(const Index& index, const State& state, bool online);

const char* status_text(ImageStatus status);

// Row to preselect: the last image booted, else this build's own image,
// else the latest release, else the first row. -1 if there are no rows.
int pick_default(const std::vector<CatalogRow>& rows, const std::string& last_id,
                 const std::string& default_id, const std::string& latest_release);

}  // namespace cl
