#include "seccomp_bpf.h"

namespace brook {
namespace {
// Exact classic BPF opcodes accepted for seccomp_data, with no packet or
// indirect loads. The X-register source modifier is bit 3.
enum Opcode : uint16_t {
    LoadA = 0x00, LoadX = 0x01, LoadAbsolute = 0x20,
    LoadMemA = 0x60, LoadMemX = 0x61,
    LoadLengthA = 0x80, LoadLengthX = 0x81,
    StoreA = 0x02, StoreX = 0x03, Tax = 0x07, Txa = 0x87,
    Add = 0x04, Sub = 0x14, Mul = 0x24, Div = 0x34,
    Or = 0x44, And = 0x54, Lsh = 0x64, Rsh = 0x74,
    Neg = 0x84, Xor = 0xa4,
    Jump = 0x05, Eq = 0x15, Gt = 0x25, Ge = 0x35, Set = 0x45,
    ReturnK = 0x06, ReturnA = 0x16,
};
constexpr uint16_t UseX = 0x08;
constexpr unsigned ScratchWords = 16;
constexpr uint32_t Reachable = 1u << ScratchWords;
}

bool SeccompVerify(const SeccompInstruction* code, unsigned count)
{
    if (!code || !count || count > SeccompMaxFilterInstructions) return false;
    if (code[count - 1].code != ReturnA && code[count - 1].code != ReturnK) return false;

    // Jumps only go forward. A scratch word is initialized at a join only
    // if every incoming path has stored it.
    uint32_t scratch[SeccompMaxFilterInstructions]{};
    scratch[0] = Reachable;
    auto propagate = [&](unsigned target, uint32_t initialized) {
        if (initialized)
            scratch[target] = scratch[target] ? scratch[target] & initialized : initialized;
    };
    for (unsigned i = 0; i < count; ++i) {
        const auto& insn = code[i];
        const unsigned remaining = count - i - 1;
        uint32_t initialized = scratch[i];
        switch (insn.code) {
        case LoadAbsolute:
            if ((insn.value & 3) || insn.value > sizeof(SeccompData) - sizeof(uint32_t)) return false;
            break;
        case LoadMemA: case LoadMemX:
            if (insn.value >= ScratchWords || (initialized && !(initialized & (1u << insn.value))))
                return false;
            break;
        case StoreA: case StoreX:
            if (insn.value >= ScratchWords) return false;
            if (initialized) initialized |= 1u << insn.value;
            break;
        case Div:
            if (!insn.value) return false;
            break;
        case Lsh: case Rsh:
            if (insn.value >= 32) return false;
            break;
        case Jump:
            if (insn.value >= remaining) return false;
            propagate(i + 1 + insn.value, initialized);
            continue;
        case Eq: case Eq | UseX: case Gt: case Gt | UseX:
        case Ge: case Ge | UseX: case Set: case Set | UseX:
            if (insn.jumpTrue >= remaining || insn.jumpFalse >= remaining) return false;
            propagate(i + 1 + insn.jumpTrue, initialized);
            propagate(i + 1 + insn.jumpFalse, initialized);
            continue;
        case ReturnK: case ReturnA: continue;
        case LoadA: case LoadX: case LoadLengthA: case LoadLengthX: case Tax: case Txa:
        case Add: case Add | UseX: case Sub: case Sub | UseX: case Mul: case Mul | UseX:
        case Div | UseX: case Or: case Or | UseX: case And: case And | UseX:
        case Xor: case Xor | UseX: case Lsh | UseX: case Rsh | UseX: case Neg:
            break;
        default: return false;
        }
        if (!remaining) return false;
        propagate(i + 1, initialized);
    }
    return true;
}

uint32_t SeccompExecute(const SeccompInstruction* code, unsigned count, const SeccompData& input)
{
    uint32_t a = 0, x = 0, scratch[ScratchWords]{};
    // Only immutable, verified code reaches here. No instruction can move
    // backwards, so execution is bounded by its instruction count.
    for (unsigned pc = 0; pc < count; ++pc) {
        const auto& insn = code[pc];
        const auto k = insn.value;
        const uint32_t source = insn.code & UseX ? x : k;
        switch (insn.code) {
        case LoadA: a = k; break;
        case LoadX: x = k; break;
        case LoadAbsolute:
            __builtin_memcpy(&a, reinterpret_cast<const uint8_t*>(&input) + k, sizeof(a));
            break;
        case LoadLengthA: a = sizeof(input); break;
        case LoadLengthX: x = sizeof(input); break;
        case LoadMemA: a = scratch[k]; break;
        case LoadMemX: x = scratch[k]; break;
        case StoreA: scratch[k] = a; break;
        case StoreX: scratch[k] = x; break;
        case Tax: x = a; break;
        case Txa: a = x; break;
        case Add: case Add | UseX: a += source; break;
        case Sub: case Sub | UseX: a -= source; break;
        case Mul: case Mul | UseX: a *= source; break;
        case Div: case Div | UseX:
            if (!source) return SeccompKillThread;
            a /= source;
            break;
        case Or: case Or | UseX: a |= source; break;
        case And: case And | UseX: a &= source; break;
        case Xor: case Xor | UseX: a ^= source; break;
        case Lsh: case Lsh | UseX: a <<= source & 31; break;
        case Rsh: case Rsh | UseX: a >>= source & 31; break;
        case Neg: a = 0 - a; break;
        case Jump: pc += k; break;
        case Eq: case Eq | UseX: pc += a == source ? insn.jumpTrue : insn.jumpFalse; break;
        case Gt: case Gt | UseX: pc += a > source ? insn.jumpTrue : insn.jumpFalse; break;
        case Ge: case Ge | UseX: pc += a >= source ? insn.jumpTrue : insn.jumpFalse; break;
        case Set: case Set | UseX: pc += a & source ? insn.jumpTrue : insn.jumpFalse; break;
        case ReturnK: return k;
        case ReturnA: return a;
        default: return SeccompKillProcess;
        }
    }
    return SeccompKillProcess;
}

bool SeccompSupportedAction(uint32_t action)
{
    switch (action) {
    case SeccompKillProcess: case SeccompKillThread: case SeccompTrap:
    case SeccompErrno: case SeccompAllow: return true;
    default: return false;
    }
}

uint32_t SeccompSelectAction(uint32_t previous, uint32_t candidate)
{
    if (!SeccompSupportedAction(candidate & SeccompActionMask)) return SeccompKillProcess;
    // Linux action ordering is signed, so KILL_PROCESS compares lowest. The
    // newest filter wins the data when the actions have equal precedence.
    return static_cast<int32_t>(candidate & SeccompActionMask) <
        static_cast<int32_t>(previous & SeccompActionMask) ? candidate : previous;
}
}
