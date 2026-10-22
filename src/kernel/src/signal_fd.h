#pragma once
#include <stdint.h>
#include <stddef.h>
namespace brook {
struct Process;
struct SignalFd { volatile uint32_t references; uint64_t mask; };
struct SignalFdInfo {
    uint32_t signo; int32_t error,code; uint32_t pid,uid; int32_t fd;
    uint32_t tid,band,overrun,trap; int32_t status,value;
    uint64_t pointer,userTime,systemTime,address;
    uint16_t addressLsb,padding; int32_t syscall;
    uint64_t callAddress; uint32_t architecture; uint8_t reserved[28];
};
static_assert(sizeof(SignalFdInfo)==128 && offsetof(SignalFdInfo,pointer)==48);
inline uint64_t SignalFdMask(uint64_t mask) {return mask & ~((1ULL<<8)|(1ULL<<18));}
inline SignalFdInfo SignalFdEncode(int32_t signo,int32_t error,int32_t code,const uint8_t* payload) {
    SignalFdInfo out{};out.signo=uint32_t(signo);out.error=error;out.code=code;
    if(code==-2) { // SI_TIMER: timer ID, overrun, sigval
        __builtin_memcpy(&out.tid,payload,4);__builtin_memcpy(&out.overrun,payload+4,4);
        __builtin_memcpy(&out.pointer,payload+8,8);out.value=int32_t(out.pointer);
    } else if(signo==17 && code>0 && code<=6) { // CLD_*
        __builtin_memcpy(&out.pid,payload,4);__builtin_memcpy(&out.uid,payload+4,4);
        __builtin_memcpy(&out.status,payload+8,4);
        __builtin_memcpy(&out.userTime,payload+16,8);__builtin_memcpy(&out.systemTime,payload+24,8);
    } else {
        __builtin_memcpy(&out.pid,payload,4);__builtin_memcpy(&out.uid,payload+4,4);
        if(code==-1) {__builtin_memcpy(&out.pointer,payload+8,8);out.value=int32_t(out.pointer);}
    }
    return out;
}
SignalFd* SignalFdCreate(uint64_t mask);
void SignalFdRef(SignalFd*);
void SignalFdUnref(SignalFd*);
void SignalFdSetMask(SignalFd*,uint64_t);
uint32_t SignalFdReady(Process*,SignalFd*,uint32_t events);
int64_t SignalFdRead(Process*,SignalFd*,uint64_t destination,uint64_t count,bool nonblocking);
}
