#include "intel_bdw_ggtt_space.h"
#include "intel_bdw_inventory.h"
#include <cassert>
using namespace brook;
static BdwGgttSpace space;
static BdwGgttSpace small;
static BdwGgttSpace crossing;
static BdwGgttSpace scanout;
int main() {
    BdwGgttReservation untouched{42,7,99};
    assert(!space.reserve(1,untouched));
    assert(!BdwGgttEntries({false,0x40,0,0}) && !BdwGgttEntries({true,0,0,0}));
    assert(BdwGgttEntries({true,0x40,0,0})==262144);
    assert(BdwGgttEntries({true,0x80,0,0})==524288);
    assert(BdwGgttEntries({true,0xc0,0,0})==1048576);
    assert(!space.configure(65536) && space.configure(262144) && !space.configure(1048576));
    BdwGgttReservation edge{};assert(small.configure(65544) && small.reserve(8,edge));
    assert(edge.first==65536 && !small.reserve(1,untouched) && small.release(edge));
    assert(crossing.configure(262144));
    BdwGgttReservation chunks[16]{};
    for(auto& chunk:chunks)assert(crossing.reserve(31,chunk));
    BdwGgttReservation next{};assert(crossing.reserve(32,next));
    assert(next.first==65536+512);
    for(const auto& chunk:chunks)assert(crossing.release(chunk));
    assert(crossing.release(next));
    assert(!space.reserve(0,untouched) && !space.reserve(33,untouched));
    assert(untouched.first==42 && untouched.count==7 && untouched.token==99);
    BdwGgttReservation a[64]{};
    for(unsigned i=0;i<64;++i) {
        assert(space.reserve(32,a[i]));
        assert(a[i].first==65536+i*32 && a[i].count==32 && space.owns(a[i]));
        for(unsigned j=0;j<i;++j)assert(a[i].token!=a[j].token);
    }
    assert(!space.reserve(1,untouched));
    auto stale=a[13];auto altered=stale;altered.count--;
    assert(!space.release(altered) && space.owns(stale));
    assert(space.release(stale) && !space.release(stale));
    BdwGgttReservation replacement{};
    assert(space.reserve(8,replacement) && replacement.first==stale.first && replacement.token!=stale.token);
    assert(!space.release(stale) && space.owns(replacement));
    assert(space.release(replacement));
    for(unsigned i=0;i<64;++i)if(i!=13)assert(space.release(a[i]));
    BdwGgttReservation final{};
    assert(space.reserve(8,final) && final.first==65536 && space.release(final));
    assert(scanout.configure(262144));BdwGgttReservation large{},second{},tiny{};
    assert(!scanout.reserveExtent(0,1,large) && !scanout.reserveExtent(65537,1,large));
    assert(!scanout.reserveExtent(1,0,large) && !scanout.reserveExtent(1,3,large));
    assert(scanout.reserve(8,tiny) && scanout.reserveExtent(5625,512,large));
    assert(large.first==66048 && large.count==5625 && scanout.owns(large));
    assert(scanout.reserveExtent(5625,512,second) && second.first>=large.first+large.count && !(second.first&511));
    auto staleLarge=large;assert(scanout.release(large) && !scanout.release(staleLarge));
    BdwGgttReservation reused{};assert(scanout.reserveExtent(5625,512,reused));
    assert(reused.first==staleLarge.first && reused.token!=staleLarge.token && !scanout.release(staleLarge));
    assert(scanout.release(reused) && scanout.release(second) && scanout.release(tiny));
}
