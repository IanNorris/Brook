// Actual credential implementation; heap and scheduler operation boundaries only
// are replaced. This is independent from the pure namespace policy test.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <cstdio>
#include "process.h"
#include "process_user_namespace.h"
namespace brook {
thread_local Process* current;
Process* ProcessCurrent(){return current;}
struct ProcessOperationGuard {explicit ProcessOperationGuard(Process*){} };
#include "../../kernel/src/process_credentials.inc"
static bool failAlloc=false;
static unsigned live=0;
extern "C" void* kmalloc(uint64_t size){assert(g_credentialsLock.next==g_credentialsLock.serving);if(failAlloc)return nullptr;auto* p=std::malloc(size);if(p)++live;return p;}
extern "C" void kfree(void* p){assert(g_credentialsLock.next==g_credentialsLock.serving);if(p){assert(live);--live;std::free(p);}}
}
int main(){using namespace brook;
 Process parent{};parent.uid=parent.euid=parent.suid=1000;parent.gid=parent.egid=parent.sgid=100;current=&parent;
 Process child{};InheritCredentials(&child,&parent);
 failAlloc=true;assert(ProcessUserNamespaceCreate(&child)==-12 && !child.userNamespace);failAlloc=false;
 assert(!ProcessUserNamespaceCreate(&child));assert(live==1);assert(child.euid==1000);
 auto* ns=child.userNamespace;assert(!ProcessCapable(&child,nullptr,CapSysAdmin));assert(ProcessCapable(&child,ns,CapSysAdmin));
 current=&child;auto opened=ProcessUserAuthorityAcquire(&child);assert(ns->references==2);
 assert(ProcessUserMapWrite(ns,opened,"0 1000 1garbage",15,false,false)==-22);
 assert(!ns->users.written);assert(ProcessUserMapWrite(ns,opened,"0 0 1",5,false,false)==-1);
 assert(!ProcessUserMapWrite(ns,opened,"0 1000 1\n",9,false,false));assert(ProcessReportId(&child,false,1000)==0);
 assert(ProcessResolveId(&child,false,65534)==UserIdInvalid);assert(ProcessChangeIds(&child,false,CredentialChange::Single,65534)==-22);
 assert(ProcessUserMapWrite(ns,opened,"0 100 1\n",8,true,false)==-1);
 assert(!ProcessUserMapWrite(ns,opened,"deny\n",5,false,true));
 assert(!ProcessUserMapWrite(ns,opened,"0 100 1\n",8,true,false));assert(!ProcessSetgroupsAllowed(&child));
 assert(!ProcessChangeIds(&child,false,CredentialChange::Single,0));assert(child.euid==1000);
 assert(!ProcessFileAccess(&child,0,0,0100600).privileged);assert(ProcessCheckFileAccess(&child,0,0,0100600,4)==-13);
 // Production filesystem authority snapshot owns immutable group storage and
 // copies maps/capabilities under the same credential publication lock.
 auto* fileGroups=static_cast<SupplementaryGroups*>(kmalloc(sizeof(SupplementaryGroups)));*fileGroups={1,1,{200}};
 ProcessGroupsReplace(&child,fileGroups);
 auto fileActor=ProcessFileActorAcquire(&child,ns);
 assert(!fileActor.initialNamespace && fileActor.uid==1000 && fileActor.gid==100);
 assert(fileActor.mappedUid==1000 && fileActor.mappedGid==100 && (fileActor.capabilities&(uint64_t{1}<<CapChown)));
 assert(fileActor.groups==fileGroups && fileGroups->references==2);
 ProcessGroupsRelease(fileActor.groups);assert(fileGroups->references==1);
 auto realActor=ProcessFileActorAcquire(&child,ns,true);assert(!realActor.capabilities);ProcessGroupsRelease(realActor.groups);
 auto creatorActor=ProcessFileActorAcquire(&parent,ns);assert(creatorActor.initialNamespace && (creatorActor.capabilities&(uint64_t{1}<<CapFowner)));ProcessGroupsRelease(creatorActor.groups);
 ProcessGroupsReplace(&child,nullptr);
 Process inherited{};InheritCredentials(&inherited,&child);assert(inherited.userNamespace==ns && inherited.capabilities.effective==UserCapabilities);
 char text[80];auto n=ProcessUserMapRead(ns,child.userNamespace,false,false,text,sizeof(text));assert(n==9&&!memcmp(text,"0 1000 1\n",n));
 current=&parent;n=ProcessUserMapRead(ns,parent.userNamespace,false,false,text,sizeof(text));assert(n==9&&!memcmp(text,"0 1000 1\n",n));
 current=&child;assert(!ProcessSetCapabilities(&child,0,0,0));assert(!ProcessCapable(&child,ns,CapSysAdmin));
 assert(ProcessSetCapabilities(&child,1,1,0)==-1);assert(ProcessUserMapWrite(ns,opened,"allow\n",6,false,true)==-1);
 UserNamespaceRelease(opened.ns);UserNamespaceRelease(inherited.userNamespace);UserNamespaceRelease(child.userNamespace);assert(live==0);
 // An unchanged allowed policy remains allowed after a privileged GID map;
 // disabling it after map publication is rejected.
 Process privilegedMap{};InheritCredentials(&privilegedMap,&parent);assert(!ProcessUserNamespaceCreate(&privilegedMap));
 current=&privilegedMap;auto setgroupsOpen=ProcessUserAuthorityAcquire(&privilegedMap);
 privilegedMap.userNamespace->groups={0,100,true};
 assert(!ProcessUserMapWrite(privilegedMap.userNamespace,setgroupsOpen,"allow\n",6,false,true));
 assert(ProcessUserMapWrite(privilegedMap.userNamespace,setgroupsOpen,"deny\n",5,false,true)==-1);
 UserNamespaceRelease(setgroupsOpen.ns);UserNamespaceRelease(privilegedMap.userNamespace);current=&parent;assert(!live);
 // Two writers race a write-once map under the actual credential lock.
 Process concurrent{};InheritCredentials(&concurrent,&parent);assert(!ProcessUserNamespaceCreate(&concurrent));
 std::atomic<unsigned> success{0};auto writer=[&]{current=&concurrent;auto op=ProcessUserAuthorityAcquire(&concurrent);
   if(!ProcessUserMapWrite(concurrent.userNamespace,op,"7 1000 1\n",9,false,false))++success;UserNamespaceRelease(op.ns);};
 std::thread a(writer),b(writer);a.join();b.join();assert(success==1);
 current=&parent;UserNamespaceRelease(concurrent.userNamespace);assert(!live);
 // Open-description authority survives its opening task and can be
 // delegated to an unrelated current user namespace for the single-ID form.
 Process openingTask{};InheritCredentials(&openingTask,&parent);assert(!ProcessUserNamespaceCreate(&openingTask));
 auto authorized=ProcessUserAuthorityAcquire(&openingTask);auto* pinned=openingTask.userNamespace;
 UserNamespaceRelease(openingTask.userNamespace);openingTask.userNamespace=nullptr;
 Process receivingTask{};InheritCredentials(&receivingTask,&parent);assert(!ProcessUserNamespaceCreate(&receivingTask));current=&receivingTask;
 assert(!ProcessUserMapWrite(pinned,authorized,"7 1000 1\n",9,false,false));
 assert(ProcessUserMapWrite(pinned,authorized,"7 1000 1\n",9,false,false)==-1);
 n=ProcessUserMapRead(pinned,authorized.ns,false,false,text,sizeof(text));assert(n==9&&!memcmp(text,"7 1000 1\n",n));
 n=ProcessUserMapRead(pinned,receivingTask.userNamespace,false,false,text,sizeof(text));
 assert(n==15&&!memcmp(text,"7 4294967295 1\n",n));
 UserNamespaceRelease(authorized.ns);UserNamespaceRelease(receivingTask.userNamespace);assert(!live);current=&parent;
 puts("actual user credential mapping/authority/unwind/concurrent publication passed");
}
