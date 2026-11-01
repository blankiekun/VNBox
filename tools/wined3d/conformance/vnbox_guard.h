/* VNBox conformance guard (tools/wined3d/conformance/patch_tests.py inserts it into Wine's d3d11.c / dxgi.c).
 * Every test function runs inside VNBOX_CALL: "### BEGIN <idx> <name>" before it, "### END <idx> <name> ok= fail=
 * todo= skip=" after it (deltas of the winetest counters). A crash (unhandled exception) prints "### CRASH" and
 * the run goes on with the next function (longjmp out of the top-level filter); a function that runs longer than
 * --hang seconds (default 600) prints "### HANG" and ends the process; "### EXIT" (patched into wine/test.h)
 * follows winetest's summary line of a run that finished. --from N / --to N run only those indices
 * (to restart after a hang); --skip name,name@fl skips functions (one that kills the emulator). Output is unbuffered so nothing is lost when the process dies. */
#include <setjmp.h>
#include <io.h>

static jmp_buf vnbox_jb;
static volatile LONG vnbox_active;
static volatile DWORD vnbox_start_tick;
static unsigned int vnbox_idx, vnbox_from, vnbox_to = ~0u, vnbox_hang = 600;
static const char *vnbox_names[4096];
static const char *vnbox_qname;
static char vnbox_cur[160];
static LONG vnbox_c0[5];
static const char *vnbox_skip = ""; /* ",name,name@fl," */

static LONG WINAPI vnbox_filter(EXCEPTION_POINTERS *p)
{
    if (!vnbox_active || GetCurrentThreadId() != vnbox_active) return EXCEPTION_CONTINUE_SEARCH;
    printf("### CRASH %u %s code %08lx at %p\n", vnbox_idx, vnbox_cur,
            p->ExceptionRecord->ExceptionCode, p->ExceptionRecord->ExceptionAddress);
    fflush(stdout);
    vnbox_active = 0;
    longjmp(vnbox_jb, 1);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI vnbox_watchdog(void *arg)
{
    for (;;)
    {
        Sleep(2000);
        if (vnbox_active && GetTickCount() - vnbox_start_tick > vnbox_hang * 1000u)
        {
            printf("### HANG %u %s after %us\n", vnbox_idx, vnbox_cur, vnbox_hang);
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    return 0;
}

static void vnbox_init(void)
{
    char **argv;
    int argc = winetest_get_mainargs(&argv), i;

    HANDLE tty;

    /* BoxedWine's stdout is /dev/tty0, so Wine draws the console through conhost (escape codes, 80-column wraps):
     * write to the device directly instead, plain lines. */
    tty = CreateFileA("Z:\\dev\\tty0", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (tty != INVALID_HANDLE_VALUE)
    {
        int fd = _open_osfhandle((intptr_t)tty, 0);
        _dup2(fd, 1);
        SetStdHandle(STD_OUTPUT_HANDLE, tty);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && strncmp(argv[2], "--", 2)) return; /* a child process (d3d11's test_create_device_child) */
    for (i = 2; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--from") && i + 1 < argc) vnbox_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) vnbox_to = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hang") && i + 1 < argc) vnbox_hang = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc)
        {
            static char list[4096];
            snprintf(list, sizeof(list), ",%s,", argv[++i]);
            vnbox_skip = list;
        }
    }
    SetUnhandledExceptionFilter(vnbox_filter);
    CloseHandle(CreateThread(NULL, 0, vnbox_watchdog, NULL, 0, NULL));
    printf("### START from %u to %u hang %u\n", vnbox_from, vnbox_to, vnbox_hang);
}

static void vnbox_counts(LONG *c)
{
    c[0] = winetest_successes; c[1] = winetest_failures; c[2] = winetest_todo_failures;
    c[3] = winetest_todo_successes; c[4] = winetest_skipped;
}

/* --skip a,b: a test function by name (every feature level) or name@fl (one), e.g. one that kills the emulator */
static BOOL vnbox_skipped(const char *name)
{
    char key[170];

    snprintf(key, sizeof(key), ",%s,", vnbox_cur);
    if (strstr(vnbox_skip, key)) return TRUE;
    snprintf(key, sizeof(key), ",%s,", name ? name : "?");
    return strstr(vnbox_skip, key) != NULL;
}

static BOOL vnbox_begin(const char *name, unsigned int fl)
{
    unsigned int idx = vnbox_idx;

    if (idx < vnbox_from || idx > vnbox_to) { ++vnbox_idx; return FALSE; }
    if (fl) snprintf(vnbox_cur, sizeof(vnbox_cur), "%s@%x", name ? name : "?", fl);
    else snprintf(vnbox_cur, sizeof(vnbox_cur), "%s", name ? name : "?");
    if (vnbox_skipped(name))
    {
        printf("### SKIP %u %s\n", idx, vnbox_cur);
        ++vnbox_idx;
        return FALSE;
    }
    printf("### BEGIN %u %s\n", idx, vnbox_cur);
    vnbox_counts(vnbox_c0);
    vnbox_start_tick = GetTickCount();
    vnbox_active = GetCurrentThreadId();
    return TRUE;
}

static void vnbox_end(void)
{
    LONG c[5];

    vnbox_active = 0;
    winetest_get_thread_data()->context_count = 0; /* a crash skipped winetest_pop_context */
    vnbox_counts(c);
    printf("### END %u %s ok=%ld fail=%ld todofail=%ld todo=%ld skip=%ld ms=%lu\n", vnbox_idx, vnbox_cur,
            c[0] - vnbox_c0[0], c[1] - vnbox_c0[1], c[2] - vnbox_c0[2], c[3] - vnbox_c0[3], c[4] - vnbox_c0[4],
            GetTickCount() - vnbox_start_tick);
    ++vnbox_idx;
}

#define VNBOX_CALL(name, fl, stmt) do { if (vnbox_begin(name, fl)) { if (!setjmp(vnbox_jb)) { stmt; } vnbox_end(); } } while (0)
