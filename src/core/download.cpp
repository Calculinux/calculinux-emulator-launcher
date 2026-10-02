#include "download.h"

#include "sha256.h"

#include <sys/statvfs.h>

namespace cl {

namespace {

// Room kept free beyond the download itself (the overlay and U-Boot
// environment are created right after).
const uint64_t kSpaceMargin = 64ull * 1024 * 1024;

bool is_sha256(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

uint64_t size_of(const fs::path& path) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    return ec ? 0 : (uint64_t)size;
}

FetchResult fail(FetchError error, std::string message) {
    FetchResult result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

}  // namespace

FetchResult fetch_image(State& state, const IndexEntry& entry, FetchProgress& progress,
                        const HttpOptions& options) {
    const std::string& id = entry.id;
    if (!valid_id(id)) return fail(FetchError::Other, "invalid image id");
    Progress& transfer = progress.transfer;
    transfer.now = 0;
    transfer.total = entry.size;

    // What the server has right now.
    progress.phase = FetchPhase::Checking;
    std::string want = entry.sha256;
    uint64_t size = entry.size;
    std::string sidecar;
    HttpResult sha_result = http_get(entry.url + ".sha256", sidecar, options);
    if (sha_result.ok && is_sha256(first_token(sidecar))) {
        if (first_token(sidecar) != want) size = 0;  // the index is stale; its size may be too
        want = first_token(sidecar);
    } else if (!is_sha256(want)) {
        return fail(FetchError::Network, "cannot fetch " + entry.url + ".sha256: " + sha_result.error);
    }
    if (transfer.cancel.load()) return fail(FetchError::Cancelled, "cancelled");

    if (state.installed_sha(id) == want) return FetchResult();

    std::error_code ec;
    fs::create_directories(state.image_dir(id), ec);
    if (ec) return fail(FetchError::Other, "cannot create " + state.image_dir(id).string() + ": " + ec.message());

    // Resume a partial download of this same image, discard any other.
    const fs::path part = state.part_path(id), part_meta = state.part_meta_path(id);
    std::string part_sha;
    read_file(part_meta, part_sha);
    uint64_t have = first_token(part_sha) == want ? size_of(part) : 0;
    if (have == 0) {
        fs::remove(part, ec);
        if (!write_file_atomic(part_meta, want + "\n"))
            return fail(FetchError::Other, "cannot write " + part_meta.string());
    }

    bool complete = size > 0 && have >= size;
    if (!complete) {
        if (size > have) {
            struct statvfs vfs;
            if (statvfs(state.image_dir(id).c_str(), &vfs) == 0) {
                uint64_t free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
                if (free_bytes < size - have + kSpaceMargin)
                    return fail(FetchError::DiskSpace,
                                "not enough disk space: " + human_size(size - have + kSpaceMargin) +
                                    " needed, " + human_size(free_bytes) + " free in " + state.root().string());
            }
        }

        progress.phase = FetchPhase::Downloading;
        transfer.total = size;
        HttpResult result = http_download(entry.url, part, have, transfer, options);
        if (result.range_unsupported) {
            // The server ignored the range; start over.
            have = 0;
            result = http_download(entry.url, part, 0, transfer, options);
        }
        if (result.cancelled) return fail(FetchError::Cancelled, "cancelled");
        // 416: the partial file already holds everything; let the checksum decide.
        if (!result.ok && !(have > 0 && result.status == 416))
            return fail(FetchError::Network, "download failed: " + result.error);
    }

    progress.phase = FetchPhase::Verifying;
    transfer.now = 0;
    transfer.total = size_of(part);
    std::string got;
    if (!sha256_file(part, got, &transfer.now, &transfer.cancel)) {
        if (transfer.cancel.load()) return fail(FetchError::Cancelled, "cancelled");
        return fail(FetchError::Other, "cannot read " + part.string());
    }
    if (got != want) {
        fs::remove(part, ec);
        fs::remove(part_meta, ec);
        return fail(FetchError::Checksum, "checksum mismatch for the downloaded image");
    }

    // Install. The overlay and U-Boot environment of an older base do not
    // fit the new one.
    FetchResult done;
    if (state.is_downloaded(id)) {
        std::string error;
        done.replaced = state.has_state(id);
        if (!state.reset_image(id, error)) return fail(FetchError::Other, error);
    }
    fs::rename(part, state.base_path(id), ec);
    if (ec) return fail(FetchError::Other, "cannot install the image: " + ec.message());
    fs::remove(part_meta, ec);
    if (!write_file_atomic(state.sha_path(id), want + "  base.qcow2\n"))
        return fail(FetchError::Other, "cannot write " + state.sha_path(id).string());

    ImageMeta meta;
    meta.id = id;
    meta.channel = entry.channel;
    meta.label = entry.label;
    meta.version = want == entry.sha256 ? entry.version : "";
    meta.url = entry.url;
    meta.size = size_of(state.base_path(id));
    state.save_meta(meta);
    return done;
}

}  // namespace cl
