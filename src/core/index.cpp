#include "index.h"

#include "util.h"

#include <nlohmann/json.hpp>

namespace cl {

using nlohmann::json;

namespace {

std::string str(const json& obj, const char* key) {
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

const IndexEntry* Index::find(const std::string& id) const {
    for (const auto& e : images)
        if (e.id == id) return &e;
    return nullptr;
}

bool parse_index(const std::string& text, Index& out, std::string& error) {
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        error = "index is not valid JSON";
        return false;
    }
    auto images = doc.find("images");
    if (images == doc.end() || !images->is_array()) {
        error = "index has no image list";
        return false;
    }

    out = Index();
    for (const auto& item : *images) {
        if (!item.is_object()) continue;
        IndexEntry e;
        e.id = str(item, "id");
        e.url = str(item, "url");
        if (!valid_id(e.id) || reserved_id(e.id) || e.url.empty()) continue;
        if (out.find(e.id)) continue;
        e.channel = str(item, "channel");
        e.label = str(item, "label");
        if (e.label.empty()) e.label = e.id;
        e.version = str(item, "version");
        e.sha256 = str(item, "sha256");
        e.last_modified = str(item, "last_modified");
        auto size = item.find("size");
        if (size != item.end() && size->is_number_unsigned()) e.size = size->get<uint64_t>();
        out.images.push_back(std::move(e));
    }

    std::string latest = str(doc, "latest_release");
    if (out.find(latest)) out.latest_release = latest;
    return true;
}

}  // namespace cl
