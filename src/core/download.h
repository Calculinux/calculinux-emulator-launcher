// Download one published image into the state directory.
#pragma once

#include "http.h"
#include "index.h"
#include "state.h"

#include <atomic>

namespace cl {

enum class FetchPhase { Idle, Checking, Downloading, Verifying };

struct FetchProgress {
    Progress transfer;  // bytes while Downloading and Verifying; cancel flag
    std::atomic<FetchPhase> phase{FetchPhase::Idle};
};

enum class FetchError { None, Network, Checksum, DiskSpace, Cancelled, Other };

struct FetchResult {
    FetchError error = FetchError::None;
    std::string message;
    bool replaced = false;  // an older base image and its overlay were discarded
    bool ok() const { return error == FetchError::None; }
};

// Make images/<entry.id>/base.qcow2 the image entry describes: download to
// base.qcow2.part (resuming a matching partial download), verify the sha256,
// then move it into place. An already downloaded older image is replaced and
// its overlay and U-Boot environment deleted, since they only fit the old
// base. Does nothing if the image is already current.
//
// The expected sha256 is read from <url>.sha256, which the server updates
// together with the image; entry.sha256 is the fallback.
FetchResult fetch_image(State& state, const IndexEntry& entry, FetchProgress& progress,
                        const HttpOptions& options);

}  // namespace cl
