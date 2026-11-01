/* Host wined3d (HOST_WINED3D=1): kernel32/ntdll for a single-threaded host. wined3d runs with its command stream
 * on the calling thread (csmt=0, module.c), so locks, events and TLS need no real threads: critical sections only
 * count, events only flag, a wait that would block reports it and fails. */
/* Not __WINESRC__ here: that makes GetCurrentThreadId/GetLastError inline TEB reads, and this file provides the real
 * ones (for vkd3d, which isn't built with it). */
#undef __WINESRC__
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wd3d_shim.h"

double emscripten_get_now(void);

#define WD3D_THREAD_ID 1
#define WD3D_PROCESS_ID 1
#define WD3D_HEAP ((HANDLE)0x10000010)

/* Wine's headers read the TEB directly (GetCurrentThreadId, GetLastError, GetProcessHeap are inline). */
static PEB peb;
static TEB teb;

TEB * WINAPI NtCurrentTeb(void)
{
    if (!teb.Peb)
    {
        peb.ProcessHeap = WD3D_HEAP;
        teb.Peb = &peb;
        teb.ClientId.UniqueProcess = ULongToHandle(WD3D_PROCESS_ID);
        teb.ClientId.UniqueThread = ULongToHandle(WD3D_THREAD_ID);
        teb.Tib.Self = &teb.Tib;
    }
    return &teb;
}

/* vkd3d isn't built with __WINESRC__, so it calls the real functions. */
DWORD WINAPI GetCurrentThreadId(void)
{
    return WD3D_THREAD_ID;
}

DWORD WINAPI GetLastError(void)
{
    return NtCurrentTeb()->LastErrorValue;
}

void WINAPI SetLastError(DWORD error)
{
    NtCurrentTeb()->LastErrorValue = error;
}

/* Critical sections: one thread, so only the recursion count and owner matter (RtlIsCriticalSectionLockedByThread
 * is used in assertions). */
BOOL WINAPI InitializeCriticalSectionEx(CRITICAL_SECTION *cs, DWORD spin, DWORD flags)
{
    memset(cs, 0, sizeof(*cs));
    cs->LockCount = -1;
    return TRUE;
}

void WINAPI DeleteCriticalSection(CRITICAL_SECTION *cs)
{
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
}

void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs)
{
    cs->RecursionCount++;
    cs->OwningThread = ULongToHandle(WD3D_THREAD_ID);
}

void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs)
{
    if (cs->RecursionCount > 0 && !--cs->RecursionCount) cs->OwningThread = 0;
}

BOOL WINAPI RtlIsCriticalSectionLockedByThread(RTL_CRITICAL_SECTION *cs)
{
    return cs->RecursionCount > 0 && cs->OwningThread == ULongToHandle(WD3D_THREAD_ID);
}

/* Events */
#define WD3D_EVENT_MAGIC 0x45763344 /* "D3vE" */
struct wd3d_event
{
    DWORD magic;
    BOOL manual_reset, signaled;
};

HANDLE WINAPI CreateEventW(SECURITY_ATTRIBUTES *sa, BOOL manual_reset, BOOL initial_state, const WCHAR *name)
{
    struct wd3d_event *event;

    if (!(event = malloc(sizeof(*event))))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    event->magic = WD3D_EVENT_MAGIC;
    event->manual_reset = manual_reset;
    event->signaled = initial_state;
    return event;
}

static struct wd3d_event *get_event(HANDLE handle)
{
    struct wd3d_event *event = handle;
    return event && event->magic == WD3D_EVENT_MAGIC ? event : NULL;
}

BOOL WINAPI SetEvent(HANDLE handle)
{
    struct wd3d_event *event;

    if (!(event = get_event(handle)))
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    event->signaled = TRUE;
    return TRUE;
}

BOOL WINAPI CloseHandle(HANDLE handle)
{
    struct wd3d_event *event;

    if (!(event = get_event(handle)))
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    event->magic = 0;
    free(event);
    return TRUE;
}

/* TRUE when the wait is satisfied. Nothing else runs, so an unsignaled event stays unsignaled. */
static BOOL wait_event(HANDLE handle, BOOL zero_timeout)
{
    struct wd3d_event *event = get_event(handle);

    if (event && event->signaled)
    {
        if (!event->manual_reset) event->signaled = FALSE;
        return TRUE;
    }
    if (!zero_timeout) wd3d_log("wait on an unsignaled object would block forever (single-threaded host); failing it");
    return FALSE;
}

DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD timeout)
{
    if (wait_event(handle, !timeout)) return WAIT_OBJECT_0;
    if (!timeout) return WAIT_TIMEOUT;
    SetLastError(ERROR_NOT_SUPPORTED);
    return WAIT_FAILED;
}

NTSTATUS WINAPI NtWaitForSingleObject(HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout)
{
    BOOL zero_timeout = timeout && !timeout->QuadPart;

    if (wait_event(handle, zero_timeout)) return STATUS_SUCCESS;
    return zero_timeout ? STATUS_TIMEOUT : STATUS_NOT_SUPPORTED;
}

/* No sleeping on the host's one thread: returning lets the caller re-check. */
NTSTATUS WINAPI NtDelayExecution(BOOLEAN alertable, const LARGE_INTEGER *timeout)
{
    return STATUS_SUCCESS;
}

/* Threads: wined3d only starts one for the multithreaded command stream, which is off. */
HANDLE WINAPI CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, void *param,
        DWORD flags, DWORD *id)
{
    wd3d_log("CreateThread: no threads on the host (is csmt on?)");
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

void WINAPI FreeLibraryAndExitThread(HINSTANCE module, DWORD code)
{
    wd3d_log("FreeLibraryAndExitThread: reached without threads");
    abort();
}

HRESULT WINAPI SetThreadDescription(HANDLE thread, const WCHAR *description)
{
    return S_OK;
}

BOOL WINAPI DisableThreadLibraryCalls(HMODULE module)
{
    return TRUE;
}

/* TLS: one thread, so a slot is one value. */
#define WD3D_TLS_SLOTS 64
static BOOL tls_used[WD3D_TLS_SLOTS];
static void *tls_values[WD3D_TLS_SLOTS];

DWORD WINAPI TlsAlloc(void)
{
    DWORD i;

    for (i = 0; i < WD3D_TLS_SLOTS; i++)
    {
        if (tls_used[i]) continue;
        tls_used[i] = TRUE;
        tls_values[i] = NULL;
        return i;
    }
    SetLastError(ERROR_NO_MORE_ITEMS);
    return TLS_OUT_OF_INDEXES;
}

BOOL WINAPI TlsFree(DWORD index)
{
    if (index >= WD3D_TLS_SLOTS || !tls_used[index])
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    tls_used[index] = FALSE;
    return TRUE;
}

void * WINAPI TlsGetValue(DWORD index)
{
    if (index >= WD3D_TLS_SLOTS || !tls_used[index])
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    SetLastError(ERROR_SUCCESS);
    return tls_values[index];
}

BOOL WINAPI TlsSetValue(DWORD index, void *value)
{
    if (index >= WD3D_TLS_SLOTS || !tls_used[index])
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    tls_values[index] = value;
    return TRUE;
}

/* Heaps: libc malloc (cs.c's deferred upload heap, and GetProcessHeap's handle). */
HANDLE WINAPI HeapCreate(DWORD flags, SIZE_T initial, SIZE_T max)
{
    return WD3D_HEAP;
}

BOOL WINAPI HeapDestroy(HANDLE heap)
{
    /* shortcut: blocks still allocated from a destroyed heap leak; cs.c frees its uploads first. */
    return TRUE;
}

void * WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    return flags & HEAP_ZERO_MEMORY ? calloc(1, size) : malloc(size);
}

BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, void *ptr)
{
    free(ptr);
    return TRUE;
}

/* Time */
DWORD WINAPI GetTickCount(void)
{
    return (DWORD)(ULONGLONG)emscripten_get_now();
}

ULONGLONG WINAPI GetTickCount64(void)
{
    return (ULONGLONG)emscripten_get_now();
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
    frequency->QuadPart = 1000000;
    return TRUE;
}

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *counter)
{
    counter->QuadPart = (LONGLONG)(emscripten_get_now() * 1000.0);
    return TRUE;
}

/* System information: what Windows 10 on a 4 GB machine reports. */
BOOL WINAPI GetVersionExW(OSVERSIONINFOW *info)
{
    if (info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOW) && info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXW))
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    info->dwMajorVersion = 10;
    info->dwMinorVersion = 0;
    info->dwBuildNumber = 19045;
    info->dwPlatformId = VER_PLATFORM_WIN32_NT;
    info->szCSDVersion[0] = 0;
    if (info->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXW))
    {
        OSVERSIONINFOEXW *ex = (OSVERSIONINFOEXW *)info;
        ex->wServicePackMajor = ex->wServicePackMinor = 0;
        ex->wSuiteMask = 0;
        ex->wProductType = VER_NT_WORKSTATION;
        ex->wReserved = 0;
    }
    return TRUE;
}

BOOL WINAPI GlobalMemoryStatusEx(MEMORYSTATUSEX *status)
{
    if (status->dwLength != sizeof(*status))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    status->dwMemoryLoad = 50;
    status->ullTotalPhys = (ULONGLONG)4 << 30;
    status->ullAvailPhys = (ULONGLONG)2 << 30;
    status->ullTotalPageFile = (ULONGLONG)8 << 30;
    status->ullAvailPageFile = (ULONGLONG)4 << 30;
    status->ullTotalVirtual = (ULONGLONG)2 << 30;
    status->ullAvailVirtual = (ULONGLONG)1 << 30;
    status->ullAvailExtendedVirtual = 0;
    return TRUE;
}

BOOL WINAPI AllocateLocallyUniqueId(LUID *luid)
{
    static LONG next = 0x1000;

    luid->LowPart = next++;
    luid->HighPart = 0;
    return TRUE;
}

/* Misc */
void WINAPI DebugBreak(void)
{
    wd3d_log("DebugBreak");
}

BOOL WINAPI IsBadStringPtrA(const char *str, UINT_PTR max)
{
    return !str;
}

BOOL WINAPI IsBadStringPtrW(const WCHAR *str, UINT_PTR max)
{
    return !str;
}

/* shortcut: case folding for ASCII only; wined3d compares module and driver names. */
int WINAPI lstrcmpiW(const WCHAR *a, const WCHAR *b)
{
    WCHAR ca, cb;

    if (!a || !b) return a ? 1 : b ? -1 : 0;
    do
    {
        ca = *a++;
        cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
    } while (ca && ca == cb);
    return ca < cb ? -1 : ca > cb ? 1 : 0;
}
