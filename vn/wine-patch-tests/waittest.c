/* Checks vn/wine/ntdll-wait-cost.patch (cheaper yields, sleeps and timed-out waits, SleepEx through the ntsync alert).
 * Run by hand like ggotest: every line must say ok, and it must end with fails=0.
 * Bugs it catches: an APC that does not run or does not end an alertable sleep (a game's audio or timer thread hangs for
 * good); a timed sleep or wait that returns early or late or with the wrong value. Timing limits are wide on purpose:
 * this is correctness, not speed. Prints to stdout (the player console in VNBox).
 * Build: i686-w64-mingw32-gcc -O2 -o waittest.exe waittest.c -lwinmm */
#include <windows.h>
#include <stdio.h>

static volatile LONG apcs;
static int fails;

static void CALLBACK apc( ULONG_PTR arg )
{
    InterlockedIncrement( &apcs );
}

static void check( int ok, const char *what, DWORD got )
{
    printf( "[wait] %s %s (%lu)\n", ok ? "ok" : "FAIL", what, got );
    fflush( stdout );
    if (!ok) fails++;
}

static void self_apc( DWORD timeout, const char *what )
{
    DWORD ret;
    LONG before = apcs;

    QueueUserAPC( apc, GetCurrentThread(), 0 );
    ret = SleepEx( timeout, TRUE );
    check( ret == WAIT_IO_COMPLETION && apcs == before + 1, what, ret );
}

static void timed( DWORD timeout, BOOL alertable, const char *what )
{
    DWORD start = timeGetTime(), ret = SleepEx( timeout, alertable ), took = timeGetTime() - start;

    check( ret == 0 && took + 2 >= timeout && took < timeout + 2000, what, took );
}

static DWORD WINAPI sleeper( void *arg )
{
    DWORD ret = SleepEx( INFINITE, TRUE );
    *(DWORD *)arg = ret;
    return 0;
}

static void other_thread(void)
{
    DWORD result = 12345, ret;
    HANDLE thread = CreateThread( NULL, 0, sleeper, &result, 0, NULL );
    LONG before = apcs;

    Sleep( 100 );  /* it is asleep by now, or the APC waits for it: both must end the sleep */
    QueueUserAPC( apc, thread, 0 );
    ret = WaitForSingleObject( thread, 10000 );
    check( ret == WAIT_OBJECT_0 && result == WAIT_IO_COMPLETION && apcs == before + 1, "APC from another thread ends SleepEx(INFINITE, TRUE)", result );
    CloseHandle( thread );
}

static void waits(void)
{
    HANDLE event = CreateEventW( NULL, TRUE, FALSE, NULL );
    DWORD start = timeGetTime(), ret = WaitForSingleObject( event, 50 ), took = timeGetTime() - start;
    LONG before = apcs;

    check( ret == WAIT_TIMEOUT && took + 2 >= 50 && took < 2050, "WaitForSingleObject(50) times out after 50 ms", took );
    check( WaitForSingleObject( event, 0 ) == WAIT_TIMEOUT, "WaitForSingleObject(0) on an unset event", 0 );
    SetEvent( event );
    check( WaitForSingleObject( event, 1000 ) == WAIT_OBJECT_0, "WaitForSingleObject on a set event", 0 );
    ResetEvent( event );
    QueueUserAPC( apc, GetCurrentThread(), 0 );
    ret = WaitForSingleObjectEx( event, 5000, TRUE );
    check( ret == WAIT_IO_COMPLETION && apcs == before + 1, "APC ends WaitForSingleObjectEx(5000, TRUE)", ret );
    CloseHandle( event );
}

int main(void)
{
    DWORD ret;

    timeBeginPeriod( 1 );
    self_apc( 0, "APC runs in SleepEx(0, TRUE)" );
    self_apc( INFINITE, "APC runs in SleepEx(INFINITE, TRUE)" );
    self_apc( 5000, "APC ends SleepEx(5000, TRUE) early" );
    ret = SleepEx( 0, TRUE );
    check( ret == 0, "SleepEx(0, TRUE) with no APC returns 0", ret );
    timed( 50, TRUE, "SleepEx(50, TRUE) takes 50 ms and returns 0" );
    timed( 50, FALSE, "SleepEx(50, FALSE) takes 50 ms and returns 0" );
    timed( 1, FALSE, "Sleep(1) takes at least 1 ms" );
    Sleep( 0 );
    SwitchToThread();
    other_thread();
    waits();
    printf( "[wait] done fails=%d\n", fails );
    fflush( stdout );
    return fails != 0;
}
