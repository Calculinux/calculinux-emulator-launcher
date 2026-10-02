#include "state.h"

#include "sha256.h"

#include <algorithm>
#include <sstream>

#include <nlohmann/json.hpp>

namespace cl {

using nlohmann::json;

namespace {

const char* const kOverlayFiles[] = {"disk.qcow2", "uboot-env.img", "image-id"};

// Rename src to dst if src exists; a missing src is not an error, so a
// migration interrupted half way can be run again.
bool move_if_exists(const fs::path& src, const fs::path& dst, std::string& error) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(src, ec))) return true;
    fs::rename(src, dst, ec);
    if (ec) {
        error = "cannot move " + src.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

bool State::prepare(std::string& error) {
    std::error_code ec;
    fs::create_directories(images_dir(), ec);
    if (ec) {
        error = "cannot create " + images_dir().string() + ": " + ec.message();
        return false;
    }
    if (fs::exists(root_ / "layout", ec)) return true;
    if (!migrate_v1(error)) return false;
    if (!write_file_atomic(root_ / "layout", "2\n")) {
        error = "cannot write " + (root_ / "layout").string();
        return false;
    }
    return true;
}

bool State::migrate_v1(std::string& error) {
    const fs::path old_base = images_dir() / "calculinux.qcow2";
    const fs::path old_sha = images_dir() / "calculinux.qcow2.sha256";
    std::error_code ec;

    // Leftovers of an interrupted layout 1 download.
    fs::remove(images_dir() / "calculinux.qcow2.part", ec);
    fs::remove(images_dir() / "calculinux.qcow2.sha256.new", ec);

    std::string image_id;
    read_file(root_ / "image-id", image_id);
    bool overlay_is_local = image_id.rfind("local ", 0) == 0;

    // The overlay belongs to whichever base it was created on: an --image
    // FILE (AppRun keeps that state in local/) or the downloaded image.
    fs::path overlay_dir = overlay_is_local ? root_ / "local" : image_dir("legacy");
    bool have_overlay = false;
    for (const char* name : kOverlayFiles)
        if (fs::exists(root_ / name, ec)) have_overlay = true;

    if (fs::exists(old_base, ec)) {
        fs::create_directories(image_dir("legacy"), ec);
        std::string sha_text, sha;
        if (read_file(old_sha, sha_text)) sha = first_token(sha_text);
        if (sha.empty() && !sha256_file(old_base, sha)) {
            error = "cannot read " + old_base.string();
            return false;
        }
        if (!write_file_atomic(sha_path("legacy"), sha + "  base.qcow2\n")) {
            error = "cannot write " + sha_path("legacy").string();
            return false;
        }
        if (!move_if_exists(old_base, base_path("legacy"), error)) return false;
        fs::remove(old_sha, ec);
    }

    if (have_overlay) {
        fs::create_directories(overlay_dir, ec);
        for (const char* name : kOverlayFiles)
            if (!move_if_exists(root_ / name, overlay_dir / name, error)) return false;
    }

    if (is_downloaded("legacy")) {
        Settings settings = load_settings();
        if (settings.last_id.empty()) {
            settings.last_id = "legacy";
            save_settings(settings);
        }
    }
    return true;
}

std::string State::adopt_legacy(const Index& index) {
    std::string sha = installed_sha("legacy");
    if (sha.empty()) return "";
    for (const auto& e : index.images) {
        if (e.sha256 != sha) continue;
        std::error_code ec;
        if (fs::exists(image_dir(e.id), ec)) return "";
        fs::rename(image_dir("legacy"), image_dir(e.id), ec);
        if (ec) return "";
        ImageMeta meta;
        meta.id = e.id;
        meta.channel = e.channel;
        meta.label = e.label;
        meta.version = e.version;
        meta.url = e.url;
        meta.size = e.size;
        save_meta(meta);
        Settings settings = load_settings();
        if (settings.last_id == "legacy") {
            settings.last_id = e.id;
            save_settings(settings);
        }
        return e.id;
    }
    return "";
}

Settings State::load_settings() const {
    Settings settings;
    std::string text;
    if (!read_file(root_ / "settings", text)) return settings;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "last_id" && valid_id(value)) settings.last_id = value;
        if (key == "ssh_port") settings.ssh_port = value;
    }
    return settings;
}

bool State::save_settings(const Settings& settings) const {
    std::string text;
    if (!settings.last_id.empty()) text += "last_id=" + settings.last_id + "\n";
    if (!settings.ssh_port.empty()) text += "ssh_port=" + settings.ssh_port + "\n";
    return write_file_atomic(root_ / "settings", text);
}

bool State::is_downloaded(const std::string& id) const {
    std::error_code ec;
    return valid_id(id) && fs::is_regular_file(base_path(id), ec);
}

std::string State::installed_sha(const std::string& id) const {
    std::string text;
    if (!is_downloaded(id) || !read_file(sha_path(id), text)) return "";
    return first_token(text);
}

bool State::has_state(const std::string& id) const {
    std::error_code ec;
    return fs::exists(image_dir(id) / "disk.qcow2", ec) || fs::exists(image_dir(id) / "uboot-env.img", ec);
}

std::vector<std::string> State::downloaded_ids() const {
    std::vector<std::string> ids;
    std::error_code ec;
    for (fs::directory_iterator it(images_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        std::string id = it->path().filename().string();
        if (is_downloaded(id)) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool State::load_meta(const std::string& id, ImageMeta& meta) const {
    std::string text;
    if (!read_file(meta_path(id), text)) return false;
    json doc = json::parse(text, nullptr, false);
    if (!doc.is_object()) return false;
    auto str = [&](const char* key) {
        auto it = doc.find(key);
        return it != doc.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    meta.id = id;
    meta.channel = str("channel");
    meta.label = str("label");
    meta.version = str("version");
    meta.url = str("url");
    meta.downloaded_at = str("downloaded_at");
    auto size = doc.find("size");
    meta.size = size != doc.end() && size->is_number_unsigned() ? size->get<uint64_t>() : 0;
    return true;
}

bool State::save_meta(const ImageMeta& meta) const {
    json doc = {
        {"id", meta.id},           {"channel", meta.channel}, {"label", meta.label},
        {"version", meta.version}, {"url", meta.url},         {"size", meta.size},
        {"downloaded_at", meta.downloaded_at.empty() ? now_iso8601() : meta.downloaded_at},
    };
    return write_file_atomic(meta_path(meta.id), doc.dump(2) + "\n");
}

bool State::remove_image(const std::string& id, std::string& error) {
    if (!valid_id(id)) {
        error = "invalid image id";
        return false;
    }
    std::error_code ec;
    if (!fs::exists(image_dir(id), ec)) {
        error = "no such image: " + id;
        return false;
    }
    fs::remove_all(image_dir(id), ec);
    if (ec) {
        error = "cannot delete " + image_dir(id).string() + ": " + ec.message();
        return false;
    }
    Settings settings = load_settings();
    if (settings.last_id == id) {
        settings.last_id.clear();
        save_settings(settings);
    }
    return true;
}

bool State::reset_image(const std::string& id, std::string& error) {
    if (!valid_id(id)) {
        error = "invalid image id";
        return false;
    }
    for (const char* name : kOverlayFiles) {
        std::error_code ec;
        fs::remove(image_dir(id) / name, ec);
        if (ec) {
            error = "cannot delete " + (image_dir(id) / name).string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

}  // namespace cl
