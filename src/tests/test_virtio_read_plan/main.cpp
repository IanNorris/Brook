#include <cassert>
#include <cstdint>
#include "../../kernel/src/virtio_blk_read_plan.h"
int main(){
    using brook::VirtioContiguousReadBytes;
    uint64_t blocks[17];for(unsigned i=0;i<17;++i)blocks[i]=1234+i;
    assert(VirtioContiguousReadBytes(blocks,16,16,4096,256*1024)==65536);
    assert(VirtioContiguousReadBytes(blocks,2,16,4096,8192)==8192);
    assert(!VirtioContiguousReadBytes(blocks,17,16,4096,256*1024));
    assert(!VirtioContiguousReadBytes(blocks,16,16,4096,65535));
    assert(!VirtioContiguousReadBytes(blocks,1,16,4096,65536));
    assert(!VirtioContiguousReadBytes(blocks,0,16,4096,65536));
    assert(!VirtioContiguousReadBytes(nullptr,16,16,4096,65536));
    assert(!VirtioContiguousReadBytes(blocks,16,16,0,65536));
    assert(!VirtioContiguousReadBytes(blocks,2,16,UINT32_MAX,UINT32_MAX));
    for(unsigned hole=1;hole<16;++hole){
        ++blocks[hole];assert(!VirtioContiguousReadBytes(blocks,16,16,4096,65536));--blocks[hole];
    }
    blocks[0]=UINT64_MAX;blocks[1]=0;
    assert(!VirtioContiguousReadBytes(blocks,2,16,4096,8192));
    blocks[0]=UINT64_MAX-1;blocks[1]=UINT64_MAX;
    assert(VirtioContiguousReadBytes(blocks,2,16,4096,8192)==8192);
}
