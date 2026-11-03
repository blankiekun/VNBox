// A signal handler runs on a thread blocked in an ntsync wait or a futex wait. Linux takes the thread out of line
// before the handler runs, so what it was waiting for goes to the next waiter. Each test holds T1 inside its handler
// (the way Wine's SuspendThread does) and checks that T3 gets the object. A kernel that leaves T1 queued gives the
// object to T1 and T3 sleeps on: a Windows game that suspends a waiting thread hangs for good.
// Also: the rerun keeps the absolute timeout, and a wait already satisfied when the signal comes keeps its object.
// Run under VNBox or Linux with /dev/ntsync: "RESULT ... ok" per test and a last line "DONE bad=0".
// gcc -m32 -O1 -pthread signal_wait.c -o signal_wait
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/ntsync.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define NO_TIMEOUT (~0ULL)
#define OWNER_T1 11
#define OWNER_T3 13
#define OWNER_MAIN 15

static int bad, device;
static int gate[2];
static volatile int inHandler;
static const char* current;
static int badBefore;

#define CHECK(c) do { if (!(c)) { bad++; printf("BAD %s line %d: %s (errno %d)\n", current, __LINE__, #c, errno); fflush(stdout); } } while (0)

static void begin(const char* name) {
    current = name;
    badBefore = bad;
}

static void verdict(void) {
    printf("RESULT %s %s\n", current, bad == badBefore ? "ok" : "failed");
}

static unsigned long long nowNs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static void sleepMs(int ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR);
}

// Stands in for Wine's wait_suspend: stays in the handler until main says resume.
static void onSignal(int sig) {
    char c;
    int saved = errno;
    inHandler = 1;
    while (read(gate[0], &c, 1) < 0 && errno == EINTR);
    inHandler = 0;
    errno = saved;
}

struct waitJob {
    int objs[4];
    unsigned count, owner;
    int all, alert;
    unsigned long long timeout;
    volatile int done;
    int result, err;
    unsigned index;
    unsigned long long endNs;
};

static void* waitThread(void* p) {
    struct waitJob* j = p;
    struct ntsync_wait_args a;
    memset(&a, 0, sizeof(a));
    a.timeout = j->timeout;
    a.objs = (unsigned long)j->objs;
    a.count = j->count;
    a.owner = j->owner;
    a.alert = j->alert;
    j->result = ioctl(device, j->all ? NTSYNC_IOC_WAIT_ALL : NTSYNC_IOC_WAIT_ANY, &a);
    j->err = errno;
    j->index = a.index;
    j->endNs = nowNs();
    j->done = 1;
    return NULL;
}

static int makeEvent(int manual, int signaled) {
    struct ntsync_event_args a = { manual, signaled };
    return ioctl(device, NTSYNC_IOC_CREATE_EVENT, &a);
}

static void setEvent(int fd) {
    unsigned prev;
    CHECK(ioctl(fd, NTSYNC_IOC_EVENT_SET, &prev) == 0);
}

static void suspend(pthread_t t) {
    pthread_kill(t, SIGUSR1);
    for (int i = 0; i < 2000 && !inHandler; i++) sleepMs(1);
    CHECK(inHandler);
}

static void resume(void) {
    CHECK(write(gate[1], "x", 1) == 1);
}

static int finished(volatile int* done, int ms) {
    for (int i = 0; i < ms && !*done; i++) sleepMs(1);
    return *done;
}

static void startWaits(struct waitJob* j1, pthread_t* t1, struct waitJob* j3, pthread_t* t3) {
    pthread_create(t1, NULL, waitThread, j1);
    sleepMs(50); // T1 is first in line
    if (j3) {
        pthread_create(t3, NULL, waitThread, j3);
        sleepMs(50);
    }
}

// T1 and T3 wait on an auto-reset event, T1 first. T1 suspended, event set: T3 wakes.
static void testEvent(void) {
    begin("event");
    int e = makeEvent(0, 0);
    struct waitJob j1 = { .objs = { e }, .count = 1, .owner = OWNER_T1, .timeout = NO_TIMEOUT };
    struct waitJob j3 = { .objs = { e }, .count = 1, .owner = OWNER_T3, .timeout = NO_TIMEOUT };
    pthread_t t1, t3;
    startWaits(&j1, &t1, &j3, &t3);
    suspend(t1);
    setEvent(e);
    CHECK(finished(&j3.done, 2000));
    CHECK(!j1.done);
    resume();
    sleepMs(50);
    CHECK(!j1.done); // the rerun waits again: T3 took the event
    setEvent(e);
    CHECK(finished(&j1.done, 2000));
    CHECK(j1.result == 0);
    setEvent(e); // hung case: frees T3 so the test can go on
    pthread_join(t1, NULL);
    pthread_join(t3, NULL);
    CHECK(j3.result == 0);
    close(e);
    verdict();
}

// T1 waits on a mutex owned by main. T1 suspended, main unlocks: main can lock it again, it did not go to T1.
static void testMutex(void) {
    begin("mutex");
    struct ntsync_mutex_args m = { OWNER_MAIN, 1 };
    int mutex = ioctl(device, NTSYNC_IOC_CREATE_MUTEX, &m);
    struct waitJob j1 = { .objs = { mutex }, .count = 1, .owner = OWNER_T1, .timeout = NO_TIMEOUT };
    pthread_t t1;
    startWaits(&j1, &t1, NULL, NULL);
    suspend(t1);
    m.owner = OWNER_MAIN;
    CHECK(ioctl(mutex, NTSYNC_IOC_MUTEX_UNLOCK, &m) == 0);
    struct waitJob jm = { .objs = { mutex }, .count = 1, .owner = OWNER_MAIN, .timeout = nowNs() + 2000000000ULL };
    waitThread(&jm);
    CHECK(jm.result == 0);
    resume();
    if (jm.result == 0) {
        m.owner = OWNER_MAIN;
        CHECK(ioctl(mutex, NTSYNC_IOC_MUTEX_UNLOCK, &m) == 0);
    }
    CHECK(finished(&j1.done, 2000));
    CHECK(j1.result == 0);
    pthread_join(t1, NULL);
    close(mutex);
    verdict();
}

// T1 and T3 wait on a semaphore at 0. T1 suspended, one released: T3 wakes, and the count is not spent on T1.
static void testSemaphore(void) {
    begin("semaphore");
    struct ntsync_sem_args s = { 0, 2 };
    int sem = ioctl(device, NTSYNC_IOC_CREATE_SEM, &s);
    struct waitJob j1 = { .objs = { sem }, .count = 1, .owner = OWNER_T1, .timeout = NO_TIMEOUT };
    struct waitJob j3 = { .objs = { sem }, .count = 1, .owner = OWNER_T3, .timeout = NO_TIMEOUT };
    pthread_t t1, t3;
    startWaits(&j1, &t1, &j3, &t3);
    suspend(t1);
    unsigned add = 1;
    CHECK(ioctl(sem, NTSYNC_IOC_SEM_RELEASE, &add) == 0);
    CHECK(finished(&j3.done, 2000));
    resume();
    add = 1;
    CHECK(ioctl(sem, NTSYNC_IOC_SEM_RELEASE, &add) == 0);
    CHECK(finished(&j1.done, 2000));
    pthread_join(t1, NULL);
    add = 1;
    ioctl(sem, NTSYNC_IOC_SEM_RELEASE, &add); // hung case
    pthread_join(t3, NULL);
    close(sem);
    verdict();
}

// T1 waits for all of two events, with an alert. T3 waits on the first. T1 suspended, both set: T3 wakes, and
// T1's rerun completes only once both are set again (its alert entry left the queue too).
static void testWaitAll(void) {
    begin("wait-all");
    int e1 = makeEvent(0, 0), e2 = makeEvent(0, 0), alert = makeEvent(0, 0);
    struct waitJob j1 = { .objs = { e1, e2 }, .count = 2, .owner = OWNER_T1, .all = 1, .alert = alert, .timeout = NO_TIMEOUT };
    struct waitJob j3 = { .objs = { e1 }, .count = 1, .owner = OWNER_T3, .timeout = NO_TIMEOUT };
    pthread_t t1, t3;
    startWaits(&j1, &t1, &j3, &t3);
    suspend(t1);
    setEvent(e2);
    setEvent(e1); // completes T1's set, if T1 were still in line
    CHECK(finished(&j3.done, 2000));
    resume();
    sleepMs(50);
    CHECK(!j1.done); // e1 went to T3
    setEvent(e1);
    CHECK(finished(&j1.done, 2000));
    CHECK(j1.result == 0 && j1.index == 0);
    setEvent(e1); // hung case
    pthread_join(t1, NULL);
    pthread_join(t3, NULL);
    close(e1);
    close(e2);
    close(alert);
    verdict();
}

// The timeout stays absolute across the handler: suspended past its deadline, T1 times out once resumed.
static void testTimeout(void) {
    begin("timeout");
    int e = makeEvent(0, 0);
    unsigned long long deadline = nowNs() + 300000000ULL;
    struct waitJob j1 = { .objs = { e }, .count = 1, .owner = OWNER_T1, .timeout = deadline };
    pthread_t t1;
    startWaits(&j1, &t1, NULL, NULL);
    suspend(t1);
    sleepMs(500);
    CHECK(!j1.done);
    resume();
    CHECK(finished(&j1.done, 200)); // not a whole new 300 ms
    CHECK(j1.result == -1 && j1.err == ETIMEDOUT);
    pthread_join(t1, NULL);
    close(e);
    verdict();
}

// A wait the event already went to keeps it: the signal comes right after the set, before T1 ran, and the
// handler must not cost T1 the event it was given.
static void testClaimedKept(void) {
    begin("claimed");
    int e = makeEvent(0, 0);
    struct waitJob j1 = { .objs = { e }, .count = 1, .owner = OWNER_T1, .timeout = NO_TIMEOUT };
    pthread_t t1;
    startWaits(&j1, &t1, NULL, NULL);
    setEvent(e);
    pthread_kill(t1, SIGUSR1);
    resume(); // the handler reads this whenever it runs
    CHECK(finished(&j1.done, 2000));
    CHECK(j1.result == 0);
    setEvent(e); // hung case
    pthread_join(t1, NULL);
    close(e);
    verdict();
}

static long futexCall(volatile int* word, int op, int value) {
    return syscall(SYS_futex, word, op, value, NULL, NULL, 0);
}

struct futexJob {
    volatile int* word;
    volatile int done;
    long result;
};

static void* futexThread(void* p) {
    struct futexJob* j = p;
    j->result = futexCall(j->word, FUTEX_WAIT_PRIVATE, 0);
    j->done = 1;
    return NULL;
}

// T1 and T3 wait on a futex, T1 first. T1 suspended, wake one: the wake reaches T3, not T1.
static void testFutex(void) {
    begin("futex");
    static volatile int word;
    struct futexJob j1 = { &word }, j3 = { &word };
    pthread_t t1, t3;
    pthread_create(&t1, NULL, futexThread, &j1);
    sleepMs(50);
    pthread_create(&t3, NULL, futexThread, &j3);
    sleepMs(50);
    suspend(t1);
    CHECK(futexCall(&word, FUTEX_WAKE_PRIVATE, 1) == 1);
    CHECK(finished(&j3.done, 2000));
    CHECK(!j1.done);
    resume();
    sleepMs(50);
    CHECK(!j1.done); // the rerun waits again
    CHECK(futexCall(&word, FUTEX_WAKE_PRIVATE, 1) == 1);
    CHECK(finished(&j1.done, 2000));
    futexCall(&word, FUTEX_WAKE_PRIVATE, 2); // hung case
    pthread_join(t1, NULL);
    pthread_join(t3, NULL);
    verdict();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    device = open("/dev/ntsync", O_RDWR);
    CHECK(device >= 0);
    CHECK(pipe(gate) == 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSignal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    testEvent();
    testMutex();
    testSemaphore();
    testWaitAll();
    testTimeout();
    testClaimedKept();
    testFutex();
    printf("DONE bad=%d\n", bad);
    return bad != 0;
}
