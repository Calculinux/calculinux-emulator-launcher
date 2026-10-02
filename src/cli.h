// Headless commands and the exit codes AppRun acts on.
#pragma once

#include "core/download.h"
#include "core/session.h"

#include <string>
#include <vector>

namespace cl {

enum ExitCode {
    kExitOk = 0,
    kExitError = 1,
    kExitUsage = 2,
    kExitUnavailable = 3,  // unknown image, or not downloaded and offline
    kExitNetwork = 4,
    kExitChecksum = 5,
    kExitDiskSpace = 6,
    kExitClosed = 10,     // gui: window closed without choosing an image
    kExitNoDisplay = 20,  // gui: could not open a window
};

int exit_code_for(FetchError error);

int cmd_fetch(Session& session, const std::vector<std::string>& args);
int cmd_list(Session& session, const std::vector<std::string>& args);
int cmd_remove(Session& session, const std::vector<std::string>& args);
int cmd_reset(Session& session, const std::vector<std::string>& args);

}  // namespace cl
