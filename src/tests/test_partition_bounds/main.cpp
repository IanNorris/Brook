#include "../../kernel/src/partition_bounds.h"
#include <cassert>
#include <cstring>
using namespace brook;
static void put(uint8_t* p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=v>>(8*i);}
int main(){
    uint8_t m[512]{};MbrPartition parts[4];assert(MbrPartitions(m,100000,parts)==0);
    m[510]=0x55;m[511]=0xaa;m[446+4]=0xc;put(m+446+8,2048);put(m+446+12,90000);
    assert(MbrPartitions(m,100000,parts)==1 && parts[0].first==2048 && parts[0].count==90000);
    assert(MbrPartitions(m,1000,parts)<0);
    m[446+16+4]=0x83;put(m+446+16+8,2200);put(m+446+16+12,1000);assert(MbrPartitions(m,100000,parts)<0);
    m[446+16+4]=0;m[446+4]=0xee;assert(MbrPartitions(m,100000,parts)==0);
    m[446+4]=0xc;m[446]=1;assert(MbrPartitions(m,100000,parts)<0);
    uint64_t physical,amount;
    assert(PartitionIoSpan(4096,10000,9999,900,physical,amount) && physical==14095 && amount==1);
    assert(PartitionIoSpan(4096,10000,10000,1,physical,amount) && amount==0);
    assert(!PartitionIoSpan(4096,10000,10001,1,physical,amount));
    assert(!PartitionIoSpan(~0ull-10,100,20,1,physical,amount));
    assert(!PartitionIoSpan(~0ull-10,100,0,11,physical,amount));
}
