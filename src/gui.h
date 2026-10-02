// The launcher window.
#pragma once

#include "core/session.h"

#include <string>
#include <vector>

namespace cl {

// gui [--ssh-port N]: prints "id=<id>" and "ssh_port=<port>" and returns 0
// when the user boots an image, kExitClosed when the window is closed, and
// kExitNoDisplay when no window can be opened (or the launcher was built
// without the GUI).
int run_gui(Session& session, const std::vector<std::string>& args);

}  // namespace cl
