#pragma once

#include <stdint.h>

namespace brook {
struct Process;
struct IrqSignalFrame;

// Linux x86-64 NT_PRSTATUS register order.
struct CoreRegisters {
    uint64_t r15,r14,r13,r12,rbp,rbx,r11,r10,r9,r8,rax,rcx,rdx,rsi,rdi;
    uint64_t origRax,rip,cs,rflags,rsp,ss,fsBase,gsBase,ds,es,fs,gs;
};
static_assert(sizeof(CoreRegisters)==216);

struct CoreRegisterSample {
    uint64_t sequence, addressSpace;
    CoreRegisters registers;
};

struct CoreThreadNote {
    int32_t signal[3];
    int16_t currentSignal;
    uint64_t pending,blocked;
    int32_t pid,ppid,pgrp,sid;
    uint64_t time[8];
    CoreRegisters registers;
    int32_t fpValid;
};
static_assert(__builtin_offsetof(CoreThreadNote,registers)==112 && sizeof(CoreThreadNote)==336);

struct CoreProcessNote {
    char state,stateName,zombie,nice;
    uint64_t flags;
    uint32_t uid,gid;
    int32_t pid,ppid,pgrp,sid;
    char name[16],args[80];
};
static_assert(sizeof(CoreProcessNote)==136);

struct CoreDumpResult {
    uint64_t bytes=0,holes=0,durationMs=0;
    uint32_t threads=0,missingRegisters=0,mappings=0;
    char executable[256]{};
    const char* stage="snapshot";
};

void CoreDumpPublishRegisters(Process*,const CoreRegisters&);
void CoreDumpCaptureIrq(Process*,const IrqSignalFrame&);
bool CoreDumpReadRegisters(const Process*,uint64_t addressSpace,CoreRegisters&);
int CoreDumpProcess(uint16_t pid,const char* path,bool full,CoreDumpResult&);
}
