
#include <stdbool.h>
#include <string.h>

#include "config.h"

int flutterpi_app_main(int argc, char **argv);
int crashpad_handler_main(int argc, char **argv);

#ifdef HAVE_BUNDLED_CRASHPAD_HANDLER
static bool running_in_crashpad_mode(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--attachment=FlutterpiCrashpadHandlerMode") == 0) {
            return true;
        }
    }

    return false;
}
#endif

int vrx_compositor_main(int argc, char **argv);

static bool running_in_compositor_mode(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--compositor") == 0) {
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv) {
#ifdef HAVE_BUNDLED_CRASHPAD_HANDLER
    if (running_in_crashpad_mode(argc, argv)) {
        return crashpad_handler_main(argc, argv);
    }
    if (running_in_compositor_mode(argc, argv)) {
        return vrx_compositor_main(argc, argv);
    }
#endif

    return flutterpi_app_main(argc, argv);
}
