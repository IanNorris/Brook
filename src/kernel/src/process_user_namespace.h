#pragma once
#include "user_namespace.h"
namespace brook {
struct Process;
struct SignalInfo;
bool ProcessCanSignal(Process* sender,Process* target);
void ProcessSignalInfoToUser(Process* viewer,SignalInfo* info);
UserNamespace* ProcessUserNamespaceAcquire(Process* process);
int ProcessUserNamespaceCreate(Process* unpublishedChild);
bool ProcessCapable(Process* process,UserNamespace* target,unsigned capability);
// Scalar socket metadata only; grants no memory, directory, or FD-open capability.
struct SocketIdentity;
// 1: copied socket; 0: another FD kind; negative: denied/missing.
int ProcessReadSocketMetadata(Process* viewer,Process* target,uint32_t fd,SocketIdentity* out);
uint32_t ProcessReportId(Process* viewer,bool group,uint32_t canonical);
uint32_t ProcessResolveId(Process* actor,bool group,uint32_t visible);
// Canonical IDs remain available through ProcessReadIds for internal checks.
int ProcessReadCapabilities(Process*,UserCapabilityState*);
int ProcessSetCapabilities(Process*,uint64_t effective,uint64_t permitted,uint64_t inheritable);
int ProcessCapabilityControl(Process*,unsigned option,uint64_t argument);
bool ProcessSetgroupsAllowed(Process*);
UserAuthority ProcessUserAuthorityAcquire(Process*); // owns ns ref
int ProcessUserMapWrite(UserNamespace*,const UserAuthority& opened,const char*,uint64_t,bool group,bool setgroups);
uint32_t ProcessUserMapRead(UserNamespace*,UserNamespace* viewer,bool group,bool setgroups,char* out,uint32_t capacity);
}
