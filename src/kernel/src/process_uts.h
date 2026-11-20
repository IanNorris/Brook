#pragma once
#include <stdint.h>

namespace brook {
struct Process;
struct UtsNamespace;
inline constexpr uint64_t CloneNewUts = 0x04000000;
inline constexpr unsigned UtsNameBytes = 65;
void ProcessUtsInherit(Process* target,Process* source);
void ProcessUtsRelease(Process* process);
int ProcessUtsIsolate(Process* process);
void ProcessUtsRead(Process* process,char hostname[UtsNameBytes],char domain[UtsNameBytes]);
int ProcessUtsWrite(Process* process,bool domain,const char* value,unsigned length);
}
