// One launcher run: configuration, the state directory and the index.
#pragma once

#include "catalog.h"
#include "http.h"
#include "index.h"
#include "state.h"

#include <string>
#include <vector>

namespace cl {

struct Config {
    fs::path state_dir;
    std::string index_url;
    std::string default_id;    // this build's own image (tag or continuous channel)
    std::string fallback_url;  // image to offer when there is no index at all
    std::string app_version;
    std::string ca_bundle;
};

// The fallback URL's image is kept under this id, like the image of a
// migrated layout 1 directory; State::adopt_legacy() renames it once the
// index lists that image.
extern const char* const kUnindexedId;
// An image from an explicit URL (fetch --url).
extern const char* const kCustomId;

class Session {
public:
    explicit Session(Config config);

    // Fetch the index; on failure fall back to the cached copy and go
    // offline. Network access, so not for the UI thread.
    void refresh();
    // Use only the cached copy.
    void load_cached();

    std::vector<CatalogRow> rows() const;

    Config config;
    State state;
    HttpOptions http;
    Index index;
    bool online = false;
    std::string index_error;  // why we are offline
};

}  // namespace cl
