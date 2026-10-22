#include <cassert>
#include <cstdlib>
#include <cstdio>
#include "../../kernel/src/user_namespace.h"
namespace brook { unsigned frees=0;void kfree(void* p){++frees;std::free(p);} }
int main(){using namespace brook;
 UserNamespace ns{};ns.owner=1000;ns.creatorGroup=100;ns.depth=1;
 UserAuthority owner{nullptr,1000,100,0},child{&ns,1000,100,UserCapabilities},stranger{nullptr,1001,100,0};
 assert(UserNamespaceCapable(owner,&ns,CapSysAdmin));assert(!UserNamespaceCapable(stranger,&ns,CapSysAdmin));
 assert(!UserNamespaceCapable(child,nullptr,CapSysAdmin));assert(UserNamespaceCapable(child,&ns,CapSysAdmin));
 assert(UserIdReport(&ns,false,1000)==65534);assert(UserIdToCanonical(&ns,false,0)==UserIdInvalid);
 assert(UserNamespaceWriteMap(&ns,stranger,false,0,1000,1)==-1);
 assert(UserNamespaceWriteMap(&ns,child,false,0,0,1)==-1);
 assert(UserNamespaceWriteMap(&ns,child,false,0,1000,2)==-22);
 assert(!UserNamespaceWriteMap(&ns,child,false,0,1000,1));assert(UserIdReport(&ns,false,1000)==0);
 assert(UserIdToCanonical(&ns,false,0)==1000);assert(UserIdToCanonical(&ns,false,65534)==UserIdInvalid);
 assert(UserNamespaceWriteMap(&ns,owner,false,1000,1000,1)==-1);
 assert(UserNamespaceWriteMap(&ns,child,true,0,100,1)==-1);
 assert(!UserNamespaceSetgroups(&ns,child,true));assert(UserNamespaceSetgroups(&ns,child,false)==-1);
 assert(!UserNamespaceWriteMap(&ns,child,true,0,100,1));assert(UserNamespaceSetgroups(&ns,child,true)==-1);
 UserNamespace nested{};nested.parent=&ns;nested.owner=1000;nested.creatorGroup=100;
 UserAuthority nestedActor{&nested,1000,100,UserCapabilities};nested.denyGroups=true;
 assert(UserNamespaceWriteMap(&nested,nestedActor,false,42,0,1)==-1);nested.creatorSetfcap=true;
 assert(!UserNamespaceWriteMap(&nested,nestedActor,false,42,0,1));assert(UserIdReport(&nested,false,1000)==42);
 assert(UserNamespaceCapable(child,&nested,CapSysAdmin));assert(!UserNamespaceCapable(nestedActor,&ns,CapSysAdmin));
 UserNamespace allowNs{};allowNs.owner=1000;allowNs.groups={0,100,true};
 assert(!UserNamespaceSetgroups(&allowNs,owner,false));assert(UserNamespaceSetgroups(&allowNs,owner,true)==-1);
 UserCapabilityState caps{UserCapabilities,UserCapabilities,0,UserCapabilities};
 assert(UserCapabilitiesSet(caps,UserCapabilities,0,0)==-1);assert(!UserCapabilitiesSet(caps,0,0,0));
 assert(UserCapabilitiesSet(caps,1,1,0)==-1);UserCapabilitiesExec(caps,false,false);assert(!caps.effective&&!caps.permitted);
 caps.bounding&=~(1ull<<CapSysAdmin);UserCapabilitiesExec(caps,true,false);assert(!(caps.effective&(1ull<<CapSysAdmin)));
 caps.permitted=caps.effective=0;UserCapabilitiesExec(caps,true,true);assert(!caps.permitted);
 auto* parent=static_cast<UserNamespace*>(std::malloc(sizeof(UserNamespace)));*parent={};
 auto* held=static_cast<UserNamespace*>(std::malloc(sizeof(UserNamespace)));*held={};held->parent=parent;
 UserNamespaceRetain(parent);UserNamespaceRelease(parent);UserNamespaceRetain(held);UserNamespaceRelease(held);
 assert(frees==0);UserNamespaceRelease(held);assert(frees==2);puts("user namespace policy/lifetime passed");
}
