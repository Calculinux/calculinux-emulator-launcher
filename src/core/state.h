// The emulator's state directory (layout 2):
//
//   layout                  "2"
//   settings                last_id=..., ssh_port=...
//   index.json              last index fetched, for labels when offline
//   images/<id>/
//     base.qcow2  base.qcow2.sha256  meta.json
//     base.qcow2.part  base.qcow2.part.meta     (download in progress)
//     disk.qcow2  uboot-env.img  image-id       (written by AppRun)
//   local/                  AppRun's state for --image FILE; not ours
//
// The launcher owns everything under images/ except the three AppRun files,
// which it only ever deletes (reset, update, remove).
#pragma once

#include "index.h"
#include "util.h"

#include <string>
#include <vector>

namespace cl {

struct Settings {
    std::string last_id;
    std::string ssh_port;
};

// What meta.json remembers about a downloaded image, so it can still be
// shown once it is gone from the index or the index is unreachable.
struct ImageMeta {
    std::string id, channel, label, version, url, downloaded_at;
    uint64_t size = 0;
};

class State {
public:
    explicit State(fs::path root) : root_(std::move(root)) {}

    const fs::path& root() const { return root_; }
    fs::path images_dir() const { return root_ / "images"; }
    fs::path image_dir(const std::string& id) const { return images_dir() / id; }
    fs::path base_path(const std::string& id) const { return image_dir(id) / "base.qcow2"; }
    fs::path sha_path(const std::string& id) const { return image_dir(id) / "base.qcow2.sha256"; }
    fs::path meta_path(const std::string& id) const { return image_dir(id) / "meta.json"; }
    fs::path part_path(const std::string& id) const { return image_dir(id) / "base.qcow2.part"; }
    fs::path part_meta_path(const std::string& id) const { return image_dir(id) / "base.qcow2.part.meta"; }
    fs::path cached_index_path() const { return root_ / "index.json"; }

    // Create the directory and bring a layout 1 state directory (one image,
    // one overlay, in the top level) up to layout 2. Safe to call every run.
    bool prepare(std::string& error);

    // Layout 1's image becomes images/legacy; once the index shows which
    // published image it is, give it that id. Returns the new id, or "".
    std::string adopt_legacy(const Index& index);

    Settings load_settings() const;
    bool save_settings(const Settings& settings) const;

    bool is_downloaded(const std::string& id) const;
    // sha256 of the downloaded base image, "" if there is none.
    std::string installed_sha(const std::string& id) const;
    // Whether the image has an overlay or U-Boot environment (user changes).
    bool has_state(const std::string& id) const;
    // Size of an unfinished download, 0 if there is none.
    uint64_t partial_size(const std::string& id) const;
    // Ids with a downloaded base image, sorted.
    std::vector<std::string> downloaded_ids() const;

    bool load_meta(const std::string& id, ImageMeta& meta) const;
    bool save_meta(const ImageMeta& meta) const;

    // Delete the image and everything belonging to it.
    bool remove_image(const std::string& id, std::string& error);
    // Delete only the overlay and U-Boot environment.
    bool reset_image(const std::string& id, std::string& error);

private:
    bool migrate_v1(std::string& error);
    fs::path root_;
};

}  // namespace cl
