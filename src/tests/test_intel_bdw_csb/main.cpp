#include "intel_bdw_csb.h"
#include <cassert>
#include <cstring>
#include <sys/mman.h>
using namespace brook;
int main(){
 auto* base=(uint8_t*)mmap(nullptr,3*4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(base!=MAP_FAILED);
 auto* page=base+4096;assert(!mprotect(page,4096,PROT_READ|PROT_WRITE));
 uint32_t producer=5;std::memcpy(page+0x7c,&producer,4);
 for(unsigned i=0;i<6;++i){uint64_t value=0x1234567800000000ull+i;std::memcpy(page+0x40+8*i,&value,8);}
 uint32_t tail=0;assert(BdwCsbReadProducer(page,tail) && tail==5);
 producer=0xa5a50001;std::memcpy(page+0x7c,&producer,4);assert(BdwCsbReadProducer(page,tail) && tail==1);
 producer=0x81;std::memcpy(page+0x7c,&producer,4);assert(BdwCsbReadProducer(page,tail) && tail==0x81);
 for(unsigned i=0;i<6;++i){uint64_t value=0;assert(BdwCsbReadRecord(page,i,value) && value==0x1234567800000000ull+i);}
 uint64_t record=123;tail=123;
 assert(!BdwCsbReadProducer(nullptr,tail) && tail==123);
 assert(!BdwCsbReadProducer(page+1,tail) && tail==123);
 assert(!BdwCsbReadRecord(page,6,record) && record==123);
 assert(!BdwCsbReadRecord(page+1,0,record) && record==123);
 assert(!munmap(base,3*4096));
}
