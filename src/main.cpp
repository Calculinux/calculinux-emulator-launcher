// calculinux-emulator-launcher: choose, download and manage the system
// images of the Calculinux emulator. Run by the emulator AppImage's AppRun,
// which boots the image whose id this prints.

#include "cli.h"
#include "core/session.h"
#include "gui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef LAUNCHER_VERSION
#define LAUNCHER_VERSION "dev"
#endif

namespace {

const char* const kDefaultIndexUrl = "https://opkg.calculinux.org/emulator/calculinux-qemuarm/index.json";

void usage(FILE* out) {
    fprintf(out,
            "calculinux-emulator-launcher %s\n"
            "\n"
            "Usage: calculinux-emulator-launcher [options] <command>\n"
            "\n"
            "Options:\n"
            "  --state DIR         state directory (default $CALCULINUX_EMULATOR_HOME,\n"
            "                      else ~/.local/share/calculinux-emulator)\n"
            "  --index-url URL     image index (default $CALCULINUX_EMULATOR_INDEX_URL,\n"
            "                      else %s)\n"
            "  --default-id ID     image to preselect when none was used before\n"
            "  --fallback-url URL  image to offer when the index cannot be fetched\n"
            "  --app-version STR   emulator version, shown in the window title\n"
            "  --ca-bundle FILE    CA certificates to use if the host has none\n"
            "\n"
            "Commands:\n"
            "  gui [--ssh-port N]            choose an image in a window\n"
            "  fetch [ID|--auto|--url URL] [--update]\n"
            "                                make sure an image is downloaded\n"
            "  list [--json] [--offline]     show images and their status\n"
            "  remove ID                     delete an image and its saved state\n"
            "  reset ID                      delete an image's saved state\n"
            "  migrate                       convert an old state directory\n"
            "\n"
            "gui and fetch print \"id=<image id>\" on success. Exit codes: 0 ok, 1 error,\n"
            "2 usage, 3 image unavailable, 4 network, 5 checksum, 6 disk space,\n"
            "10 window closed, 20 no display.\n",
            LAUNCHER_VERSION, kDefaultIndexUrl);
}

std::filesystem::path default_state_dir() {
    if (const char* home = getenv("CALCULINUX_EMULATOR_HOME"); home && *home) return home;
    if (const char* data = getenv("XDG_DATA_HOME"); data && *data)
        return std::filesystem::path(data) / "calculinux-emulator";
    const char* home = getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".local/share/calculinux-emulator";
}

}  // namespace

int main(int argc, char** argv) {
    cl::Config config;
    config.state_dir = default_state_dir();
    config.app_version = LAUNCHER_VERSION;
    if (const char* url = getenv("CALCULINUX_EMULATOR_INDEX_URL"); url && *url) config.index_url = url;
    else config.index_url = kDefaultIndexUrl;

    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        std::string opt = argv[i];
        if (opt == "--help" || opt == "-h") {
            usage(stdout);
            return cl::kExitOk;
        }
        if (opt == "--version") {
            printf("%s\n", LAUNCHER_VERSION);
            return cl::kExitOk;
        }
        if (i + 1 >= argc) {
            usage(stderr);
            return cl::kExitUsage;
        }
        std::string value = argv[++i];
        if (opt == "--state") config.state_dir = value;
        else if (opt == "--index-url") config.index_url = value;
        else if (opt == "--default-id") config.default_id = value;
        else if (opt == "--fallback-url") config.fallback_url = value;
        else if (opt == "--app-version") config.app_version = value;
        else if (opt == "--ca-bundle") config.ca_bundle = value;
        else {
            fprintf(stderr, "calculinux-emulator: unknown option %s\n", opt.c_str());
            return cl::kExitUsage;
        }
    }
    if (i >= argc) {
        usage(stderr);
        return cl::kExitUsage;
    }
    std::string command = argv[i++];
    std::vector<std::string> args(argv + i, argv + argc);

    cl::http_global_init();
    cl::Session session(config);
    std::string error;
    if (!session.state.prepare(error)) {
        fprintf(stderr, "calculinux-emulator: %s\n", error.c_str());
        return cl::kExitError;
    }

    if (command == "gui") return cl::run_gui(session, args);
    if (command == "fetch") return cl::cmd_fetch(session, args);
    if (command == "list") return cl::cmd_list(session, args);
    if (command == "remove") return cl::cmd_remove(session, args);
    if (command == "reset") return cl::cmd_reset(session, args);
    if (command == "migrate") return cl::kExitOk;  // prepare() did it

    fprintf(stderr, "calculinux-emulator: unknown command %s\n", command.c_str());
    return cl::kExitUsage;
}
