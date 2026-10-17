// Actual record-lock intervals and logical FD close hooks with Linux-style
// owner identities; allocator and inode access are the only platform stubs.
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>
#include <random>
#include "record_locks.h"
#include "file_locks.h"
#include "process.h"
#include "vfs.h"
#include "memory/file_object.h"
using namespace brook;
#define CHECK(c) do { if(!(c)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort(); } } while(0)
static std::atomic<int> allocations{0};
static int failAfter=-1;
namespace brook {
extern "C" void* kmalloc(uint64_t size) {
    if(failAfter==0)return nullptr;
    if(failAfter>0)--failAfter;
    auto* result=std::malloc(size);if(result)++allocations;return result;
}
extern "C" void kfree(void* memory) { if(memory) { --allocations;std::free(memory); } }
}
constexpr RecordLockOwner a{101,1},b{102,2},c{103,3};
static int set(RecordLockTable& table,RecordLockOwner owner,uint64_t begin,uint64_t end,unsigned type) {
    RecordLockPlan plan;CHECK(plan.Prepare(type==2?1:2));return plan.Set(table,owner,{begin,end},type);
}
static void release(RecordLockTable& table,RecordLockOwner owner) {
    RecordLockNode* retired=nullptr;RecordLockReleaseOwner(table,owner,retired);RecordLockFree(retired);
}
static bool query(RecordLockTable& table,RecordLockOwner owner,uint64_t begin,uint64_t end,unsigned type) {
    RecordLockNode conflict{};return RecordLockQuery(table,owner,{begin,end},type,&conflict);
}
static void intervals() {
    RecordLockTable table{};
    CHECK(set(table,a,0,99,0)==0);CHECK(set(table,b,0,99,0)==0);
    CHECK(set(table,c,10,20,1)==-11);
    CHECK(set(table,a,10,20,1)==-11);release(table,b);
    CHECK(set(table,a,10,20,1)==0);CHECK(query(table,b,10,20,0));
    CHECK(!query(table,b,0,9,0));CHECK(!query(table,a,0,99,1));
    CHECK(set(table,a,13,17,2)==0);CHECK(!query(table,b,13,17,1));
    CHECK(query(table,b,12,12,0));CHECK(query(table,b,18,18,0));
    CHECK(set(table,a,0,99,0)==0);CHECK(table.head && !table.head->next);
    CHECK(set(table,a,100,199,0)==0);CHECK(table.head && !table.head->next);
    RecordLockNode conflict{};
    CHECK(RecordLockQuery(table,b,{40,40},1,&conflict));
    CHECK(conflict.owner==a && conflict.range.begin==0 && conflict.range.end==199);
    CHECK(!query(table,{999,a.pid},0,0,0));CHECK(query(table,{999,a.pid},0,0,1));
    release(table,a);CHECK(!table.head);
    constexpr uint64_t max=0x7fffffffffffffffULL;
    CHECK(set(table,a,0,max,1)==0);CHECK(set(table,a,1,max-1,2)==0);
    CHECK(query(table,b,0,0,1));CHECK(query(table,b,max,max,1));CHECK(!query(table,b,1,max-1,1));
    release(table,a);
    for(unsigned i=0;i<4096;++i)CHECK(set(table,a,2*i,2*i,0)==0);
    CHECK(set(table,a,0,8191,1)==0);CHECK(table.head && !table.head->next);
    release(table,a);CHECK(!table.head);
}
static void normalization() {
    RecordLockRange r{};constexpr uint64_t max=0x7fffffffffffffffULL;
    CHECK(!RecordLockNormalize(0,10,-5,0,0,&r) && r.begin==5 && r.end==9);
    CHECK(!RecordLockNormalize(1,-4,2,9,0,&r) && r.begin==5 && r.end==6);
    CHECK(!RecordLockNormalize(2,-4,0,0,9,&r) && r.begin==5 && r.end==max);
    CHECK(RecordLockNormalize(0,0,-1,0,0,&r)==-22);
    CHECK(RecordLockNormalize(0,max,2,0,0,&r)==-75);
    CHECK(RecordLockNormalize(1,1,1,max,0,&r)==-75);
    CHECK(RecordLockNormalize(3,0,0,0,0,&r)==-22);
}
static void oracle() {
    RecordLockTable table{};int held[4][64]{};std::mt19937 random(71823);
    for(unsigned round=0;round<10000;++round) {
        unsigned owner=random()%4,start=random()%64,end=start+random()%(64-start),type=random()%3;
        bool blocked=false;
        if(type!=2)for(unsigned other=0;other<4;++other)if(other!=owner)
            for(unsigned offset=start;offset<=end;++offset)
                if(held[other][offset] && (held[other][offset]==2 || type==1))blocked=true;
        CHECK(set(table,{owner+1,static_cast<uint16_t>(owner+1)},start,end,type)==(blocked?-11:0));
        if(!blocked)for(unsigned offset=start;offset<=end;++offset)held[owner][offset]=type==2?0:type+1;
        for(unsigned offset=0;offset<64;++offset) {
            bool expected=false;
            for(unsigned other=0;other<4;++other)if(other!=owner && held[other][offset])expected=true;
            CHECK(query(table,{owner+1,static_cast<uint16_t>(owner+1)},offset,offset,1)==expected);
        }
        for(auto* x=table.head;x;x=x->next)for(auto* y=x->next;y;y=y->next)
            if(x->owner==y->owner)CHECK(x->range.end<y->range.begin || y->range.end<x->range.begin);
    }
    for(unsigned owner=0;owner<4;++owner)release(table,{owner+1,static_cast<uint16_t>(owner+1)});
}
static void allocationFailure() {
    RecordLockTable table{};CHECK(!set(table,a,0,99,0));
    for(int fail=0;fail<2;++fail) {
        failAfter=fail;
        { RecordLockPlan plan;CHECK(!plan.Prepare(2)); }
        failAfter=-1;
        CHECK(table.head && !table.head->next && table.head->range.end==99 && table.head->type==0);
    }
    { RecordLockPlan plan;CHECK(plan.Set(table,a,{10,20},1)==-37); }
    CHECK(table.head && !table.head->next && table.head->range.end==99);
    release(table,a);
}
static FileObject* inode(Vnode* vnode) { return static_cast<FileObject*>(vnode->priv); }
static void concurrentOwners() {
    RecordLockTable table{};std::atomic<unsigned> active{0};std::vector<std::thread> threads;
    for(unsigned id=1;id<=8;++id)threads.emplace_back([&,id] {
        RecordLockOwner owner{id,static_cast<uint16_t>(id)};
        for(unsigned round=0;round<200;++round) {
            int result;
            do { result=set(table,owner,0,99,1);if(result)std::this_thread::yield(); } while(result==-11);
            CHECK(result==0);CHECK(active.fetch_add(1)==0);std::this_thread::yield();
            CHECK(active.fetch_sub(1)==1);CHECK(!set(table,owner,0,99,2));
        }
    });
    for(auto& thread:threads)thread.join();CHECK(!table.head && active==0);
}
static void fdLifecycle() {
    static Process process{};process.pid=1;process.tgid=1;process.incarnation=101;
    static FdEntry fds[MAX_FDS]{};SpinLock lock{};FileObject object{},other{};
    VnodeOps ops{};ops.file_object=inode;
    Vnode first{&ops,VnodeType::File,&object,1,0},second{&ops,VnodeType::File,&object,1,0};
    Vnode distinct{&ops,VnodeType::File,&other,1,0};
    int fd=FdTableAlloc(fds,&lock,FdType::Vnode,&first);
    int independent=FdTableAlloc(fds,&lock,FdType::Vnode,&second);CHECK(fd>=0 && independent>=0);
    CHECK(!set(object.recordLocks,a,0,99,1));CHECK(FdTablePin(fds,&lock,independent));
    { FileLockCloseContext context(&process);FdClaimResult result{};
      CHECK(FdTableClose(fds,&lock,independent,&result,FileLockCloseContext::Closing,&context)==FdCloseResult::Deferred); }
    CHECK(!query(object.recordLocks,b,0,99,1));
    CHECK(!set(object.recordLocks,a,0,99,1));
    FdClaimResult retired{};CHECK(FdTableUnpin(fds,&lock,independent,&retired));
    CHECK(query(object.recordLocks,b,0,99,1)); // Final unpin must not release again.
    int otherFd=FdTableAlloc(fds,&lock,FdType::Vnode,&distinct);
    { FileLockCloseContext context(&process);
      CHECK(FdTableClaim(fds,&lock,otherFd,&retired,FileLockCloseContext::Closing,&context)); }
    CHECK(query(object.recordLocks,b,0,99,1));
    independent=FdTableAlloc(fds,&lock,FdType::Vnode,&second);
    FdEntry owned=fds[fd];
    { FileLockCloseContext context(&process);
      CHECK(FdTablePublishOwned(fds,&lock,owned,0,independent,&retired,nullptr,
                                FileLockCloseContext::Closing,&context)==independent); }
    CHECK(!query(object.recordLocks,b,0,99,1));
    CHECK(!set(object.recordLocks,a,0,99,1));
    static Process child{};child.pid=2;child.tgid=2;child.incarnation=102;
    { FileLockCloseContext context(&child);FileLockCloseContext::Closing(fds[fd],&context); }
    CHECK(query(object.recordLocks,b,0,99,1)); // Fork child close is not parent close.
    CHECK(FdTablePin(fds,&lock,fd));
    { FileLockCloseContext context(&process);
      CHECK(FdTableClose(fds,&lock,fd,&retired,FileLockCloseContext::Closing,&context)==FdCloseResult::Deferred); }
    CHECK(!set(object.recordLocks,a,0,99,1));
    { FileLockCloseContext context(&process);
      CHECK(FdTableClaim(fds,&lock,fd,&retired,FileLockCloseContext::Closing,&context)); }
    CHECK(query(object.recordLocks,b,0,99,1)); // Forced reclaim of already-closed slot is not another close.
    release(object.recordLocks,a);
}
int main() {
    normalization();intervals();oracle();allocationFailure();fdLifecycle();concurrentOwners();
    CHECK(allocations==0);std::puts("record locks: intervals/oracle/allocation/identity/logical-close PASS");
}
