#include "../../kernel/src/ssd_install_digest.h"
#include <cassert>
#include <cstring>
#include <vector>
using namespace brook;
struct Source {
    std::vector<uint8_t> bytes;unsigned reads=0;bool shortRead=false;
    int readPacked(uint64_t offset,void* out,uint32_t n) {
        ++reads;if(offset>bytes.size() || n>bytes.size()-offset)return -1;
        std::memcpy(out,bytes.data()+offset,n);return shortRead?int(n/2):int(n);
    }
};
int main(){
    const SsdInstallGeometry geometry{68,34,1};const uint64_t size=68*512;
    uint8_t record[SsdDigestRecordBytes]{};std::memcpy(record,"BRKSSD1",7);
    BrookUpdatePut64(record+8,68);BrookUpdatePut64(record+16,34);BrookUpdatePut64(record+24,1);BrookUpdatePut64(record+32,size);
    const uint8_t expected[32]={0x59,0x5d,0x36,0x30,0xdc,0x2a,0x68,0x48,0x1f,0x09,0xc3,0xb7,0xb4,0xc1,0x53,0xb2,0x68,0x9f,0x16,0xea,0x10,0x46,0xd6,0x66,0x29,0xc1,0xe3,0x49,0xdc,0xf4,0x7b,0x45};
    std::memcpy(record+40,expected,32);Source source;source.bytes.resize(size);
    for(size_t i=0;i<size;++i)source.bytes[i]=uint8_t(i%251);
    uint8_t buffer[1024];assert(SsdDigestVerifyPinned(source,record,sizeof(record),geometry,size,buffer,sizeof(buffer)));
    uint8_t trusted[SsdDigestRecordBytes];std::memcpy(trusted,record,sizeof(trusted));
    assert(SsdDigestVerifyExpectedPinned(source,record,sizeof(record),trusted,sizeof(trusted),geometry,size,buffer,sizeof(buffer)));
    trusted[40]^=1;auto savedReads=source.reads;
    assert(!SsdDigestVerifyExpectedPinned(source,record,sizeof(record),trusted,sizeof(trusted),geometry,size,buffer,sizeof(buffer)) && source.reads==savedReads);
    trusted[40]^=1;
    assert(!SsdDigestVerifyExpectedPinned(source,record,sizeof(record),trusted,sizeof(trusted)-1,geometry,size,buffer,sizeof(buffer)) && source.reads==savedReads);
    source.bytes[100]^=1;assert(!SsdDigestVerifyPinned(source,record,sizeof(record),geometry,size,buffer,sizeof(buffer)));source.bytes[100]^=1;
    auto before=source.reads;assert(!SsdDigestVerifyPinned(source,record,sizeof(record),{69,34,1},size,buffer,sizeof(buffer)) && source.reads==before);
    assert(!SsdDigestVerifyPinned(source,record,sizeof(record)-1,geometry,size,buffer,sizeof(buffer)) && source.reads==before);
    assert(!SsdDigestVerifyPinned(source,record,sizeof(record),geometry,size,buffer,0) && source.reads==before);
    source.shortRead=true;assert(!SsdDigestVerifyPinned(source,record,sizeof(record),geometry,size,buffer,sizeof(buffer)));
}
