#include "../../drivers/xhci/usb_block_bytes.h"
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
int main(){
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
