/* Stand-in for wine/debug.h: Wine's debug channels are off in the host copy of the primitives. */
#define WINE_DEFAULT_DEBUG_CHANNEL(ch)
#define TRACE(...) do {} while (0)
#define FIXME(...) do {} while (0)
#define ERR(...) do {} while (0)
#define WARN(...) do {} while (0)
