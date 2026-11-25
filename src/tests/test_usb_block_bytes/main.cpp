#include "../../drivers/xhci/usb_block_bytes.h"
#include "../../drivers/xhci/usb_scsi_unmap.h"
#include "../../drivers/xhci/usb_uas.h"
#include "../../drivers/xhci/usb_write_windows.h"
#include "../../kernel/src/usb_trim_batch.h"
#include <vector>
#include <cassert>
#include <cstring>
#include <initializer_list>
using namespace brook;
struct Disk {
    uint32_t block;std::vector<uint8_t> disk,scratch;unsigned reads=0,writes=0,failAt=0,failWriteAt=0;bool quarantined=false;
    Disk(unsigned b):block(b),disk(b*1024),scratch(65536) {for(size_t i=0;i<disk.size();++i)disk[i]=(i*17+5)%251;}
    bool usable(){return !quarantined;}
    uint32_t blockSize(){return block;}
    uint64_t blockCount(){return disk.size()/block;}
    uint32_t bounceBytes(){return scratch.size();}
    uint8_t* bounce(){return scratch.data();}
    bool readBlocks(uint64_t lba,uint32_t count){assert(count*block<=scratch.size());if(++reads==failAt){quarantined=true;return false;}memcpy(scratch.data(),disk.data()+lba*block,count*block);return true;}
    bool writeBlocks(uint64_t lba,uint32_t count){assert(count*block<=scratch.size());if(++writes==failWriteAt){quarantined=true;return false;}memcpy(disk.data()+lba*block,scratch.data(),count*block);return true;}
};
static void Unmap() {
    uint8_t vpd[28]={0,0xb0,0,24};
    assert(ScsiUnmapVpd(vpd,sizeof(vpd),0xb0,28));
    assert(!ScsiUnmapVpd(vpd,sizeof(vpd)-1,0xb0,28));
    assert(!ScsiUnmapVpd(vpd,sizeof(vpd),0xb2,28));
    vpd[3]=16;
    assert(!ScsiUnmapVpd(vpd,sizeof(vpd),0xb0,28));

    const UsbUnmapLimits limits{128,2};
    DeviceWriteWindow ranges[]={{512ull*0x12345678,32768},{512ull*0x200000000,32768}};
    uint8_t encoded[8+16*UsbUnmapMaxRanges];
    assert(ScsiEncodeUnmap(encoded,limits,ranges,2)==40);
    const uint8_t expected[]={0,38,0,32,0,0,0,0,
        0,0,0,0,0x12,0x34,0x56,0x78,0,0,0,64,0,0,0,0,
        0,0,0,2,0,0,0,0,0,0,0,64,0,0,0,0};
    assert(!memcmp(encoded,expected,sizeof(expected)));
    assert(!ScsiEncodeUnmap(encoded,{127,2},ranges,2));
    assert(!ScsiEncodeUnmap(encoded,{128,1},ranges,2));
    ++ranges[1].start;
    assert(!ScsiEncodeUnmap(encoded,limits,ranges,2));
    ranges[1].start=~uint64_t{511};
    assert(!ScsiEncodeUnmap(encoded,limits,ranges,2));
}

static void Uas() {
    std::vector<uint8_t> cfg={9,2,0,0,1,1,0,0x80,0,9,4,2,1,4,8,6,0x62,0};
    const uint8_t endpoints[]={1,0x82,0x83,4};
    for(uint8_t pipe=0;pipe<4;++pipe) {
        const uint8_t block[]={7,5,endpoints[pipe],2,0,4,0,
            6,0x30,15,uint8_t(pipe?5:0),0,0,4,0x24,uint8_t(pipe+1),0};
        cfg.insert(cfg.end(),block,block+sizeof(block));
    }
    UsbUasConfig parsed{};
    assert(UsbUasParse(cfg.data(),cfg.size(),2,parsed));
    assert(parsed.interface==2 && parsed.alternate==1 && parsed.pipes[UasDataOut].address==4);
    assert(!UsbUasParse(cfg.data(),cfg.size(),3,parsed));
    assert(!UsbUasParse(cfg.data(),cfg.size()-1,2,parsed));
    cfg[18+17+7+3]=1; // status endpoint offers only two streams
    assert(!UsbUasParse(cfg.data(),cfg.size(),2,parsed));
    uint8_t iu[16]={3,0,0,4};
    uint8_t status=0;
    uint32_t sense=0;
    assert(UsbUasStatus(iu,sizeof(iu),4,status,sense) && !status && !sense);
    assert(!UsbUasStatus(iu,sizeof(iu),3,status,sense));
    iu[15]=1;
    assert(!UsbUasStatus(iu,sizeof(iu),4,status,sense));
}

static void UasFlight() {
    UsbUasFlightState flight{0b11,0b11};
    unsigned step=0,retired=0;
    assert(UsbUasWaitFlight(flight,[&] {
        switch(++step) {
        case 1: flight.Status(0,2); break; // CHECK CONDITION with no data
        case 2: flight.Status(1,0); break; // GOOD before its data
        case 3: flight.Data(1); break;
        default: assert(false);
        }
        return true;
    },[&] {return step>=4;},[&](unsigned mask) {retired=mask;return true;}));
    assert(step==3 && retired==1);
    flight={1,1};
    step=0;retired=0;
    assert(!UsbUasWaitFlight(flight,[&] {++step;flight.Data(0);return true;},
        [&] {return step==2;},[&](unsigned) {assert(false);return true;}));
    flight={1,1};
    assert(!UsbUasWaitFlight(flight,[&] {flight.Status(0,8);return true;},
        [] {return false;},[](unsigned mask) {assert(mask==1);return false;}));
    assert(UsbUasRetryRead(8,false,0) && UsbUasRetryRead(0x28,false,0));
    assert(UsbUasRetryRead(2,true,0xb) && !UsbUasRetryRead(2,true,3));
    assert(!UsbUasRetryRead(2,false,0xb) && !UsbUasRetryRead(2,true,6));

    unsigned calls=0;
    assert(!UsbUasRestoreBot(false,true,[&] {assert(false);return true;},[&] {assert(false);return true;}));
    assert(!UsbUasRestoreBot(true,true,[&] {++calls;return false;},[&] {assert(false);return true;}));
    assert(UsbUasRestoreBot(true,true,[&] {assert(calls++==1);return true;},
        [&] {assert(calls++==2);return true;}));
    assert(!UsbUasRestoreBot(true,false,[&] {assert(calls++==3);return true;},
        [&] {assert(false);return true;}));
}

static void TrimAndDma() {
    UsbTrimBatchPlan plan({0xffffffffu,2},UsbUnmapMaxSectors+24);
    uint64_t totals[3]{};
    unsigned drains=0;
    auto submit=[&](const DeviceWriteWindow* ranges,uint32_t count) {
        assert(count<=2 && drains<3);
        for(unsigned i=0;i<count;++i)totals[drains]+=ranges[i].length/512;
        ++drains;return true;
    };
    assert(plan.Add({8192,UsbUnmapMaxSectors+8},submit));
    assert(plan.Add({4u<<20,8},submit));
    assert(plan.Add({5u<<20,80},submit) && plan.Drain(submit));
    assert(drains==3 && totals[0]==UsbUnmapMaxSectors && totals[1]==16 && totals[2]==8);
    assert(plan.total==UsbUnmapMaxSectors+24);
    UsbTrimBatchPlan small({17,1},0);
    assert(!small.Add({1024,40},[](const DeviceWriteWindow* ranges,uint32_t count) {
        assert(count==1 && ranges[0].length==8192);return false;
    }));
    const DeviceWriteWindow good{16384,8192}, crossing{20480,8192}, p1{4096,4096};
    UsbWriteWindows windows{p1,good,true};
    assert(windows.CheckUnmap({&good,1})==0 && windows.CheckUnmap({&crossing,1})==-30);
    assert(windows.CheckUnmap({&p1,1})==-30 && windows.CheckUnmap({nullptr,0})==-22);
    UsbDmaRead dma{good.start,4096,4096,false};
    assert(windows.CanReadDma(dma,512,65536,1u<<20));
    dma.offset=p1.start;assert(!windows.CanReadDma(dma,512,65536,1u<<20));
    dma.offset=good.start;dma.physical=(1u<<20)-1024;
    assert(!windows.CanReadDma(dma,512,65536,1u<<20));
    assert(UsbDmaTransferFailure(dma,true)==-5 && dma.retain);
    assert(UsbDmaTransferFailure(dma,false)==-5 && !dma.retain);
}

int main(){
    Unmap();
    Uas();
    UasFlight();
    TrimAndDma();
    Disk large(4096);
    large.scratch.resize(1<<20);
    std::vector<uint8_t> largeRead((1<<20)+4096);
    assert(UsbBlockReadBytes(large,0,largeRead.data(),largeRead.size())==int(largeRead.size()));
    assert(large.reads==2 && !memcmp(largeRead.data(),large.disk.data(),largeRead.size()));
    for(unsigned capacity: {4096u,65536u})for(unsigned block: {512u,4096u}) {
        Disk d(block);d.scratch.resize(capacity);std::vector<uint8_t> out(150000);
        for(unsigned offset: {0u,1u,17u,511u,4095u,65535u})
            for(unsigned len: {1u,17u,511u,512u,513u,1024u,70000u,150000u}) {
                assert(UsbBlockReadBytes(d,offset,out.data(),len)==int(len));
                assert(!memcmp(out.data(),d.disk.data()+offset,len));
            }
        const auto saved=d.disk;
        for(unsigned offset: {1u,511u,4095u,65535u}) {
            std::vector<uint8_t> data(70001,0xa5);d.disk=saved;
            assert(UsbBlockWriteBytes(d,offset,data.data(),data.size())==int(data.size()));
            assert(!memcmp(d.disk.data(),saved.data(),offset));
            assert(!memcmp(d.disk.data()+offset,data.data(),data.size()));
            assert(!memcmp(d.disk.data()+offset+data.size(),saved.data()+offset+data.size(),saved.size()-offset-data.size()));
        }
        assert(UsbBlockReadBytes(d,d.disk.size(),out.data(),1)==0);
        assert(UsbBlockReadBytes(d,d.disk.size()-1,out.data(),2)==1);
        assert(UsbBlockReadBytes(d,d.disk.size()+1,out.data(),1)<0);
        assert(UsbBlockReadBytes(d,~0ull,out.data(),1)<0);
        assert(UsbBlockWriteBytes(d,d.disk.size()-1,out.data(),2)<0);
        assert(UsbBlockReadBytes(d,0,nullptr,1)<0);
        assert(UsbBlockReadBytes(d,0,out.data(),0)==0);
        assert(UsbBlockReadBytes(d,0,out.data(),1ull<<32)<0);
        d.failAt=d.reads+1;assert(UsbBlockReadBytes(d,3,out.data(),1)==-5);
        d.quarantined=false;d.failAt=d.reads+2;assert(UsbBlockReadBytes(d,0,out.data(),100000)==int(capacity));
        Disk failed(block);failed.failWriteAt=1;std::vector<uint8_t> a(512,0x31),b(512,0xa9);
        assert(UsbBlockWriteBytes(failed,0,a.data(),a.size())<0);
        const auto activeDma=failed.scratch;
        assert(UsbBlockWriteBytes(failed,0,b.data(),b.size())<0);
        assert(failed.scratch==activeDma && failed.writes==1);
    }
}
