#pragma once
#include "idt.h"
namespace brook {
#ifdef BROOK_REGISTER_TEST_HOOKS
int64_t RegisterTestControl(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
bool RegisterTestException(FullExceptionFrame*,uint64_t vector,uint64_t address);
void RegisterTestReschedArrival();
#else
inline bool RegisterTestException(FullExceptionFrame*,uint64_t,uint64_t) {return false;}
inline void RegisterTestReschedArrival() {}
#endif
}
