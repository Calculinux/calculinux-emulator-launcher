// Stands in for gui.cpp when the launcher is built without the window.
#include "cli.h"
#include "gui.h"

#include <cstdio>

namespace cl {

int run_gui(Session&, const std::vector<std::string>&) {
    fprintf(stderr, "calculinux-emulator: built without the launcher window\n");
    return kExitNoDisplay;
}

}  // namespace cl
