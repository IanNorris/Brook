#include <cassert>
#include <cstdio>
#include "tcp_flight_control.h"
using brook::TcpFlightControl;

int main() {
    constexpr uint32_t mss=1200;
    TcpFlightControl c{};c.Init(mss);
    assert(c.cwnd==2400 && c.threshold==9600 && !c.recovering);
    c.Ack(0,mss);assert(c.cwnd==2400);
    c.Ack(2400,mss);assert(c.cwnd==4800);
    c.Ack(100000,mss);assert(c.cwnd==9600 && c.additiveCredit==0);
    c.Loss(mss);assert(c.cwnd==1200 && c.threshold==4800 && c.recovering);
    c.Loss(mss);assert(c.cwnd==1200 && c.threshold==4800);
    c.Ack(600,mss);assert(c.cwnd==1800 && !c.recovering);
    c.Loss(mss);assert(c.cwnd==1200 && c.threshold==2400 && c.recovering);

    // ACK segmentation must not change growth across slow-start threshold
    // and congestion avoidance, including integer remainders.
    TcpFlightControl batch{},fragmented{};
    batch.Init(mss);fragmented.Init(mss);batch.Loss(mss);fragmented.Loss(mss);
    batch.Ack(9000,mss);
    for(unsigned n=0;n<9000;++n)fragmented.Ack(1,mss);
    assert(batch.cwnd==fragmented.cwnd && batch.additiveCredit==fragmented.additiveCredit);
    auto before=batch;batch.Ack(0,mss);
    assert(batch.cwnd==before.cwnd && batch.additiveCredit==before.additiveCredit);
    batch.Loss(mss);assert(batch.additiveCredit==0);
    TcpFlightControl fresh{};fresh.Init(mss);assert(fresh.cwnd==2400 && !fresh.recovering);
    TcpFlightControl wide{};wide.Init(mss,16);assert(wide.cwnd==2*mss && wide.threshold==16*mss);
    wide.Ack(300000,mss,16);assert(wide.cwnd==16*mss);
    wide.Loss(mss,16);assert(wide.cwnd==mss && wide.threshold==8*mss);
    wide.Ack(300000,mss,16);assert(wide.cwnd==16*mss);
    std::puts("tcp flight control: PASS");
}
