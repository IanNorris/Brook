#pragma once
#include <stdint.h>

namespace brook {
struct MqueueDomain;
struct UserNamespace;
struct Process;
struct MountNamespace;
struct VfsPath;
bool MqueueRegisterFilesystem();
int MqueueMount(Process* process,MountNamespace* ns,const VfsPath& target,uint64_t flags);

MqueueDomain* MqueueDomainCreate(UserNamespace* owner);
void MqueueDomainRetire(MqueueDomain* domain);
MqueueDomain* ProcessMqueueDomain(Process* process);
MqueueDomain* MqueueInitialDomain();
}
