#include "../../kernel/src/ahci_byte_write.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <random>
#include <vector>
using namespace brook;
struct Disk {
    std::vector<uint8_t> data;
    unsigned reads=0,writes=0,failRead=0,failWrite=0;bool partialFailure=false;
    bool read(uint64_t lba,uint32_t count,uint8_t* out) {
        ++reads;if(reads==failRead)return false;
        assert(lba*512+count*512<=data.size());memcpy(out,data.data()+lba*512,count*512);return true;
    }
    bool write(uint64_t lba,uint32_t count,const uint8_t* in) {
        ++writes;if(writes==failWrite){if(partialFailure)data[lba*512]=in[0];return false;}
        assert(lba*512+count*512<=data.size());memcpy(data.data()+lba*512,in,count*512);return true;
    }
};
int main() {
    Disk d;d.data.resize(256*512);std::vector<uint8_t> oracle=d.data,scratch(65536),in(90000);
    std::mt19937 rnd(4916);
    for(unsigned i=0;i<500;++i) {
        size_t pos=rnd()%oracle.size();size_t len=std::min<size_t>(rnd()%in.size(),oracle.size()-pos);
        for(size_t j=0;j<len;++j)in[j]=uint8_t(rnd());
        assert(AhciByteWrite(d,pos,in.data(),len,256,scratch.data())==int(len));
        std::copy(in.begin(),in.begin()+len,oracle.begin()+pos);assert(d.data==oracle);
    }
    unsigned before=d.writes;
    assert(AhciByteWrite(d,256*512-1,in.data(),2,256,scratch.data())==-22 && d.writes==before);
    assert(AhciByteWrite(d,0,nullptr,1,256,scratch.data())==-22 && d.writes==before);
    d.failRead=d.reads+1;assert(AhciByteWrite(d,1,in.data(),1,256,scratch.data())==-5 && d.writes==before);
    d.failRead=0;d.failWrite=d.writes+2;d.partialFailure=true;
    assert(AhciByteWrite(d,0,in.data(),65536+512,256,scratch.data())==65536);
    assert(d.writes==d.failWrite); // no command after an uncertain write
}
