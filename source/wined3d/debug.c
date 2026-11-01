/* Host wined3d (HOST_WINED3D=1): Wine's debug channels (ERR/FIXME/TRACE macros) on the Boxedwine log. */
#include <stdarg.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"
#include "wd3d_shim.h"

/* Classes on by default: err only. shortcut: no per-channel setting (Wine's WINEDEBUG); add one when debugging. */
static unsigned char default_flags = 1 << __WINE_DBCL_ERR;

unsigned char __cdecl __wine_dbg_get_channel_flags(struct __wine_debug_channel *channel)
{
    if (channel->flags & (1 << __WINE_DBCL_INIT)) channel->flags = default_flags;
    return channel->flags;
}

/* wine_dbg_sprintf results (debugstr_*) live until the ring wraps, as in Wine's per-thread buffer. */
const char * __cdecl __wine_dbg_strdup(const char *str)
{
    static char ring[16384];
    static size_t pos;
    size_t len = strlen(str) + 1;

    if (len > sizeof(ring)) return "...";
    if (pos + len > sizeof(ring)) pos = 0;
    memcpy(ring + pos, str, len);
    pos += len;
    return ring + pos - len;
}

/* One log line per message: the header and the formatted parts collect here until a '\n'. */
static char line[1024];
static size_t line_len;

static void line_add(const char *str)
{
    for (; *str; str++)
    {
        if (*str == '\n' || line_len == sizeof(line) - 1)
        {
            line[line_len] = 0;
            wd3d_log(line);
            line_len = 0;
            if (*str == '\n') continue;
        }
        line[line_len++] = *str;
    }
}

int __cdecl __wine_dbg_output(const char *str)
{
    line_add(str);
    return strlen(str);
}

int __cdecl __wine_dbg_header(enum __wine_debug_class cls, struct __wine_debug_channel *channel, const char *function)
{
    static const char *const classes[] = { "fixme", "err", "warn", "trace" };
    char header[128];

    if (!(__wine_dbg_get_channel_flags(channel) & (1 << cls))) return -1;
    snprintf(header, sizeof(header), "%s:%s:%s ", cls < ARRAY_SIZE(classes) ? classes[cls] : "?", channel->name,
             function ? function : "");
    return __wine_dbg_output(header);
}
