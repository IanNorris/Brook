#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <map>
#include <vector>
#include <algorithm>

struct iovec { uint64_t iov_base,iov_len; };
struct UnixSocketData {};
struct FdEntry { void* handle;uint32_t statusFlags; };
static constexpr uint64_t PIPE_BUF_UNIX_SIZE=65536;
static std::map<uint64_t,uint64_t> allocations,readable;
static uint64_t copiedBytes,copyCalls,allocationCalls,failAllocation;
static int transportCalls;
static bool transportNonblock;
static int64_t transportError;
static uint64_t transportLimit;
static std::vector<char> delivered;
static iovec* mutateAfterSnapshot;
static void* kmalloc(uint64_t size) {
    if(++allocationCalls==failAllocation)return nullptr;
    void* p=std::malloc(size);assert(p);allocations[reinterpret_cast<uint64_t>(p)]=size;return p;
}
static void kfree(void* p) { assert(allocations.erase(reinterpret_cast<uint64_t>(p))==1);std::free(p); }
static bool contains(const std::map<uint64_t,uint64_t>& ranges,uint64_t address,uint64_t length) {
    auto it=ranges.upper_bound(address);if(it==ranges.begin())return false;--it;
    return address>=it->first && address-it->first<=it->second && length<=it->second-(address-it->first);
}
static bool CopyFromUser(void* target,uint64_t address,uint64_t length) {
    ++copyCalls;
    assert(contains(allocations,reinterpret_cast<uint64_t>(target),length));
    if(!contains(readable,address,length))return false;
    std::memcpy(target,reinterpret_cast<void*>(address),length);copiedBytes+=length;
    if(mutateAfterSnapshot && address==reinterpret_cast<uint64_t>(mutateAfterSnapshot)) {
        mutateAfterSnapshot->iov_base=1;mutateAfterSnapshot->iov_len=~uint64_t{0};mutateAfterSnapshot=nullptr;
    }
    return true;
}
static int64_t UnixSocketWriteSome(UnixSocketData*,bool nonblock,const char* bytes,uint64_t length) {
    ++transportCalls;transportNonblock=nonblock;
    assert(contains(allocations,reinterpret_cast<uint64_t>(bytes),length));
    if(transportError)return transportError;
    if(transportLimit && length>transportLimit)length=transportLimit;
    delivered.assign(bytes,bytes+length);return length;
}
#include "../../kernel/src/unix_stream_vector.inc"

static void reset() {
    assert(allocations.empty());readable.clear();delivered.clear();copiedBytes=copyCalls=allocationCalls=failAllocation=0;
    transportCalls=0;transportNonblock=false;transportError=0;transportLimit=0;mutateAfterSnapshot=nullptr;
}
static void allow(void* p,uint64_t bytes) { readable[reinterpret_cast<uint64_t>(p)]=bytes; }
static int64_t run(FdEntry& fd,std::vector<iovec>& vectors) {
    allow(vectors.data(),vectors.size()*sizeof(iovec));
    auto result=UnixStreamWriteVector(&fd,reinterpret_cast<uint64_t>(vectors.data()),vectors.size());
    assert(allocations.empty());return result;
}
int main() {
    UnixSocketData socket;FdEntry fd{&socket,0x800};std::vector<char> a(16,'H'),b(65536,'B');
    reset();allow(a.data(),a.size());allow(b.data(),b.size());
    std::vector<iovec> v{{reinterpret_cast<uint64_t>(a.data()),a.size()},{reinterpret_cast<uint64_t>(b.data()),8*1024*1024}};
    assert(run(fd,v)==65535 && transportCalls==1 && transportNonblock);
    assert(delivered.size()==65535 && std::all_of(delivered.begin(),delivered.begin()+16,[](char c){return c=='H';}));
    assert(std::all_of(delivered.begin()+16,delivered.end(),[](char c){return c=='B';}));
    assert(copiedBytes==sizeof(iovec)*2+65535); // Unreadable remaining tail was never touched.
    reset();allow(a.data(),a.size());allow(b.data(),b.size());transportLimit=17;
    assert(run(fd,v)==17 && delivered.size()==17 && delivered[16]=='B' && transportCalls==1);
    reset();allow(b.data(),b.size());v={{reinterpret_cast<uint64_t>(b.data()),0x100000000ULL}};
    assert(run(fd,v)==65535); // Large stream lengths do not imply large staging allocations.
    reset();allow(a.data(),a.size());v={{reinterpret_cast<uint64_t>(a.data()),a.size()}};mutateAfterSnapshot=v.data();
    assert(run(fd,v)==16 && delivered==a); // User mutation cannot change the snapshotted vector.
    reset();v={{1,16}};assert(run(fd,v)==-EFAULT && !transportCalls);
    reset();allow(a.data(),a.size());v={{reinterpret_cast<uint64_t>(a.data()),16},{1,16}};
    assert(run(fd,v)==-EFAULT && !transportCalls); // Failed staging never publishes partial kernel garbage.
    reset();v={{1,0},{2,0}};assert(run(fd,v)==0 && !transportCalls && allocationCalls==1);
    reset();assert(UnixStreamWriteVector(&fd,1,0)==0 && !allocationCalls);
    assert(UnixStreamWriteVector(&fd,1,1025)==-EINVAL && !allocationCalls);
    assert(UnixStreamWriteVector(&fd,1,1)==-EFAULT && allocations.empty());
    reset();v={{1,0x7fffffffffffffffULL},{1,1}};assert(run(fd,v)==-EINVAL && !transportCalls);
    for(uint64_t fail:{1,2}) {
        reset();allow(a.data(),a.size());v={{reinterpret_cast<uint64_t>(a.data()),a.size()}};failAllocation=fail;
        assert(run(fd,v)==-ENOMEM && !transportCalls);
    }
    reset();allow(a.data(),a.size());v={{reinterpret_cast<uint64_t>(a.data()),a.size()}};transportError=-EAGAIN;
    assert(run(fd,v)==-EAGAIN && transportCalls==1 && transportNonblock);
    reset();fd.statusFlags=0;allow(a.data(),a.size());v={{reinterpret_cast<uint64_t>(a.data()),a.size()}};
    assert(run(fd,v)==16 && !transportNonblock);
}
