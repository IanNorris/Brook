#pragma once
#include "spinlock.h"

namespace brook {
struct RecordLockOwner {
    uint64_t incarnation;
    uint16_t pid;
    bool operator==(const RecordLockOwner& other) const {
        return pid==other.pid && incarnation==other.incarnation;
    }
};
// Inclusive offsets in [0, INT64_MAX]; an end of INT64_MAX covers future EOF.
struct RecordLockRange { uint64_t begin,end; };
struct RecordLockNode {
    RecordLockNode* next;
    RecordLockOwner owner;
    RecordLockRange range;
    uint16_t type; // Linux F_RDLCK=0, F_WRLCK=1
};
struct RecordLockTable {
    SpinLock lock;
    RecordLockNode* head=nullptr;
};
// No allocation, sleeping, filesystem access or callbacks while holding lock.
// The containing inode must be retained for the entire operation.
bool RecordLockQuery(RecordLockTable& table,RecordLockOwner owner,
                     RecordLockRange range,uint16_t type,RecordLockNode* conflict);
// Detach all owner's locks at logical close, not deferred resource finalization.
// Append to caller's retired list; release memory only after dropping FD locks.
void RecordLockReleaseOwner(RecordLockTable& table,RecordLockOwner owner,RecordLockNode*& retired);
void RecordLockFree(RecordLockNode* nodes);
// At most one old range splits, plus one new interval. Reserve before FD lock.
class RecordLockPlan {
    RecordLockNode* spare=nullptr;
    RecordLockNode* retired=nullptr;
public:
    RecordLockPlan()=default;
    RecordLockPlan(const RecordLockPlan&)=delete;
    ~RecordLockPlan();
    bool Prepare(unsigned count);
    // 0 on commit, -11 conflict, -37 insufficient prepared storage. Atomic on
    // failure. Caller serializes against logical close with its FD table lock.
    int Set(RecordLockTable& table,RecordLockOwner owner,RecordLockRange range,uint16_t type);
};
// Validate and normalize Linux SEEK_* offsets, including backwards lengths.
int RecordLockNormalize(int16_t whence,int64_t start,int64_t length,
                        uint64_t position,uint64_t size,RecordLockRange* range);
}
