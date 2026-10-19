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
    assert(r.BeginCapture() && !r.BeginCapture() && !r.Ready() && !r.Begin());
    r.FinishCapture(true,false); // safe capacity/cancellation rollback
    assert(r.BeginCapture());r.FinishCapture(true,true);
    assert(!r.BeginCapture() && r.Ready());
    assert(r.ImportCompleted(1) && !r.BeginCapture());
    assert(r.ImportCompleted(2) && r.BeginCapture());r.FinishCapture(true,true);
    const uint64_t snapshot=r.completedWrite;
    assert(r.Begin() && r.Begin() && !r.Ready());
    assert(r.ImportCompleted(3) && r.completedWrite==5 && !r.Ready() && !r.BeginCapture());
    assert(r.Finish(true) && !r.Ready());
    assert(r.Finish(true) && r.Ready() && snapshot==4 && r.BeginCapture());
    r.FinishCapture(true,true);
    assert(r.Begin() && !r.Finish(false) && r.uncertain && !r.Ready());
    assert(!r.ImportCompleted(3) && !r.Begin());
    GpuReservation bad{};assert(!bad.Finish(true) && bad.uncertain);
    GpuReservation full{};full.pending=UINT32_MAX;assert(!full.Begin());
    GpuReservation saturated{};saturated.imports=UINT64_MAX;assert(!saturated.ImportCompleted(2));
    GpuReservation capture{};
    assert(capture.ImportCompleted(2) && capture.BeginCapture());
    assert(capture.ImportCompleted(2)); // later producer token must not be consumed by old capture
    capture.FinishCapture(true,true);assert(capture.capturedWrite==1 && capture.BeginCapture());
    capture.FinishCapture(false,false);assert(capture.uncertain && !capture.Ready() && !capture.Begin());
    for(unsigned repeat=0;repeat<10000;++repeat) {
        GpuReservation clean{};
        for(unsigned i=0;i<8;++i)assert(clean.Begin() && !clean.Ready());
        for(unsigned i=0;i<8;++i){assert(clean.Finish(true));assert(clean.Ready()==(i==7));}
    }
    puts("GPU_RESERVATION_PASS completed imports/read-write/overlap/failure/saturation");
}
