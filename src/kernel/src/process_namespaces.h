#pragma once
#include <stdint.h>

namespace brook {
struct Process;
struct UtsNamespace;
struct IpcNamespace;
struct UserNamespace;
inline constexpr uint64_t CloneNewUts = 0x04000000;
inline constexpr uint64_t CloneNewIpc = 0x08000000;
inline constexpr uint64_t CloneNewRuntimeNamespaces = CloneNewUts|CloneNewIpc;
inline constexpr unsigned UtsNameBytes = 65;
void ProcessNamespacesInherit(Process* target,Process* source);
void ProcessNamespacesRelease(Process* process);
int ProcessNamespacesIsolate(Process* process,uint64_t flags);
// Borrowed from the calling task. Stable until it changes its IPC namespace
// or exits. Null means the initial domain.
const IpcNamespace* ProcessIpcScope(Process* process);
UserNamespace* ProcessIpcOwner(const IpcNamespace* scope);
void ProcessUtsRead(Process* process,char hostname[UtsNameBytes],char domain[UtsNameBytes]);
int ProcessUtsWrite(Process* process,bool domain,const char* value,unsigned length);
}
