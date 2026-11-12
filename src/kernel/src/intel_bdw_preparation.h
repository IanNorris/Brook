#pragma once
#include <stdint.h>
namespace brook {
// Stack-owned by an exec ioctl from before decoding until publication or
// rejected-snapshot cleanup completes. Never copy while registered.
struct BdwSubmissionPreparation {uint64_t owner=0;BdwSubmissionPreparation* next=nullptr;};
}
