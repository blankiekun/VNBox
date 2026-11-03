// robust_futex.c: a thread that dies holding a robust mutex must leave it owner-dead and wake a waiter. Each case runs in
// its own child with an alarm, so a hang is one case's result. Passes on real Linux too. Build 32-bit (see build.sh).
// Last line: DONE bad=<n>.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;
static pthread_mutex_t* m;
static pthread_mutex_t* m2;
static volatile int locked;

static void initRobust(pthread_mutex_t* mutex, int shared) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    if (shared) pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(mutex, &a);
}

static void* holdAndDie(void* arg) {
    pthread_mutex_lock(m);
    if (arg) pthread_mutex_lock(m2);
    locked = 1;
    if (arg) usleep(200000); // the main thread is blocked on the mutex by then
    return 0;
}

// a later locker is told, can recover the lock, and it stays usable
static int recovers(pthread_mutex_t* mutex) {
    if (pthread_mutex_lock(mutex) != EOWNERDEAD) return 0;
    if (pthread_mutex_consistent(mutex) != 0 || pthread_mutex_unlock(mutex) != 0) return 0;
    if (pthread_mutex_lock(mutex) != 0) return 0;
    return pthread_mutex_unlock(mutex) == 0;
}

static pthread_mutex_t storage[2];

static int deadBeforeLock(void) {
    m = &storage[0];
    initRobust(m, 0);
    pthread_t t;
    pthread_create(&t, 0, holdAndDie, 0);
    pthread_join(t, 0);
    return recovers(m);
}

// the mutex's waiter is asleep in the futex when its owner dies: only the kernel can wake it
static int waiterWoken(void) {
    m = &storage[0];
    m2 = &storage[1];
    initRobust(m, 0);
    initRobust(m2, 0);
    pthread_t t;
    pthread_create(&t, 0, holdAndDie, (void*)1);
    while (!locked) usleep(1000);
    int first = pthread_mutex_lock(m);
    if (first != EOWNERDEAD) return 0;
    pthread_mutex_consistent(m);
    pthread_mutex_unlock(m);
    pthread_join(t, 0);
    return recovers(m2); // the second lock on the dead thread's list
}

// the owner is a whole process that exits
static int processDies(void) {
    pthread_mutex_t* shared = mmap(0, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    initRobust(shared, 1);
    pid_t child = fork();
    if (!child) {
        pthread_mutex_lock(shared);
        _exit(0);
    }
    waitpid(child, 0, 0);
    return recovers(shared);
}

// a thread that unlocked everything it held changes nothing
static void* lockUnlock(void* arg) {
    pthread_mutex_lock(m);
    pthread_mutex_unlock(m);
    return arg;
}
static int cleanExit(void) {
    m = &storage[0];
    initRobust(m, 0);
    pthread_t t;
    pthread_create(&t, 0, lockUnlock, 0);
    pthread_join(t, 0);
    return pthread_mutex_lock(m) == 0 && pthread_mutex_unlock(m) == 0;
}

static void run(const char* name, int (*test)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (!pid) {
        alarm(10);
        _exit(test() ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) bad++;
    printf("RESULT %s %s status=0x%x\n", name, ok ? "ok" : "BAD", status);
}

int main(void) {
    setvbuf(stdout, 0, _IOLBF, 0);
    run("dead-before-lock", deadBeforeLock);
    run("waiter-woken", waiterWoken);
    run("process-dies", processDies);
    run("clean-exit", cleanExit);
    printf("DONE bad=%d\n", bad);
    fflush(stdout);
    return 0;
}
