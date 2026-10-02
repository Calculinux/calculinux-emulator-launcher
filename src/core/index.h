// The published image index (emulator/<machine>/index.json on the feed
// server, written by meta-calculinux's generate-emulator-index.py).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cl {

struct IndexEntry {
    std::string id;       // tag name, or continuous-main / continuous-develop
    std::string channel;  // release, prerelease, continuous-main, continuous-develop
    std::string label;
    std::string version;
    std::string url;
    std::string sha256;
    std::string last_modified;
    uint64_t size = 0;
};

struct Index {
    std::vector<IndexEntry> images;
    std::string latest_release;  // id, empty if there is no stable release

    const IndexEntry* find(const std::string& id) const;
};

// Entries the launcher cannot use (bad id, no URL) are skipped, so an index
// from a newer generator still loads. Fails only if the document is not an
// index at all.
bool parse_index(const std::string& json, Index& out, std::string& error);

}  // namespace cl
