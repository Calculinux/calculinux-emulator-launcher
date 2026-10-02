#include "cli.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace cl {

namespace {

void say(const std::string& message) { fprintf(stderr, "calculinux-emulator: %s\n", message.c_str()); }

// Run the download on a worker and report progress on stderr: one updating
// line on a terminal, a line every 10% otherwise.
FetchResult fetch_with_progress(Session& session, const IndexEntry& entry) {
    FetchProgress progress;
    FetchResult result;
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        result = fetch_image(session.state, entry, progress, session.http);
        finished = true;
    });

    bool tty = isatty(STDERR_FILENO);
    int last_step = -1;
    FetchPhase last_phase = FetchPhase::Idle;
    bool line_open = false;
    while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        FetchPhase phase = progress.phase;
        uint64_t now = progress.transfer.now, total = progress.transfer.total;
        if (phase != FetchPhase::Downloading && phase != FetchPhase::Verifying) continue;
        const char* what = phase == FetchPhase::Downloading ? "Downloading" : "Verifying";
        int percent = total ? (int)(now * 100 / total) : 0;
        if (tty) {
            fprintf(stderr, "\r%s %3d%%  %s / %s   ", what, percent, human_size(now).c_str(),
                    total ? human_size(total).c_str() : "?");
            line_open = true;
        } else if (phase != last_phase || percent / 10 != last_step) {
            fprintf(stderr, "%s %d%%\n", what, percent);
            last_step = percent / 10;
        }
        last_phase = phase;
    }
    worker.join();
    if (line_open) fputc('\n', stderr);
    return result;
}

const CatalogRow* find_row(const std::vector<CatalogRow>& rows, const std::string& id) {
    for (const auto& row : rows)
        if (row.entry.id == id) return &row;
    return nullptr;
}

// Whether the image downloaded from an explicit URL came from this URL.
bool custom_current(Session& session, const std::string& url) {
    ImageMeta meta;
    return session.state.is_downloaded(kCustomId) && session.state.load_meta(kCustomId, meta) && meta.url == url;
}

int finish_fetch(Session& session, const std::string& id) {
    Settings settings = session.state.load_settings();
    // An explicit URL is a one-off; it does not become the remembered choice.
    if (id != kCustomId && settings.last_id != id) {
        settings.last_id = id;
        session.state.save_settings(settings);
    }
    printf("id=%s\n", id.c_str());
    return kExitOk;
}

}  // namespace

int exit_code_for(FetchError error) {
    switch (error) {
        case FetchError::None: return kExitOk;
        case FetchError::Network: return kExitNetwork;
        case FetchError::Checksum: return kExitChecksum;
        case FetchError::DiskSpace: return kExitDiskSpace;
        case FetchError::Cancelled:
        case FetchError::Other: return kExitError;
    }
    return kExitError;
}

// fetch [ID | --auto | --url URL] [--update]
int cmd_fetch(Session& session, const std::vector<std::string>& args) {
    std::string id, url;
    bool update = false;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--update") update = true;
        else if (args[i] == "--auto") id.clear();
        else if (args[i] == "--url" && i + 1 < args.size()) url = args[++i];
        else if (!args[i].empty() && args[i][0] != '-') id = args[i];
        else {
            say("fetch: unknown option " + args[i]);
            return kExitUsage;
        }
    }

    // An explicit URL is used as given: no index, nothing to choose.
    if (!url.empty()) {
        if (custom_current(session, url) && !update) return finish_fetch(session, kCustomId);
        IndexEntry entry;
        entry.id = kCustomId;
        entry.label = "Custom image";
        entry.url = url;
        say("downloading " + url);
        FetchResult result = fetch_with_progress(session, entry);
        if (!result.ok()) {
            // --update with the server unreachable: keep what we have.
            if (custom_current(session, url) && result.error == FetchError::Network) {
                say(result.message);
                say("keeping the image already downloaded");
                return finish_fetch(session, kCustomId);
            }
            say(result.message);
            return exit_code_for(result.error);
        }
        if (result.replaced) say("system image changed; saved state was reset");
        return finish_fetch(session, kCustomId);
    }

    // The last image booted needs no network unless an update was asked for.
    Settings settings = session.state.load_settings();
    if (id.empty() && !update && session.state.is_downloaded(settings.last_id))
        return finish_fetch(session, settings.last_id);
    if (!id.empty() && !update && session.state.is_downloaded(id)) return finish_fetch(session, id);

    session.refresh();
    if (!session.online) say("cannot fetch the image index: " + session.index_error);
    std::vector<CatalogRow> rows = session.rows();

    const CatalogRow* row = nullptr;
    if (!id.empty()) {
        row = find_row(rows, id);
    } else {
        int pick = pick_default(rows, settings.last_id, session.config.default_id, session.index.latest_release);
        if (pick >= 0) row = &rows[pick];
    }
    if (!row) {
        say(id.empty() ? "no image available" : "no such image: " + id + " (see --list)");
        return kExitUnavailable;
    }
    id = row->entry.id;

    bool current = row->downloaded() && !(update && row->status == ImageStatus::UpdateAvailable);
    if (current) {
        if (update) say("image is up to date");
        return finish_fetch(session, id);
    }

    say("downloading " + row->entry.label + " (" + row->entry.url + ")");
    FetchResult result = fetch_with_progress(session, row->entry);
    if (!result.ok()) {
        say(result.message);
        if (row->downloaded()) {
            say("keeping the image already downloaded");
            return finish_fetch(session, id);
        }
        return exit_code_for(result.error);
    }
    if (result.replaced) say("system image changed; saved state was reset");
    return finish_fetch(session, id);
}

// list [--json] [--offline]
int cmd_list(Session& session, const std::vector<std::string>& args) {
    bool as_json = false, offline = false;
    for (const auto& arg : args) {
        if (arg == "--json") as_json = true;
        else if (arg == "--offline") offline = true;
        else {
            say("list: unknown option " + arg);
            return kExitUsage;
        }
    }
    if (offline) {
        session.load_cached();
    } else {
        session.refresh();
        if (!session.online) say("cannot fetch the image index: " + session.index_error);
    }
    std::vector<CatalogRow> rows = session.rows();

    if (as_json) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& row : rows)
            out.push_back({
                {"id", row.entry.id},
                {"label", row.entry.label},
                {"channel", row.entry.channel},
                {"version", row.entry.version},
                {"size", row.entry.size},
                {"url", row.entry.url},
                {"status", status_text(row.status)},
                {"downloaded", row.downloaded()},
                {"has_state", row.has_state},
            });
        printf("%s\n", out.dump(2).c_str());
        return kExitOk;
    }

    if (rows.empty()) {
        say(session.online ? "no images published" : "offline and no images downloaded");
        return kExitOk;
    }
    printf("%-22s %-20s %-30s %9s\n", "ID", "STATUS", "VERSION", "SIZE");
    for (const auto& row : rows)
        printf("%-22s %-20s %-30s %9s\n", row.entry.id.c_str(), status_text(row.status),
               row.entry.version.empty() ? "-" : row.entry.version.c_str(),
               row.entry.size ? human_size(row.entry.size).c_str() : "-");
    return kExitOk;
}

int cmd_remove(Session& session, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        say("remove: expected one image id");
        return kExitUsage;
    }
    std::string error;
    if (!session.state.remove_image(args[0], error)) {
        say(error);
        return kExitUnavailable;
    }
    say("removed " + args[0]);
    return kExitOk;
}

int cmd_reset(Session& session, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        say("reset: expected one image id");
        return kExitUsage;
    }
    std::string error;
    if (!session.state.is_downloaded(args[0])) {
        say("no such image: " + args[0]);
        return kExitUnavailable;
    }
    if (!session.state.reset_image(args[0], error)) {
        say(error);
        return kExitError;
    }
    say("saved state of " + args[0] + " was reset");
    return kExitOk;
}

}  // namespace cl
