#pragma once

// syscall_rw.h -- internal declarations shared from syscall_rw.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "pipe.h"
#include "user_copy.h"
#include "syscall_internal.h"

namespace brook {

bool IsTerminalFd(const FdEntry* fde);
// O_ACCMODE check shared by the read paths: O_WRONLY descriptors are EBADF.
inline bool FdOpenForReading(const FdEntry* fde) { return (FdStatus(fde) & 3) != 1; }

// Backend/VFS byte results are signed int. Bound ordinary writes rather than
// narrowing a 64-bit request (which can wrap to zero or a negative result).
static constexpr uint64_t VnodeWriteLimit = 0x7ffff000ULL;

struct SockAddrIn;
int64_t ReceiveTcpToUser(int sockIdx, uint64_t address, uint64_t count, bool dontwait);
// One UDP datagram, staged in kernel memory and copied out. Returns the
// SockRecvFrom result (-EAGAIN when the queue is empty) or -EFAULT.
int64_t ReceiveUdpToUser(int sockIdx, uint64_t address, uint64_t count, SockAddrIn* source);
// Linux sockaddr output: copy min(*addrLen, actual) bytes, then store actual.
int64_t CopySockAddrToUser(uint64_t address, uint64_t addrLen, const void* value, uint32_t actual);

struct iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

// One sequenced-packet operation spans the whole vector. Snapshot the vector
// and fault/copy user pages outside the transport spinlock before publication.
struct UnixRecordVector {
    iovec* vectors=nullptr;
    char* bytes=nullptr;
    uint64_t count=0,length=0,capacity=0;
    ~UnixRecordVector(){if(vectors)kfree(vectors);if(bytes)kfree(bytes);}
    int Init(uint64_t address,uint64_t n,bool writing) {
        if(n>1024)return -EINVAL;
        count=n;vectors=static_cast<iovec*>(kmalloc(n?n*sizeof(iovec):1));
        if(!vectors)return -ENOMEM;
        if(n && !CopyFromUser(vectors,address,n*sizeof(iovec)))return -EFAULT;
        for(uint64_t i=0;i<n;++i) {
            if(vectors[i].iov_len>0x7fffffffULL-length)return -EINVAL;
            length+=vectors[i].iov_len;
        }
        if(writing && length>=PIPE_BUF_UNIX_SIZE)return -90;
        capacity=length<PIPE_BUF_UNIX_SIZE?length:PIPE_BUF_UNIX_SIZE-1;
        bytes=static_cast<char*>(kmalloc(capacity?capacity:1));if(!bytes)return -ENOMEM;
        uint64_t done=0;
        for(uint64_t i=0;i<n && done<capacity;++i) {
            uint64_t part=vectors[i].iov_len;if(part>capacity-done)part=capacity-done;
            if(part) {
                if(writing) {if(!CopyFromUser(bytes+done,vectors[i].iov_base,part))return -EFAULT;}
                else if(!UserBufferWritable(vectors[i].iov_base,part))return -EFAULT;
            }
            done+=part;
        }
        return 0;
    }
    int Scatter(uint64_t size) {
        uint64_t done=0;
        for(uint64_t i=0;i<count && done<size;++i) {
            uint64_t part=vectors[i].iov_len;if(part>size-done)part=size-done;
            if(part && !CopyToUser(vectors[i].iov_base,bytes+done,part))return -EFAULT;
            done+=part;
        }
        return 0;
    }
};

} // namespace brook
