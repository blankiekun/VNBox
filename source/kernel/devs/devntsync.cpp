/*
 *  /dev/ntsync for the emulated kernel: a port of Linux's drivers/misc/ntsync.c (GPL-2.0, Elizabeth Figura),
 *  ABI of include/uapi/linux/ntsync.h as of Linux 6.14 (unchanged since), the one Wine 10/11 build against.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "boxedwine.h"

#include "../../io/fsvirtualopennode.h"
#include "devntsync.h"
#include "kstat.h"

#include <atomic>
#include <list>
#include <vector>

// Differences from Linux: one lock for every device and object (Linux locks per object plus a per-device
// wait-all lock), and a blocked wait is a record that outlives the syscall: the single-threaded scheduler
// returns K_WAIT and runs the ioctl again when the thread wakes, so the rerun finds its record by thread and
// argument address and picks up where it left off. A signal delivered to a blocked thread runs its handler
// first and the ioctl then restarts the same wait (Linux: -ERESTARTSYS), so the timeout stays absolute.

#define NTSYNC_K_EOVERFLOW 75
#define NTSYNC_K_EOWNERDEAD 130

// _IOC(dir, 'N', nr, size): dir 1 = _IOW, 2 = _IOR, 3 = _IOWR
#define NTSYNC_IOC(dir, nr, size) (((U32)(dir) << 30) | ((U32)(size) << 16) | ((U32)'N' << 8) | (U32)(nr))
#define NTSYNC_IOC_CREATE_SEM   NTSYNC_IOC(1, 0x80, 8)  // 0x40084e80 struct ntsync_sem_args
#define NTSYNC_IOC_SEM_RELEASE  NTSYNC_IOC(3, 0x81, 4)  // 0xc0044e81 __u32
#define NTSYNC_IOC_WAIT_ANY     NTSYNC_IOC(3, 0x82, 40) // 0xc0284e82 struct ntsync_wait_args
#define NTSYNC_IOC_WAIT_ALL     NTSYNC_IOC(3, 0x83, 40) // 0xc0284e83 struct ntsync_wait_args
#define NTSYNC_IOC_CREATE_MUTEX NTSYNC_IOC(1, 0x84, 8)  // 0x40084e84 struct ntsync_mutex_args
#define NTSYNC_IOC_MUTEX_UNLOCK NTSYNC_IOC(3, 0x85, 8)  // 0xc0084e85 struct ntsync_mutex_args
#define NTSYNC_IOC_MUTEX_KILL   NTSYNC_IOC(1, 0x86, 4)  // 0x40044e86 __u32
#define NTSYNC_IOC_CREATE_EVENT NTSYNC_IOC(1, 0x87, 8)  // 0x40084e87 struct ntsync_event_args
#define NTSYNC_IOC_EVENT_SET    NTSYNC_IOC(2, 0x88, 4)  // 0x80044e88 __u32
#define NTSYNC_IOC_EVENT_RESET  NTSYNC_IOC(2, 0x89, 4)  // 0x80044e89 __u32
#define NTSYNC_IOC_EVENT_PULSE  NTSYNC_IOC(2, 0x8a, 4)  // 0x80044e8a __u32
#define NTSYNC_IOC_SEM_READ     NTSYNC_IOC(2, 0x8b, 8)  // 0x80084e8b struct ntsync_sem_args
#define NTSYNC_IOC_MUTEX_READ   NTSYNC_IOC(2, 0x8c, 8)  // 0x80084e8c struct ntsync_mutex_args
#define NTSYNC_IOC_EVENT_READ   NTSYNC_IOC(2, 0x8d, 8)  // 0x80084e8d struct ntsync_event_args

#define NTSYNC_WAIT_REALTIME 0x1
#define NTSYNC_MAX_WAIT_COUNT 64
#define NTSYNC_NO_TIMEOUT 0xFFFFFFFFFFFFFFFFull

// struct ntsync_wait_args { __u64 timeout; __u64 objs; __u32 count, index, flags, owner, alert, pad; }
#define WAIT_ARGS_SIZE 40
#define WAIT_ARGS_INDEX 20

// One open of /dev/ntsync. Objects of different opens can't be waited on together.
struct NtsyncDevice {
};

enum NtsyncType {
    NTSYNC_TYPE_SEM,
    NTSYNC_TYPE_MUTEX,
    NTSYNC_TYPE_EVENT,
};

class NtsyncWait;

struct NtsyncEntry {
    NtsyncWait* q = nullptr;
    U32 index = 0;
};

class NtsyncObject : public KObject {
public:
    NtsyncObject(const std::shared_ptr<NtsyncDevice>& dev, NtsyncType kind) : KObject(KTYPE_NTSYNC), dev(dev), kind(kind) {}

    // from KObject
    U32 ioctl(KThread* thread, U32 request) override;
    S64 seek(S64 pos) override { return -K_ESPIPE; }
    S64 length() override { return 0; }
    S64 getPos() override { return 0; }
    void setBlocking(bool blocking) override {}
    bool isBlocking() override { return true; }
    void setAsync(bool isAsync) override {}
    bool isAsync() override { return false; }
    KFileLock* getLock(KFileLock* lock) override { return nullptr; }
    U32 setLock(KFileLock* lock, bool wait) override { return -K_EINVAL; }
    bool supportsLocks() override { return false; }
    bool isOpen() override { return true; }
    bool isReadReady() override { return false; }
    bool isWriteReady() override { return false; }
    void waitForEvents(BOXEDWINE_CONDITION& parentCondition, U32 events) override {}
    U32 writeNative(U8* buffer, U32 len) override { return -K_EINVAL; }
    U32 readNative(U8* buffer, U32 len) override { return -K_EINVAL; }
    U32 stat(KProcess* process, U32 address, bool is64) override {
        KSystem::writeStat(process, B(""), address, is64, 0, 0, K__S_IREAD | K__S_IWRITE, 0, 0, 4096, 0, 0, 1);
        return 0;
    }
    U32 map(KThread* thread, U32 address, U32 len, S32 prot, S32 flags, U64 off) override { return 0; }
    bool canMap() override { return false; }
    BString selfFd() override { return B("anon_inode:ntsync"); }

    const std::shared_ptr<NtsyncDevice> dev;
    const NtsyncType kind;

    // semaphore
    U32 semCount = 0;
    U32 semMax = 0;
    // mutex
    U32 mutexCount = 0;
    U32 mutexOwner = 0;
    bool mutexOwnerDead = false;
    // event
    bool eventManual = false;
    bool eventSignaled = false;

    std::list<NtsyncEntry*> anyWaiters;
    std::list<NtsyncEntry*> allWaiters;
};

// A wait in progress. Lives from the first run of a wait ioctl until it returns (not K_WAIT).
class NtsyncWait {
public:
    NtsyncWait() : cond(std::make_shared<BoxedWineCondition>(B("ntsync wait"))) {}

    KThread* thread = nullptr;
    U32 argp = 0;
    bool all = false;
    U32 owner = 0;
    U32 count = 0; // objects, without the alert event
    U64 timeout = NTSYNC_NO_TIMEOUT; // absolute ns
    bool realtime = false;
    bool ownerDead = false;
    // Only the waker that swaps -1 for an index changes object state and wakes the thread.
    std::atomic<int> signaled{-1};
    std::vector<std::shared_ptr<NtsyncObject>> objs; // count objects, then the alert event if any
    std::vector<NtsyncEntry> entries;
    BOXEDWINE_CONDITION cond;
};

static BOXEDWINE_MUTEX_NR ntsyncMutex;
static std::vector<NtsyncWait*> ntsyncWaits;

class NtsyncLock {
public:
    NtsyncLock() {
        BOXEDWINE_MUTEX_LOCK(ntsyncMutex);
    }
    ~NtsyncLock() {
        BOXEDWINE_MUTEX_UNLOCK(ntsyncMutex);
    }
};

static bool isSignaled(NtsyncObject* obj, U32 owner) {
    switch (obj->kind) {
    case NTSYNC_TYPE_SEM:
        return obj->semCount != 0;
    case NTSYNC_TYPE_MUTEX:
        if (obj->mutexOwner && obj->mutexOwner != owner) {
            return false;
        }
        return obj->mutexCount < 0xFFFFFFFF;
    case NTSYNC_TYPE_EVENT:
        return obj->eventSignaled;
    }
    return false;
}

static bool claim(NtsyncWait* q, int index) {
    int expected = -1;
    return q->signaled.compare_exchange_strong(expected, index);
}

static void wakeWaiter(NtsyncWait* q) {
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(q->cond);
    BOXEDWINE_CONDITION_SIGNAL(q->cond);
}

static void acquireMutex(NtsyncObject* mutex, NtsyncWait* q) {
    if (mutex->mutexOwnerDead) {
        q->ownerDead = true;
    }
    mutex->mutexOwnerDead = false;
    mutex->mutexCount++;
    mutex->mutexOwner = q->owner;
}

static void tryWakeAll(NtsyncWait* q) {
    for (U32 i = 0; i < q->count; i++) {
        if (!isSignaled(q->objs[i].get(), q->owner)) {
            return;
        }
    }
    if (!claim(q, 0)) {
        return;
    }
    for (U32 i = 0; i < q->count; i++) {
        NtsyncObject* obj = q->objs[i].get();
        switch (obj->kind) {
        case NTSYNC_TYPE_SEM:
            obj->semCount--;
            break;
        case NTSYNC_TYPE_MUTEX:
            acquireMutex(obj, q);
            break;
        case NTSYNC_TYPE_EVENT:
            if (!obj->eventManual) {
                obj->eventSignaled = false;
            }
            break;
        }
    }
    wakeWaiter(q);
}

static void tryWakeAllObj(NtsyncObject* obj) {
    for (NtsyncEntry* entry : obj->allWaiters) {
        tryWakeAll(entry->q);
    }
}

static void tryWakeAny(NtsyncObject* obj) {
    for (NtsyncEntry* entry : obj->anyWaiters) {
        NtsyncWait* q = entry->q;
        switch (obj->kind) {
        case NTSYNC_TYPE_SEM:
            if (!obj->semCount) {
                return;
            }
            if (claim(q, (int)entry->index)) {
                obj->semCount--;
                wakeWaiter(q);
            }
            break;
        case NTSYNC_TYPE_MUTEX:
            if (obj->mutexCount == 0xFFFFFFFF) {
                return;
            }
            if (obj->mutexOwner && obj->mutexOwner != q->owner) {
                continue;
            }
            if (claim(q, (int)entry->index)) {
                acquireMutex(obj, q);
                wakeWaiter(q);
            }
            break;
        case NTSYNC_TYPE_EVENT:
            if (!obj->eventSignaled) {
                return;
            }
            if (claim(q, (int)entry->index)) {
                if (!obj->eventManual) {
                    obj->eventSignaled = false;
                }
                wakeWaiter(q);
            }
            break;
        }
    }
}

// After a state change that can satisfy waiters: wait-all waiters first, like Linux.
static void wakeObj(NtsyncObject* obj) {
    if (!obj->allWaiters.empty()) {
        tryWakeAllObj(obj);
    }
    tryWakeAny(obj);
}

static U32 putU32(KMemory* memory, U32 address, U32 value) {
    if (!memory->canWrite(address, 4)) {
        return -K_EFAULT;
    }
    memory->writed(address, value);
    return 0;
}

U32 NtsyncObject::ioctl(KThread* thread, U32 request) {
    CPU* cpu = thread->cpu;
    KMemory* memory = thread->memory;
    U32 argp = IOCTL_ARG1;
    NtsyncLock lock;

    switch (request) {
    case NTSYNC_IOC_SEM_RELEASE: {
        if (!memory->canRead(argp, 4)) {
            return -K_EFAULT;
        }
        U32 add = memory->readd(argp);
        if (kind != NTSYNC_TYPE_SEM) {
            return -K_EINVAL;
        }
        U32 prev = semCount;
        U64 sum = (U64)semCount + add;
        if (sum > semMax) {
            return -NTSYNC_K_EOVERFLOW;
        }
        semCount = (U32)sum;
        wakeObj(this);
        return putU32(memory, argp, prev);
    }
    case NTSYNC_IOC_SEM_READ:
        if (kind != NTSYNC_TYPE_SEM) {
            return -K_EINVAL;
        }
        if (!memory->canWrite(argp, 8)) {
            return -K_EFAULT;
        }
        memory->writed(argp, semCount);
        memory->writed(argp + 4, semMax);
        return 0;
    case NTSYNC_IOC_MUTEX_UNLOCK: {
        if (!memory->canRead(argp, 8)) {
            return -K_EFAULT;
        }
        U32 owner = memory->readd(argp);
        if (!owner || kind != NTSYNC_TYPE_MUTEX) {
            return -K_EINVAL;
        }
        U32 prev = mutexCount;
        if (mutexOwner != owner) {
            return -K_EPERM;
        }
        if (!--mutexCount) {
            mutexOwner = 0;
        }
        wakeObj(this);
        return putU32(memory, argp + 4, prev);
    }
    case NTSYNC_IOC_MUTEX_KILL: {
        if (!memory->canRead(argp, 4)) {
            return -K_EFAULT;
        }
        U32 owner = memory->readd(argp);
        if (!owner || kind != NTSYNC_TYPE_MUTEX) {
            return -K_EINVAL;
        }
        if (mutexOwner != owner) {
            return -K_EPERM;
        }
        mutexOwnerDead = true;
        mutexOwner = 0;
        mutexCount = 0;
        wakeObj(this);
        return 0;
    }
    case NTSYNC_IOC_MUTEX_READ:
        if (kind != NTSYNC_TYPE_MUTEX) {
            return -K_EINVAL;
        }
        if (!memory->canWrite(argp, 8)) {
            return -K_EFAULT;
        }
        memory->writed(argp, mutexOwner);
        memory->writed(argp + 4, mutexCount);
        return mutexOwnerDead ? -NTSYNC_K_EOWNERDEAD : 0;
    case NTSYNC_IOC_EVENT_SET:
    case NTSYNC_IOC_EVENT_PULSE: {
        if (kind != NTSYNC_TYPE_EVENT) {
            return -K_EINVAL;
        }
        U32 prev = eventSignaled ? 1 : 0;
        eventSignaled = true;
        wakeObj(this);
        if (request == NTSYNC_IOC_EVENT_PULSE) {
            eventSignaled = false;
        }
        return putU32(memory, argp, prev);
    }
    case NTSYNC_IOC_EVENT_RESET: {
        if (kind != NTSYNC_TYPE_EVENT) {
            return -K_EINVAL;
        }
        U32 prev = eventSignaled ? 1 : 0;
        eventSignaled = false;
        return putU32(memory, argp, prev);
    }
    case NTSYNC_IOC_EVENT_READ:
        if (kind != NTSYNC_TYPE_EVENT) {
            return -K_EINVAL;
        }
        if (!memory->canWrite(argp, 8)) {
            return -K_EFAULT;
        }
        memory->writed(argp, eventManual ? 1 : 0);
        memory->writed(argp + 4, eventSignaled ? 1 : 0);
        return 0;
    default:
        return -K_ENOTTY;
    }
}

static void unqueue(NtsyncWait* q) {
    for (U32 i = 0; i < q->objs.size(); i++) {
        NtsyncObject* obj = q->objs[i].get();
        NtsyncEntry* entry = &q->entries[i];
        if (q->all && i < q->count) {
            obj->allWaiters.remove(entry);
        } else {
            obj->anyWaiters.remove(entry);
        }
    }
    for (auto it = ntsyncWaits.begin(); it != ntsyncWaits.end(); ++it) {
        if (*it == q) {
            ntsyncWaits.erase(it);
            break;
        }
    }
}

// First run of a wait ioctl: validate, queue on every object and try to be satisfied right away.
static U32 setupWait(KThread* thread, const std::shared_ptr<NtsyncDevice>& dev, U32 argp, bool all, NtsyncWait** result) {
    KMemory* memory = thread->memory;

    if (!memory->canRead(argp, WAIT_ARGS_SIZE)) {
        return -K_EFAULT;
    }
    U64 timeout = memory->readq(argp);
    U32 objsAddress = (U32)memory->readq(argp + 8);
    U32 count = memory->readd(argp + 16);
    U32 flags = memory->readd(argp + 24);
    U32 owner = memory->readd(argp + 28);
    U32 alert = memory->readd(argp + 32);
    U32 pad = memory->readd(argp + 36);

    if (pad || (flags & ~NTSYNC_WAIT_REALTIME)) {
        return -K_EINVAL;
    }
    if (!owner) { // Linux 6.19+ (Wine always passes its thread id)
        return -K_EINVAL;
    }
    if (count > NTSYNC_MAX_WAIT_COUNT) {
        return -K_EINVAL;
    }
    if (count && !memory->canRead(objsAddress, count * 4)) {
        return -K_EFAULT;
    }
    std::vector<S32> fds;
    for (U32 i = 0; i < count; i++) {
        fds.push_back((S32)memory->readd(objsAddress + i * 4));
    }
    if (alert) {
        fds.push_back((S32)alert);
    }

    NtsyncWait* q = new NtsyncWait();
    q->thread = thread;
    q->argp = argp;
    q->all = all;
    q->owner = owner;
    q->count = count;
    q->timeout = timeout;
    q->realtime = (flags & NTSYNC_WAIT_REALTIME) != 0;
    for (U32 i = 0; i < fds.size(); i++) {
        KFileDescriptorPtr fd = thread->process->getFileDescriptor(fds[i]);
        std::shared_ptr<NtsyncObject> obj;
        if (fd && fd->kobject->type == KTYPE_NTSYNC) {
            obj = std::static_pointer_cast<NtsyncObject>(fd->kobject);
        }
        if (!obj || obj->dev != dev) {
            delete q;
            return -K_EINVAL;
        }
        if (all) {
            for (U32 j = 0; j < i; j++) {
                if (q->objs[j] == obj) {
                    delete q;
                    return -K_EINVAL;
                }
            }
        }
        q->objs.push_back(obj);
    }
    q->entries.resize(q->objs.size());
    for (U32 i = 0; i < q->objs.size(); i++) {
        q->entries[i].q = q;
        q->entries[i].index = i;
    }

    ntsyncWaits.push_back(q);
    if (!all) {
        for (U32 i = 0; i < q->objs.size(); i++) {
            q->objs[i]->anyWaiters.push_back(&q->entries[i]);
        }
        // objects in order, the alert event last
        for (U32 i = 0; i < q->objs.size() && q->signaled.load() == -1; i++) {
            tryWakeAny(q->objs[i].get());
        }
    } else {
        for (U32 i = 0; i < count; i++) {
            q->objs[i]->allWaiters.push_back(&q->entries[i]);
        }
        if (alert) {
            q->objs[count]->anyWaiters.push_back(&q->entries[count]);
        }
        tryWakeAll(q);
        if (alert && q->signaled.load() == -1) {
            tryWakeAny(q->objs[count].get());
        }
    }
    *result = q;
    return 0;
}

static U32 finishWait(KThread* thread, NtsyncWait* q) {
    int signaled;
    bool ownerDead;
    {
        NtsyncLock lock;
        unqueue(q);
        signaled = q->signaled.load();
        ownerDead = q->ownerDead;
    }
    U32 argp = q->argp;
    delete q;
    if (signaled == -1) {
        return -K_ETIMEDOUT;
    }
    // even when the index can't be written, the objects were acquired
    if (putU32(thread->memory, argp + WAIT_ARGS_INDEX, (U32)signaled)) {
        return -K_EFAULT;
    }
    return ownerDead ? -NTSYNC_K_EOWNERDEAD : 0;
}

static U32 ntsyncWait(KThread* thread, const std::shared_ptr<NtsyncDevice>& dev, U32 argp, bool all) {
    NtsyncWait* q = nullptr;
    {
        NtsyncLock lock;
        for (NtsyncWait* w : ntsyncWaits) {
            if (w->thread == thread && w->argp == argp && w->all == all) {
                q = w;
                break;
            }
        }
        if (!q) {
            U32 result = setupWait(thread, dev, argp, all, &q);
            if (result) {
                return result;
            }
        }
    }
    while (q->signaled.load() == -1) {
        U32 waitMs = 0;
        if (q->timeout != NTSYNC_NO_TIMEOUT) {
            // the clocks clock_gettime reports for CLOCK_REALTIME and CLOCK_MONOTONIC
            U64 now = (q->realtime ? KSystem::getSystemTimeAsMicroSeconds() : KSystem::getMicroCounter()) * 1000;
            if (now >= q->timeout) {
                break;
            }
            U64 ms = (q->timeout - now + 999999) / 1000000;
            waitMs = ms > 0x7FFFFFFF ? 0x7FFFFFFF : (U32)ms;
        }
        {
            BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(q->cond);
            if (q->signaled.load() == -1) {
                // single-threaded: these return K_WAIT, and the ioctl runs again when woken
                if (q->timeout == NTSYNC_NO_TIMEOUT) {
                    BOXEDWINE_CONDITION_WAIT(q->cond);
                } else {
                    BOXEDWINE_CONDITION_WAIT_TIMEOUT(q->cond, waitMs);
                }
            }
        }
#ifdef BOXEDWINE_MULTI_THREADED
        if (thread->terminating) {
            finishWait(thread, q);
            return -K_EINTR;
        }
        if (thread->startSignal) {
            // the handler runs, then the ioctl restarts and finds this wait again
            thread->startSignal = false;
            return -K_CONTINUE;
        }
#endif
    }
    return finishWait(thread, q);
}

void ntsyncThreadExit(KThread* thread) {
    std::vector<NtsyncWait*> mine;
    {
        NtsyncLock lock;
        for (NtsyncWait* q : ntsyncWaits) {
            if (q->thread == thread) {
                mine.push_back(q);
            }
        }
        for (NtsyncWait* q : mine) {
            unqueue(q);
        }
    }
    for (NtsyncWait* q : mine) {
        delete q;
    }
}

class DevNtsync : public FsVirtualOpenNode {
public:
    DevNtsync(const std::shared_ptr<FsNode>& node, U32 flags) : FsVirtualOpenNode(node, flags), dev(std::make_shared<NtsyncDevice>()) {}

    // From FsOpenNode
    U32 ioctl(KThread* thread, U32 request) override;
    U32 readNative(U8* buffer, U32 len) override { return -K_EINVAL; }
    U32 writeNative(U8* buffer, U32 len) override { return -K_EINVAL; }

private:
    U32 create(KThread* thread, const std::shared_ptr<NtsyncObject>& obj);

    const std::shared_ptr<NtsyncDevice> dev;
};

U32 DevNtsync::create(KThread* thread, const std::shared_ptr<NtsyncObject>& obj) {
    KFileDescriptorPtr fd = thread->process->allocFileDescriptor(obj, K_O_RDWR, 0, -1, 0);
    fd->descriptorFlags |= FD_CLOEXEC;
    return fd->handle;
}

U32 DevNtsync::ioctl(KThread* thread, U32 request) {
    CPU* cpu = thread->cpu;
    KMemory* memory = thread->memory;
    U32 argp = IOCTL_ARG1;

    switch (request) {
    case NTSYNC_IOC_CREATE_SEM: {
        if (!memory->canRead(argp, 8)) {
            return -K_EFAULT;
        }
        U32 count = memory->readd(argp);
        U32 max = memory->readd(argp + 4);
        if (count > max) {
            return -K_EINVAL;
        }
        std::shared_ptr<NtsyncObject> sem = std::make_shared<NtsyncObject>(dev, NTSYNC_TYPE_SEM);
        sem->semCount = count;
        sem->semMax = max;
        return create(thread, sem);
    }
    case NTSYNC_IOC_CREATE_MUTEX: {
        if (!memory->canRead(argp, 8)) {
            return -K_EFAULT;
        }
        U32 owner = memory->readd(argp);
        U32 count = memory->readd(argp + 4);
        if (!owner != !count) {
            return -K_EINVAL;
        }
        std::shared_ptr<NtsyncObject> mutex = std::make_shared<NtsyncObject>(dev, NTSYNC_TYPE_MUTEX);
        mutex->mutexOwner = owner;
        mutex->mutexCount = count;
        return create(thread, mutex);
    }
    case NTSYNC_IOC_CREATE_EVENT: {
        if (!memory->canRead(argp, 8)) {
            return -K_EFAULT;
        }
        std::shared_ptr<NtsyncObject> event = std::make_shared<NtsyncObject>(dev, NTSYNC_TYPE_EVENT);
        event->eventManual = memory->readd(argp) != 0;
        event->eventSignaled = memory->readd(argp + 4) != 0;
        return create(thread, event);
    }
    case NTSYNC_IOC_WAIT_ANY:
        return ntsyncWait(thread, dev, argp, false);
    case NTSYNC_IOC_WAIT_ALL:
        return ntsyncWait(thread, dev, argp, true);
    default:
        return -K_ENOTTY;
    }
}

FsOpenNode* openDevNtsync(const std::shared_ptr<FsNode>& node, U32 flags, U32 data) {
    return new DevNtsync(node, flags);
}
