// Linux x86-64 seccomp's classic BPF input and instruction ABI.
#pragma once

#include <stdint.h>

namespace brook {
struct SeccompInstruction {
    uint16_t code;
    uint8_t jumpTrue;
    uint8_t jumpFalse;
    uint32_t value;
};

struct SeccompData {
    uint32_t syscall;
    uint32_t architecture;
    uint64_t instructionPointer;
    uint64_t arguments[6];
};

static_assert(sizeof(SeccompInstruction) == 8);
static_assert(sizeof(SeccompData) == 64);
static_assert(__builtin_offsetof(SeccompData, arguments) == 16);

inline constexpr unsigned SeccompMaxFilterInstructions = 4096;
inline constexpr unsigned SeccompMaxPathInstructions = 32768;
inline constexpr uint32_t SeccompX86_64 = 0xc000003e;
inline constexpr uint32_t SeccompKillProcess = 0x80000000;
inline constexpr uint32_t SeccompKillThread = 0x00000000;
inline constexpr uint32_t SeccompTrap = 0x00030000;
inline constexpr uint32_t SeccompErrno = 0x00050000;
inline constexpr uint32_t SeccompUserNotify = 0x7fc00000;
inline constexpr uint32_t SeccompTrace = 0x7ff00000;
inline constexpr uint32_t SeccompLog = 0x7ffc0000;
inline constexpr uint32_t SeccompAllow = 0x7fff0000;
inline constexpr uint32_t SeccompActionMask = 0xffff0000;
inline constexpr int SeccompSignal = 31;

bool SeccompVerify(const SeccompInstruction* instructions, unsigned count);
uint32_t SeccompExecute(const SeccompInstruction* instructions, unsigned count, const SeccompData& data);
bool SeccompSupportedAction(uint32_t action);
uint32_t SeccompSelectAction(uint32_t previous, uint32_t candidate);
uint32_t SeccompEnforceAction(uint32_t verdict);
}
