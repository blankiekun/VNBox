// futex_clock.c: what Linux does with a futex wait's clock, deadline and bitset. Each case runs in its own child with an
// alarm, so a hang is one case's result. Passes on real Linux too. Build 32-bit (see build.sh). Last line: DONE bad=<n>.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_futex_time64
#define __NR_futex_time64 422
#endif

struct ts32 { int32_t sec, nsec; };
struct ts64 { int64_t sec; int64_t nsec; };

static uint32_t word;

// a futex call, 32 or 64 bit timespec, deadline given as sec/nsec; returns 0 or -errno
static long fx(int is64, int op, uint32_t val, int has, int64_t sec, long nsec, uint32_t val3) {
    long r;
    if (is64) {
        struct ts64 t = { sec, nsec };
        r = syscall(__NR_futex_time64, &word, op | FUTEX_PRIVATE_FLAG, val, has ? &t : 0, 0, val3);
    } else {
        struct ts32 t = { (int32_t)sec, (int32_t)nsec };
        r = syscall(SYS_futex, &word, op | FUTEX_PRIVATE_FLAG, val, has ? &t : 0, 0, val3);
    }
    return r < 0 ? -errno : r;
}

static int64_t nowMs(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void deadline(clockid_t c, int64_t addMs, int64_t* sec, long* nsec) {
    struct timespec t;
    clock_gettime(c, &t);
    int64_t ns = (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec + addMs * 1000000LL;
    *sec = ns / 1000000000LL;
    *nsec = (long)(ns % 1000000000LL);
}

#define EXPECT(cond, ...) do { if (!(cond)) { printf(__VA_ARGS__); printf("\n"); fflush(stdout); return 1; } } while (0)

// bug: a deadline far in the past wraps to "no timeout" and hangs (past by more than the emulator's uptime)
static int pastFar(int is64) {
    int64_t s; long n, r; deadline(CLOCK_REALTIME, -3600 * 1000, &s, &n);
    r = fx(is64, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, 0, 1, s, n, ~0u);
    EXPECT(r == -ETIMEDOUT, "returned %ld", r);
    return 0;
}
// bug: the same, past by a moment (a timed wait entered late)
static int pastNear(int is64) {
    int64_t s; long n, r; deadline(CLOCK_REALTIME, -1, &s, &n);
    r = fx(is64, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, 0, 1, s, n, ~0u);
    EXPECT(r == -ETIMEDOUT, "returned %ld", r);
    return 0;
}
// bug: a monotonic deadline is read against the realtime clock
static int pastMono(int is64) {
    long r = fx(is64, FUTEX_WAIT_BITSET, 0, 1, 0, 1, ~0u);
    EXPECT(r == -ETIMEDOUT, "returned %ld", r);
    return 0;
}
static int soon(int is64, int op, clockid_t c) {
    int64_t s, t0 = nowMs(CLOCK_MONOTONIC); long n, r; deadline(c, 200, &s, &n);
    r = fx(is64, op, 0, 1, s, n, ~0u);
    int64_t took = nowMs(CLOCK_MONOTONIC) - t0;
    EXPECT(r == -ETIMEDOUT && took >= 180, "returned %ld after %lld ms", r, (long long)took);
    return 0;
}
// bug: a monotonic deadline 200 ms away doesn't expire on time
static int soonMono(int is64) { return soon(is64, FUTEX_WAIT_BITSET, CLOCK_MONOTONIC); }
// bug: a change of clock breaks the realtime deadline everyone uses
static int soonReal(int is64) { return soon(is64, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, CLOCK_REALTIME); }
// bug: a relative FUTEX_WAIT timeout gets the deadline arithmetic
static int soonRelative(int is64) {
    int64_t t0 = nowMs(CLOCK_MONOTONIC); long r = fx(is64, FUTEX_WAIT, 0, 1, 0, 200000000, 0);
    int64_t took = nowMs(CLOCK_MONOTONIC) - t0;
    EXPECT(r == -ETIMEDOUT && took >= 180, "returned %ld after %lld ms", r, (long long)took);
    return 0;
}
// bug: a past deadline answers before the value is compared
static int mismatchFirst(int is64) {
    long r = fx(is64, FUTEX_WAIT_BITSET, 12345, 1, 0, 1, ~0u);
    EXPECT(r == -EAGAIN, "returned %ld", r);
    return 0;
}
// bug: an invalid timespec is taken as a deadline
static int badNsec(int is64) {
    long r = fx(is64, FUTEX_WAIT_BITSET, 0, 1, 1, 1000000000, ~0u);
    EXPECT(r == -EINVAL, "returned %ld", r);
    return 0;
}
static int badSec(int is64) {
    long r = fx(is64, FUTEX_WAIT_BITSET, 0, 1, -1, 0, ~0u);
    EXPECT(r == -EINVAL, "returned %ld", r);
    return 0;
}

// a waiter on word, its result in waiterResult
static volatile long waiterResult = 1;
struct waiter { int op; int has; int64_t sec; long nsec; uint32_t val3; int is64; };
static void* waiterMain(void* p) {
    struct waiter* w = p;
    waiterResult = fx(w->is64, w->op, 0, w->has, w->sec, w->nsec, w->val3);
    return 0;
}
static pthread_t startWaiter(struct waiter* w) {
    pthread_t t;
    pthread_create(&t, 0, waiterMain, w);
    usleep(100000);
    return t;
}

// bug: a deadline too far off for 32 bits of ms truncates into a short wait (time_t max, a common "never")
static int farFuture(int is64) {
    struct waiter w = { FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, 1, (int64_t)1 << 62, 0, ~0u, 1 };
    pthread_t t = startWaiter(&w);
    usleep(200000);
    EXPECT(waiterResult == 1, "gone early with %ld", (long)waiterResult);
    long woke = fx(0, FUTEX_WAKE, 1, 0, 0, 0, 0);
    pthread_join(t, 0);
    EXPECT(woke == 1 && waiterResult == 0, "woke %ld result %ld", woke, (long)waiterResult);
    return 0;
}
// bug: FUTEX_WAKE_BITSET can't wake a plain FUTEX_WAIT
static int bitsetWakesPlain(int is64) {
    struct waiter w = { FUTEX_WAIT, 0, 0, 0, 0, 0 };
    pthread_t t = startWaiter(&w);
    long woke = fx(0, FUTEX_WAKE_BITSET, 1, 0, 0, 0, 1);
    EXPECT(woke == 1, "woke %ld", woke);
    pthread_join(t, 0);
    EXPECT(waiterResult == 0, "result %ld", (long)waiterResult);
    return 0;
}
// bug: a wait with an empty bitset sleeps where no wake can reach it
static int waitBitsetZero(int is64) {
    long r = fx(is64, FUTEX_WAIT_BITSET, 0, 0, 0, 0, 0);
    EXPECT(r == -EINVAL, "returned %ld", r);
    return 0;
}
// bug: making every wait match every wake (the mask has to keep working)
static int maskStillFilters(int is64) {
    struct waiter w = { FUTEX_WAIT_BITSET, 0, 0, 0, 2, 0 };
    pthread_t t = startWaiter(&w);
    long woke = fx(0, FUTEX_WAKE_BITSET, 1, 0, 0, 0, 1);
    EXPECT(woke == 0, "mask 1 woke %ld of mask 2", woke);
    woke = fx(0, FUTEX_WAKE_BITSET, 1, 0, 0, 0, 3);
    EXPECT(woke == 1, "mask 3 woke %ld of mask 2", woke);
    pthread_join(t, 0);
    return 0;
}
// bug: the library's monotonic timed wait hangs (pthread_cond_clockwait)
static int condClockwait(int is64) {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    struct timespec ts;
    int64_t t0 = nowMs(CLOCK_MONOTONIC);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_nsec += 200000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_nsec -= 1000000000; ts.tv_sec++; }
    pthread_mutex_lock(&m);
    int r = pthread_cond_clockwait(&c, &m, CLOCK_MONOTONIC, &ts);
    int64_t took = nowMs(CLOCK_MONOTONIC) - t0;
    EXPECT(r == ETIMEDOUT && took >= 180, "returned %d after %lld ms", r, (long long)took);
    return 0;
}
// bug: the library's realtime timed wait hangs when its deadline is long past
static void* forever(void* p) { for (;;) pause(); return 0; }
static int timedjoinPast(int is64) {
    pthread_t t;
    pthread_create(&t, 0, forever, 0);
    usleep(50000);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec -= 3600;
    int r = pthread_timedjoin_np(t, 0, &ts);
    EXPECT(r == ETIMEDOUT, "returned %d", r);
    return 0;
}

static int bad;
static void run(const char* name, int (*fn)(int), int is64) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(4);
        int r = fn(is64);
        fflush(stdout);
        _exit(r);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    const char* result = WIFSIGNALED(status) ? "HUNG" : WEXITSTATUS(status) ? "FAIL" : "ok";
    if (strcmp(result, "ok")) {
        bad++;
    }
    printf("RESULT %s/%d %s\n", name, is64 ? 64 : 32, result);
    fflush(stdout);
}

int main(void) {
    for (int is64 = 0; is64 < 2; is64++) {
        run("past-realtime-far", pastFar, is64);
        run("past-realtime-near", pastNear, is64);
        run("past-monotonic", pastMono, is64);
        run("soon-monotonic", soonMono, is64);
        run("soon-realtime", soonReal, is64);
        run("soon-relative", soonRelative, is64);
        run("mismatch-before-deadline", mismatchFirst, is64);
        run("bad-nsec", badNsec, is64);
        run("bad-sec", badSec, is64);
        run("wait-bitset-zero", waitBitsetZero, is64);
    }
    run("far-future", farFuture, 1);
    run("bitset-wakes-plain", bitsetWakesPlain, 0);
    run("mask-still-filters", maskStillFilters, 0);
    run("cond-clockwait", condClockwait, 0);
    run("timedjoin-past", timedjoinPast, 0);
    printf("DONE bad=%d\n", bad);
    fflush(stdout);
    return 0;
}
