#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include "../../kernel/src/spinlock.h"
#include "../../kernel/src/socket_identity.h"
#include "../../kernel/src/netlink_route.h"
namespace brook {
struct Process {uint32_t tgid=101,refs=1;uint64_t wakeupTick=0;std::mutex mutex;std::condition_variable changed;bool wake=false;std::atomic<bool> signal{false};};
thread_local Process* current;
Process* ProcessCurrent(){assert(current);return current;}
void ProcessRef(Process* p){__atomic_add_fetch(&p->refs,1,__ATOMIC_RELAXED);}
void ProcessUnref(Process* p){assert(__atomic_fetch_sub(&p->refs,1,__ATOMIC_RELAXED)>1);}
void WakeProcess(Process* p){std::lock_guard<std::mutex> held(p->mutex);p->wake=true;p->changed.notify_all();}
void SchedulerBlockInterruptible(Process* p){std::unique_lock<std::mutex> held(p->mutex);assert(p->changed.wait_for(held,std::chrono::seconds(2),[&]{return p->wake;}));p->wake=false;}
bool HasPendingSignals(){return current->signal.load();}
uint64_t ApicTickCount(){return 0;}
struct KernelOperationGuard {explicit KernelOperationGuard(Process*){}};
std::mutex heapLock;std::unordered_set<void*> allocated;std::atomic<bool> failAllocation{false};
void* kmalloc(uint64_t n){if(failAllocation.exchange(false))return nullptr;void* p=malloc(n);assert(p);std::lock_guard<std::mutex> held(heapLock);assert(allocated.insert(p).second);return p;}
void kfree(void* p){if(!p)return;{std::lock_guard<std::mutex> held(heapLock);assert(allocated.erase(p)==1);}free(p);}
SpinLock* watchedLock=nullptr;
void checkUserCopy(){if(watchedLock){assert(SpinLockTryAcquire(watchedLock));SpinLockRelease(watchedLock);}}
bool CopyFromUser(void* dst,uint64_t src,uint64_t n){checkUserCopy();if(n && !src)return false;memcpy(dst,reinterpret_cast<void*>(src),n);return true;}
bool CopyToUser(uint64_t dst,const void* src,uint64_t n){checkUserCopy();if(n && !dst)return false;memcpy(reinterpret_cast<void*>(dst),src,n);return true;}
bool UserBufferWritable(uint64_t p,uint64_t n){checkUserCopy();return !n || p;}
uint32_t NetInterfaceViews(InterfaceView* out,uint32_t capacity){assert(capacity);out[0]={1,0x49,65536,0x0100007f,0xff,772,{},"lo"};return 1;}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../kernel/src/syscall_netlink.inc"
#pragma GCC diagnostic pop
NetlinkSocket* make(){auto* s=static_cast<NetlinkSocket*>(kmalloc(sizeof(NetlinkSocket)));*s=NetlinkSocket{};SpinLockAcquire(&g_netlinkRegistryLock);s->next=g_netlinkSockets;g_netlinkSockets=s;SpinLockRelease(&g_netlinkRegistryLock);return s;}
}
int main(){using namespace brook;Process process;current=&process;
 auto* socket=make();auto* other=make();assert(!NetlinkBind(socket,500));assert(NetlinkBind(other,500)==-98);assert(!NetlinkBind(other,0));assert(other->port!=socket->port);
 struct Request {RouteMessage h;uint8_t family;} request{{17,18,0x301,88,0},0};
 auto send=[&]{return NetlinkSend(socket,reinterpret_cast<uint64_t>(&request),17,0);};
 uint8_t bytes[8192];uint32_t full=0;
 assert(NetlinkReceive(socket,bytes,sizeof(bytes),0x40,full)==-EAGAIN);
 watchedLock=&socket->lock;assert(send()==17);assert(NetlinkReady(socket,1|4)==5);
 uint32_t firstSize=NetlinkAvailable(socket);assert(firstSize>7);
 assert(NetlinkReceive(socket,bytes,7,2,full)==7 && full==firstSize && NetlinkAvailable(socket)==firstSize);
 assert(NetlinkReceive(socket,bytes,7,0,full)==7 && !NetlinkAvailable(socket));watchedLock=nullptr;
 assert(NetlinkReady(socket,1|4)==4);
 // Account actual allocation size, including short error replies. Failed
 // admission leaves the prior queue intact and frees both staging buffers.
 request.h.type=24;unsigned accepted=0;
 while(send()==17)++accepted;
 assert(accepted==65536/sizeof(NetlinkPacket) && socket->queued==accepted*sizeof(NetlinkPacket));
 const size_t live=allocated.size();assert(send()==-105 && allocated.size()==live);
 socket->receiveLimit=1;assert(send()==-105);socket->receiveLimit=65536;
 while(NetlinkReceive(socket,bytes,sizeof(bytes),0x40,full)>=0){}assert(!socket->queued);
 failAllocation=true;assert(send()==-ENOMEM && !socket->head);
 request.h.type=18;
 // A poll registration owns both task and endpoint refs; dropping a descriptor
 // alias cannot free it. Notification before the blocking call stays pending.
 {NetlinkWaitGroup wait;assert(wait.add(socket,&process));assert(wait.add(socket,&process));assert(socket->refs==2 && process.refs==2);
  assert(send()==17);SchedulerBlockInterruptible(&process);assert(NetlinkReceive(socket,bytes,sizeof(bytes),0,full)>0);
 }assert(socket->refs==1 && process.refs==1 && !socket->waiters);
 // Receive's own registration closes arrival-before-block and unwinds on a
 // signal. Hold one explicit alias while a reader is blocked.
 NetlinkRef(socket);std::thread writer([&]{current=&process;std::this_thread::sleep_for(std::chrono::milliseconds(20));assert(send()==17);});
 assert(NetlinkReceive(socket,bytes,sizeof(bytes),0,full)>0);writer.join();NetlinkUnref(socket);assert(socket->refs==1 && process.refs==1 && !socket->waiters);
 std::thread interrupt([&]{std::this_thread::sleep_for(std::chrono::milliseconds(20));process.signal=true;WakeProcess(&process);});
 assert(NetlinkReceive(socket,bytes,sizeof(bytes),0,full)==-EINTR);interrupt.join();process.signal=false;assert(!socket->waiters && process.refs==1);
 assert(NetlinkSend(socket,reinterpret_cast<uint64_t>(&request),16,0)==-EINVAL);
 NetlinkUnref(socket);auto* replacement=make();assert(!NetlinkBind(replacement,500) && replacement->port==500);NetlinkUnref(replacement);NetlinkUnref(other);assert(!g_netlinkSockets && allocated.empty());
 puts("netlink endpoint ownership, queue pressure, readiness, signal and alias checks passed");
}
