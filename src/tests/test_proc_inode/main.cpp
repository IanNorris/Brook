#include <cassert>
#include <cstdio>
#include <unordered_set>
#include "../../kernel/src/proc_inode.h"
int main(){using namespace brook;
 std::unordered_set<uint64_t> ids;
 for(uint64_t generation: {uint64_t{0},uint64_t{1},uint64_t{2}})
  for(uint32_t role=0;role<ProcInodeRoles;++role)
   for(bool member: {false,true})for(uint32_t index=0;index<ProcInodeIndexes;++index){
    uint64_t a=0,b=0;assert(ProcInodeNumber(generation,role,member,index,a));
    assert(ProcInodeNumber(generation,role,member,index,b)&&a==b);assert(ids.insert(a).second);
   }
 uint64_t out=123;
 assert(!ProcInodeNumber(~uint64_t{0},0,false,0,out)&&out==123);
 assert(!ProcInodeNumber(1,ProcInodeRoles,false,0,out)&&out==123);
 assert(!ProcInodeNumber(1,0,false,ProcInodeIndexes,out)&&out==123);
 constexpr uint64_t last=((uint64_t{1}<<63)-1)/(uint64_t{ProcInodeRoles}*2*ProcInodeIndexes);
 assert(ProcInodeNumber(last,ProcInodeRoles-1,true,ProcInodeIndexes-1,out));
 assert(!ProcInodeNumber(last+1,0,false,0,out));
 puts("proc inode identity: globals/incarnations/roles/members/indexes/overflow passed");
}
