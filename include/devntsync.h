#ifndef __DEVNTSYNC_H__
#define __DEVNTSYNC_H__

// /dev/ntsync: Linux's NT synchronization driver (drivers/misc/ntsync.c, uapi linux/ntsync.h, Linux 6.14+),
// which Wine 10+ uses for in-process waits when built with linux/ntsync.h.
FsOpenNode* openDevNtsync(const std::shared_ptr<FsNode>& node, U32 flags, U32 data);

// A thread that exits while blocked in a wait leaves its queue entries behind: drop them.
void ntsyncThreadExit(KThread* thread);

#endif
