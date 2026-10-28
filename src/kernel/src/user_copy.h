#pragma once
#include <stdint.h>

namespace brook {
// With a live UserAccessScope, validation also excludes eviction of the range
// until that lexical scope ends. Without it, these remain validation only.
bool UserBufferReadable(uint64_t address,uint64_t length);
bool UserBufferWritable(uint64_t address,uint64_t length);
// Trusted kernel buffer, user virtual address. Return bytes actually copied;
// a raced unmap/protection/EOF failure may leave a partially copied prefix.
// Prefault/retry can sleep: never hold the inode publication gate or AS/PT locks.
uint64_t CopyFromUserBytes(void* destination,uint64_t source,uint64_t length);
uint64_t CopyToUserBytes(uint64_t destination,const void* source,uint64_t length);
inline bool CopyFromUser(void* destination,uint64_t source,uint64_t length) {
    return CopyFromUserBytes(destination,source,length)==length;
}
inline bool CopyToUser(uint64_t destination,const void* source,uint64_t length) {
    return CopyToUserBytes(destination,source,length)==length;
}
bool CopyUserCString(uint64_t address,char* destination,uint32_t capacity);
// Same page-bounded copy, with distinct EFAULT and ENAMETOOLONG outcomes.
int CopyUserCStringResult(uint64_t address,char* destination,uint32_t capacity);
}
