// self_signal.c: a thread signalling itself must enter a handler at its first instruction and, with an ignored signal,
// carry on at the instruction after the syscall. Each case runs in its own child with an alarm, so a crash or hang is one
// case's result. Passes on real Linux too. Build 32-bit (see build.sh). Last line: DONE bad=<n>.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// Handler in assembly, so a 2-byte-late entry is certain to show: the first instruction is 2 bytes long and does the work.
__asm__(".text\n"
        "handler_entry:\n"
        "    jmp 1f\n"                    // 2 bytes: entering after it skips the count
        "    ret\n"
        "1:  incl handler_count\n"
        "    ret\n");
extern void handler_entry(int);
volatile int handler_count;

static int bad;

static long tgkillSelf(int sig) { return syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig); }
static long killSelf(int sig) { return syscall(SYS_kill, getpid(), sig); }

static int withHandler(long (*send)(int)) {
    signal(SIGUSR1, handler_entry);
    long r = send(SIGUSR1);
    return r == 0 && handler_count == 1;
}

// The int 0x80 is followed by a 2-byte instruction a skip would miss.
static int ignoredKeepsNextInstruction(int number, int a1, int a2, int a3) {
    int count = 0, result;
    __asm__ volatile(
        "push %%ebx\n"
        "mov %[a1], %%ebx\n"
        "int $0x80\n"
        ".byte 0x41, 0x90\n" // inc ecx; nop: 2 bytes a double step skips (ecx is restored by the kernel to a2)
        "pop %%ebx\n"
        : "=a"(result), "=c"(count)
        : "0"(number), [a1] "r"(a1), "1"(a2), "d"(a3));
    return result == 0 && count == a2 + 1;
}

static int handlerTgkill(void) { return withHandler(tgkillSelf); }
static int handlerKill(void) { return withHandler(killSelf); }
static int ignoredTgkill(void) { return ignoredKeepsNextInstruction(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGCHLD); }
static int ignoredKill(void) { return ignoredKeepsNextInstruction(SYS_kill, getpid(), SIGCONT, 0); }
static int sigIgn(void) {
    signal(SIGUSR1, SIG_IGN);
    return ignoredKeepsNextInstruction(SYS_kill, getpid(), SIGUSR1, 0);
}
static int raiseReturns(void) {
    signal(SIGUSR1, handler_entry);
    return raise(SIGUSR1) == 0 && handler_count == 1;
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
    run("tgkill-handler", handlerTgkill);
    run("kill-handler", handlerKill);
    run("raise-handler", raiseReturns);
    run("tgkill-ignored-default", ignoredTgkill);
    run("kill-ignored-default", ignoredKill);
    run("kill-sig-ign", sigIgn);
    printf("DONE bad=%d\n", bad);
    fflush(stdout);
    return 0;
}
