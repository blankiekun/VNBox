// Fills the fd table with ntsync objects, as a wineserver does for a game that leaks handles. The kernel must answer
// EMFILE on every call that takes a new number (the emulator used to die with a fatal error), must not truncate a
// file it could not open, and must work again once fds are closed.
// Run under VNBox or Linux with /dev/ntsync: a last line "DONE bad=0 fds=<n>".
// gcc -m32 -O1 fd_limit.c -o fd_limit
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/ntsync.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

#define KEEP_FILE "/tmp/fd_limit_keep"
#define KEEP_TEXT "keep"

static int bad;
#define CHECK(c) do { if (!(c)) { bad++; printf("BAD line %d: %s (errno %d)\n", __LINE__, #c, errno); fflush(stdout); } } while (0)
#define EXPECT_EMFILE(call) do { errno = 0; int r_ = (call); if (r_ != -1 || errno != EMFILE) { bad++; printf("BAD line %d: %s = %d (errno %d), expected EMFILE\n", __LINE__, #call, r_, errno); fflush(stdout); } } while (0)

static int makeEvent(int device) {
    struct ntsync_event_args a = { 0, 0 };
    return ioctl(device, NTSYNC_IOC_CREATE_EVENT, &a);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int device = open("/dev/ntsync", O_RDWR);
    CHECK(device >= 0);

    // everything the full table needs, opened while there is room
    int keep = open(KEEP_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    CHECK(write(keep, KEEP_TEXT, 4) == 4);
    close(keep);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/fd_limit_sock");
    unlink(addr.sun_path);
    CHECK(bind(listener, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    CHECK(listen(listener, 2) == 0);
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    fcntl(client, F_SETFL, O_NONBLOCK); // VNBox: a blocking connect to a unix socket waits for the accept (Linux doesn't)
    CHECK(connect(client, (struct sockaddr*)&addr, sizeof(addr)) == 0 || errno == EINPROGRESS);
    int carrier[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, carrier) == 0);

    // a message that carries an fd, sent while there is room, received when there is none
    char data = 'd';
    struct iovec iov = { &data, 1 };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.b;
    msg.msg_controllen = sizeof(control.b);
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &device, sizeof(int));
    CHECK(sendmsg(carrier[0], &msg, 0) == 1);

    // fill
    struct rlimit limit;
    getrlimit(RLIMIT_NOFILE, &limit);
    printf("INFO soft limit %lu\n", (unsigned long)limit.rlim_cur);
    int fds = 0, last = -1;
    errno = 0;
    while (fds < 70000) {
        int fd = makeEvent(device);
        if (fd < 0) {
            break;
        }
        last = fd;
        fds++;
    }
    printf("INFO %d events, last fd %d, then errno %d\n", fds, last, errno);
    CHECK(errno == EMFILE);
    CHECK(fds > 0);

    // the table is full: every call that takes a new number says so
    struct ntsync_sem_args semArgs = { 0, 1 };
    struct ntsync_mutex_args mutexArgs = { 0, 0 };
    EXPECT_EMFILE(ioctl(device, NTSYNC_IOC_CREATE_SEM, &semArgs));
    EXPECT_EMFILE(ioctl(device, NTSYNC_IOC_CREATE_MUTEX, &mutexArgs));
    EXPECT_EMFILE(open("/dev/null", O_RDONLY));
    EXPECT_EMFILE(open(KEEP_FILE, O_WRONLY | O_TRUNC));
    EXPECT_EMFILE(open("/tmp/fd_limit_new", O_WRONLY | O_CREAT, 0644));
    EXPECT_EMFILE(dup(device));
    EXPECT_EMFILE(fcntl(device, F_DUPFD, 0));
    EXPECT_EMFILE(fcntl(device, F_DUPFD_CLOEXEC, 0));
    int pipes[2];
    EXPECT_EMFILE(pipe(pipes));
    EXPECT_EMFILE(pipe2(pipes, O_CLOEXEC));
    EXPECT_EMFILE(socketpair(AF_UNIX, SOCK_STREAM, 0, pipes));
    EXPECT_EMFILE(socket(AF_UNIX, SOCK_STREAM, 0));
    EXPECT_EMFILE(socket(AF_INET, SOCK_STREAM, 0));
    EXPECT_EMFILE(accept(listener, NULL, NULL));
    EXPECT_EMFILE(accept4(listener, NULL, NULL, SOCK_CLOEXEC));
    EXPECT_EMFILE(eventfd(0, 0));
    EXPECT_EMFILE(epoll_create1(0));
    EXPECT_EMFILE(timerfd_create(CLOCK_MONOTONIC, 0));
    EXPECT_EMFILE(inotify_init()); // VNBox has no inotify_init1
    EXPECT_EMFILE((int)syscall(SYS_memfd_create, "x", 0));
    sigset_t set;
    sigemptyset(&set);
    EXPECT_EMFILE(signalfd(-1, &set, 0));

    // dup2 onto an open number needs no new one
    CHECK(dup2(device, last) == last);

    // recvmsg delivers the data and truncates what it can't place (Linux: MSG_CTRUNC, no SCM_RIGHTS)
    char got = 0;
    struct iovec riov = { &got, 1 };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } rcontrol;
    struct msghdr rmsg;
    memset(&rmsg, 0, sizeof(rmsg));
    rmsg.msg_iov = &riov;
    rmsg.msg_iovlen = 1;
    rmsg.msg_control = rcontrol.b;
    rmsg.msg_controllen = sizeof(rcontrol.b);
    CHECK(recvmsg(carrier[1], &rmsg, 0) == 1);
    CHECK(got == 'd');
    CHECK(rmsg.msg_flags & MSG_CTRUNC);
    CHECK(CMSG_FIRSTHDR(&rmsg) == NULL);

    // one free number: a pair can't fit and must not keep the number it got
    CHECK(close(last) == 0);
    EXPECT_EMFILE(pipe(pipes));
    EXPECT_EMFILE(socketpair(AF_UNIX, SOCK_DGRAM, 0, pipes));
    int one = open("/dev/null", O_RDONLY);
    CHECK(one == last);
    close(one);

    // room again: the connection that waited is still there, the file still holds its text, the object works
    int accepted = accept(listener, NULL, NULL);
    CHECK(accepted >= 0);
    if (accepted >= 0) {
        close(accepted);
    }
    int readBack = open(KEEP_FILE, O_RDONLY);
    CHECK(readBack >= 0);
    char text[5] = { 0 };
    CHECK(read(readBack, text, 4) == 4);
    CHECK(strcmp(text, KEEP_TEXT) == 0);
    close(readBack);
    CHECK(access("/tmp/fd_limit_new", F_OK) != 0); // the failed create left nothing behind
    int again = makeEvent(device);
    CHECK(again >= 0);
    unsigned prev;
    CHECK(ioctl(again, NTSYNC_IOC_EVENT_SET, &prev) == 0);
    close(again);

    unlink(KEEP_FILE);
    unlink(addr.sun_path);
    printf("DONE bad=%d fds=%d\n", bad, fds);
    return bad != 0;
}
