#pragma once
#include <stdint.h>
namespace brook {
// Only a validated snapshot may produce a terminal file outcome. IO/preparation
// results must be rechecked against both mapping and file generations first.
enum class MappingFaultResult { Unhandled, Resolved, EndOfFile, IoError, OutOfMemory };
enum class UserFaultOrigin { KernelAccess, UserInstruction };
MappingFaultResult ResolveUserMappingFault(uint64_t address,uint64_t error,UserFaultOrigin origin=UserFaultOrigin::KernelAccess);
}
