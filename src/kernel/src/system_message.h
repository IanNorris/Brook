#pragma once
#include "../../shared/brook_system_message.h"
#include <stdint.h>
namespace brook {
// Resident bounded queue. Publication/read never allocate or call user code.
bool SystemMessagePublish(const BrookSystemMessage&);
bool SystemMessageReadAfter(uint64_t sequence,uint32_t uid,BrookSystemMessage*);
uint64_t SystemMessageLastSequence();
}
