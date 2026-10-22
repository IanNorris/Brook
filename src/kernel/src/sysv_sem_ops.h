#pragma once
#include <stdint.h>

namespace brook {
struct SysvSemOp { uint16_t number; int16_t delta; uint16_t flags; };
static_assert(sizeof(SysvSemOp)==6,"Linux x86-64 sembuf ABI");
enum class SysvSemOpState { Ready, Wait, Error };
struct SysvSemOpResult {
    SysvSemOpState state;
    int error=0;
    uint16_t waitSemaphore=0;
    bool waitZero=false;
};
inline constexpr uint16_t SysvSemNowait=04000, SysvSemUndo=010000;
inline constexpr int SysvSemMaxValue=32767, SysvSemMaxAdjustment=32767;

// Coarse scheduler wake hint only; callers recheck the absolute ns deadline.
inline uint64_t SysvSemWakeTick(uint64_t deadline,uint64_t now,uint64_t tick) {
    if(deadline==~uint64_t{0})return deadline;
    const uint64_t remaining=deadline>now?deadline-now:0;
    const uint64_t milliseconds=remaining/1000000ULL+(remaining%1000000ULL!=0);
    return milliseconds>=~uint64_t{0}-tick?~uint64_t{0}-1:tick+milliseconds;
}

// Preview an entire transaction into caller-owned scratch. The original set
// and undo ledger remain untouched on every path. Only Ready permits commit.
// Object identity, permissions, user copying, wait registration and process
// undo-group lifetime belong to the service calling this arithmetic core.
inline SysvSemOpResult SysvSemPreview(const uint16_t* values,unsigned count,
    const int32_t* undo,const SysvSemOp* ops,unsigned operations,
    uint16_t* proposed,int32_t* proposedUndo) {
    if(!values || !count || !ops || !operations || !proposed || !proposedUndo ||
       values==proposed || (undo && undo==proposedUndo))
        return {SysvSemOpState::Error,22};
    // Linux checks the complete set of indices before attempting operations.
    for(unsigned i=0;i<operations;++i)
        if(ops[i].number>=count)return {SysvSemOpState::Error,27};
    for(unsigned i=0;i<count;++i) {
        if(values[i]>SysvSemMaxValue)return {SysvSemOpState::Error,22};
        proposed[i]=values[i];proposedUndo[i]=undo?undo[i]:0;
    }
    for(unsigned i=0;i<operations;++i) {
        const auto& op=ops[i];const int current=proposed[op.number];
        const bool blocked=op.delta==0?current!=0:op.delta<0 && current < -int(op.delta);
        if(blocked) {
            if(op.flags&SysvSemNowait)return {SysvSemOpState::Error,11};
            return {SysvSemOpState::Wait,0,op.number,op.delta==0};
        }
        const int next=current+int(op.delta);
        if(next>SysvSemMaxValue)return {SysvSemOpState::Error,34};
        if(op.flags&SysvSemUndo) {
            const int64_t adjustment=int64_t(proposedUndo[op.number])-op.delta;
            if(adjustment < -SysvSemMaxAdjustment || adjustment > SysvSemMaxAdjustment)
                return {SysvSemOpState::Error,34};
            proposedUndo[op.number]=int32_t(adjustment);
        }
        proposed[op.number]=uint16_t(next);
    }
    return {SysvSemOpState::Ready};
}
} // namespace brook
