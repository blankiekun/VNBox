// Host wined3d (HOST_WINED3D=1): the shims' line to the Boxedwine log.
#include "boxedwine.h"
#include <stdarg.h>

extern "C" void wd3d_log(const char* text) {
    klog_fmt("[wined3d] %s", text);
}

void wd3d_log_fmt(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    wd3d_log(line);
}
