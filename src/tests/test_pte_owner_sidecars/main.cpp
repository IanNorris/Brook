#include "memory/pte_owner_sidecars.h"
#include <cassert>
using namespace brook;
int main(){
    PteOwnerSidecars store;PteOwnerSidecars::Page a{},b{},spare{};uint16_t value=42;
    assert(!store.Install(0,&a) && !store.Install(1,&a) && !store.Set(0x1000,0,1023));
    assert(store.Install(0x1000,&a) && !store.Install(0x1000,&spare));
    assert(!store.Read(0x1000,0,value) && value==42);
    const uint16_t owners[]={1023,1024,2047,4096,5119,65534};
    for(unsigned i=0;i<6;++i){assert(store.Set(0x1000,i,owners[i]));assert(store.Read(0x1000,i,value) && value==owners[i]);}
    assert(!store.Set(0x1000,0,1022) && !store.Set(0x1000,0,65535) && !store.Set(0x1000,512,1023));
    assert(store.Read(0x1000,0,value) && value==1023);
    store.Clear(0x1000,0);assert(!store.Read(0x1000,0,value));
    // Both frame keys hash to the same bucket. Removing either must not affect
    // the other's owner; the detached record may be recycled for a new frame.
    assert(store.Install(0x100000ull,&b));assert(store.Set(0x100000ull,0,5119));
    assert(store.Remove(0x1000)==&a && !store.Find(0x1000));
    assert(store.Read(0x100000ull,0,value) && value==5119);
    assert(store.Install(0x2000,&a) && !store.Read(0x2000,1,value));
    assert(store.Remove(0x2000)==&a && store.Remove(0x100000ull)==&b);
}
