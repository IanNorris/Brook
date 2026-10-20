#include "apic.h"
#include "mapping_fault_test.h"
#include "process.h"
#include "scheduler.h"
#include "spinlock.h"

namespace brook {
uint64_t MappingRendererTestEpoch();
namespace {
IrqSpinLock lock;
uint64_t identity,begin,end,epoch;
uint32_t selectedPhase,quota,arrivals,waiting;
bool armed,released,timedOut;
}

// Private test ABI, syscall 0xB30, absent from ordinary kernels. Scalar arguments
// only. Arm selects the caller's AS identity, never a raw retained task pointer.
// Query/release are global so a parent can cancel a child stopped in its fault.
int64_t MappingFaultTestControl(uint64_t action,uint64_t address,uint64_t length,
                                uint64_t phase,uint64_t count,uint64_t) {
    auto* actor=ProcessCurrent();
    if (!actor || actor->euid!=0) return -1;
    const uint64_t lockFlags=IrqSpinLockAcquire(&lock);
    int64_t result=-22;
    if (action==0) {
        if (waiting) result=-16;
        else if (length && !(address&4095) && !(length&4095) &&
                 address<0x0000800000000000ULL && length<=0x0000800000000000ULL-address &&
                 (phase>=1 && phase<=4) && count && count<=8) {
            identity=actor->addressSpace->identity;begin=address;end=address+length;
            selectedPhase=phase;quota=count;arrivals=0;released=false;timedOut=false;armed=true;
            ++epoch;result=0;
        }
    } else if (action==1) {
        result=arrivals | (uint64_t{waiting}<<16) | (uint64_t{timedOut}<<32);
    } else if (action==2) {
        armed=false;released=true;result=0;
    } else if (action==3) {
        result=static_cast<int64_t>(MappingRendererTestEpoch());
    }
    IrqSpinLockRelease(&lock,lockFlags);
    return result;
}

static bool Barrier(uint64_t spaceIdentity,uint64_t page,uint32_t phase) {
    const uint64_t lockFlags=IrqSpinLockAcquire(&lock);
    if (!armed || (phase!=3 && (identity!=spaceIdentity || page<begin || page>=end)) ||
        selectedPhase!=phase || arrivals==quota) {
        IrqSpinLockRelease(&lock,lockFlags);return true;
    }
    const uint64_t observedEpoch=epoch;
    ++arrivals;++waiting;
    IrqSpinLockRelease(&lock,lockFlags);
    // A CPU #PF enters with IF clear. This intentionally sleeping test point
    // owns no AS/PT/backing lock; allow the BSP timer and peer/controller work
    // to proceed. Restore IF before returning to the real exception handler.
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; sti":"=r"(flags)::"memory");
    const uint64_t start=__atomic_load_n(&ApicTickCount(),__ATOMIC_RELAXED);
    for (;;) {
        const uint64_t waitFlags=IrqSpinLockAcquire(&lock);
        const bool expired=__atomic_load_n(&ApicTickCount(),__ATOMIC_RELAXED)-start>=10000;
        if (released || observedEpoch!=epoch || expired) {
            --waiting;
            if (expired) { timedOut=true;armed=false;released=true; }
            IrqSpinLockRelease(&lock,waitFlags);
            if (!(flags&512)) __asm__ volatile("cli":::"memory");
            return !expired;
        }
        IrqSpinLockRelease(&lock,waitFlags);
        SchedulerYield();
        // Like the existing sleeping-lock/cancellation loops, the caller must
        // restore IF after a context switch: SchedulerYield saves it clear.
        __asm__ volatile("sti":::"memory");
    }
}
bool MappingFaultTestBarrier(AddressSpaceState* space,uint64_t page,uint32_t phase) {
    return Barrier(space->identity,page,phase);
}
bool MappingRendererTestBarrier() { return Barrier(0,0,3); }
}
