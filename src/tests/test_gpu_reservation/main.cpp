#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../kernel/src/gpu_reservation.h"
using brook::GpuReservation;
int main() {
    GpuReservation r{};assert(r.Ready());
    assert(!r.ImportCompleted(0) && !r.ImportCompleted(4));
    assert(r.ImportCompleted(1) && r.imports==1 && !r.completedWrite);
    assert(r.ImportCompleted(2) && r.completedWrite==2 && r.Ready());
    const uint64_t snapshot=r.completedWrite;
    assert(r.Begin() && r.Begin() && !r.Ready());
    assert(r.ImportCompleted(3) && r.completedWrite==3 && !r.Ready());
    assert(r.Finish(true) && !r.Ready());
    assert(r.Finish(true) && r.Ready() && snapshot==2);
    assert(r.Begin() && !r.Finish(false) && r.uncertain && !r.Ready());
    assert(!r.ImportCompleted(3) && !r.Begin());
    GpuReservation bad{};assert(!bad.Finish(true) && bad.uncertain);
    GpuReservation full{};full.pending=UINT32_MAX;assert(!full.Begin());
    GpuReservation saturated{};saturated.imports=UINT64_MAX;assert(!saturated.ImportCompleted(2));
    for(unsigned repeat=0;repeat<10000;++repeat) {
        GpuReservation clean{};
        for(unsigned i=0;i<8;++i)assert(clean.Begin() && !clean.Ready());
        for(unsigned i=0;i<8;++i){assert(clean.Finish(true));assert(clean.Ready()==(i==7));}
    }
    puts("GPU_RESERVATION_PASS completed imports/read-write/overlap/failure/saturation");
}
