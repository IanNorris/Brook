#include "../../kernel/src/fs/write_behind_queue.h"
#include <array>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cstdio>
using brook::WriteBehindQueue;
struct Disk {
    std::vector<uint8_t> data=std::vector<uint8_t>(8192,0);
    unsigned calls=0,flushes=0,failAt=0;
    bool failAll=false,failFlush=false,partial=false;
    std::vector<uint64_t> order;
    bool Read(uint64_t off,void* out,uint64_t len) {
        if(off>data.size() || len>data.size()-off)return false;
        std::copy_n(data.data()+off,len,static_cast<uint8_t*>(out));return true;
    }
    bool Write(uint64_t off,const void* in,uint64_t len) {
        ++calls;order.push_back(off);
        const bool fail=failAll || calls==failAt;
        if(!fail || partial)std::copy_n(static_cast<const uint8_t*>(in),len,data.data()+off);
        return !fail;
    }
    bool Flush(){++flushes;return !failFlush;}
};
struct Fixture {
    std::array<WriteBehindQueue::Record,4> rows{};
    std::array<uint8_t,512> bytes{};
    WriteBehindQueue q;Disk disk;
    Fixture(){q.Init(rows.data(),rows.size(),bytes.data(),bytes.size());}
};
int main() {
    using R=WriteBehindQueue::Result;
    Fixture f;std::array<uint8_t,64> a{},b{};a.fill(17);b.fill(39);
    assert(f.q.Write(f.disk,100,a.data(),40,1));
    assert(f.q.Write(f.disk,120,b.data(),10,2)); // contained replacement
    assert(f.q.Write(f.disk,80,b.data(),25,3)); // prepend union
    assert(f.q.Write(f.disk,140,a.data(),20,4)); // adjacent append
    assert(f.q.count==1 && f.q.used==80 && f.q.merges==3 && !f.disk.calls);
    std::array<uint8_t,100> read{};assert(f.q.Read(f.disk,70,read.data(),100));
    for(unsigned i=0;i<100;++i) {
        const auto off=i+70;const uint8_t expected=(off>=80&&off<105)||(off>=120&&off<130)?39:off>=100&&off<160?17:0;
        assert(read[i]==expected);
    }
    assert(f.q.Drain(f.disk)==R::Ok && f.disk.calls==1 && f.disk.flushes==1 && !f.q.count);
    // Descriptor pressure drains before admitting a fifth disjoint record.
    Fixture pressure;
    for(unsigned i=0;i<5;++i)assert(pressure.q.Write(pressure.disk,i*100,a.data(),8,i+1));
    assert(pressure.q.capacityDrains==1 && pressure.disk.calls==4 && pressure.q.count==1);
    assert(pressure.q.Drain(pressure.disk)==R::Ok);
    // Byte pressure and a large synchronous bypass retain older-write order.
    Fixture large;std::array<uint8_t,700> big{};big.fill(61);
    assert(large.q.Write(large.disk,0,a.data(),64,1));
    assert(large.q.Write(large.disk,100,big.data(),big.size(),2));
    assert(large.q.count==0 && large.disk.order==std::vector<uint64_t>({0,100}));
    Fixture bytePressure;
    assert(bytePressure.q.Write(bytePressure.disk,0,big.data(),400,1));
    assert(bytePressure.q.Write(bytePressure.disk,1000,big.data(),200,2));
    assert(bytePressure.q.capacityDrains==1 && bytePressure.q.count==1);
    // A partially committed OUT retains the entire ordered batch for replay.
    Fixture failed;
    assert(failed.q.Write(failed.disk,0,a.data(),32,1));
    assert(failed.q.Write(failed.disk,100,b.data(),32,2));
    failed.disk.failAt=2;failed.disk.partial=true;
    assert(failed.q.Drain(failed.disk)==R::WriteError && failed.q.count==2 && !failed.disk.flushes);
    failed.disk.failAll=true;
    assert(!failed.q.Write(failed.disk,200,a.data(),32,3) && failed.q.count==2);
    assert(std::find(failed.disk.order.begin(),failed.disk.order.end(),200)==failed.disk.order.end());
    failed.disk.failAll=false;failed.disk.failAt=0;
    assert(failed.q.Drain(failed.disk)==R::Ok && !failed.q.count);
    assert(failed.disk.order[failed.disk.order.size()-2]==0 && failed.disk.order.back()==100);
    // Flush failure also retains bytes; a subsequent write must replay first.
    Fixture flush;
    assert(flush.q.Write(flush.disk,0,a.data(),32,1));flush.disk.failFlush=true;
    assert(flush.q.Drain(flush.disk)==R::FlushError && flush.q.count==1);
    assert(!flush.q.Write(flush.disk,200,b.data(),32,2) && flush.q.count==1);
    flush.disk.failFlush=false;
    assert(flush.q.Write(flush.disk,200,b.data(),32,3));
    assert(flush.disk.order==std::vector<uint64_t>({0,0,0}));
    assert(flush.q.Drain(flush.disk)==R::Ok && flush.disk.order.back()==200);
    // An unowned oversized partial failure cannot be replayed: poison writes.
    Fixture oversized;oversized.disk.failAt=1;oversized.disk.partial=true;
    assert(!oversized.q.Write(oversized.disk,0,big.data(),big.size(),1));
    assert(!oversized.q.Write(oversized.disk,1000,a.data(),32,2));
    assert(oversized.q.Drain(oversized.disk)==R::WriteError && oversized.disk.calls==1);
    assert(!oversized.q.Write(oversized.disk,~uint64_t{0},a.data(),32,2));
    // Byte oracle across capacity drains, oversized writes and overlapping reads.
    Fixture random;auto expected=random.disk.data;uint32_t seed=0x52c91;
    auto next=[&](){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
    for(unsigned i=0;i<3000;++i) {
        const unsigned size=1+next()%700,offset=next()%(8192-size);
        for(unsigned j=0;j<size;++j)big[j]=next();
        assert(random.q.Write(random.disk,offset,big.data(),size,i+1));
        std::copy_n(big.data(),size,expected.data()+offset);
        const unsigned rsize=1+next()%700,roff=next()%(8192-rsize);
        std::array<uint8_t,700> actual{};
        assert(random.q.Read(random.disk,roff,actual.data(),rsize));
        assert(std::equal(actual.begin(),actual.begin()+rsize,expected.begin()+roff));
        if(i%13==0){assert(random.q.Drain(random.disk)==R::Ok);assert(random.disk.data==expected);}
    }
    assert(random.q.Drain(random.disk)==R::Ok && random.disk.data==expected);
    std::puts("WRITE_BEHIND_QUEUE_PASS");
}
