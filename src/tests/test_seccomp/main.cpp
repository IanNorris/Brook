#include "seccomp_bpf.h"

#include <cassert>

using namespace brook;

template <unsigned N> bool verified(const SeccompInstruction (&code)[N])
{
    return SeccompVerify(code, N);
}

template <unsigned N> uint32_t execute(const SeccompInstruction (&code)[N], const SeccompData& data)
{
    assert(verified(code));
    return SeccompExecute(code, N, data);
}

int main()
{
    const SeccompInstruction policy[] = {
        {0x20, 0, 0, 4},                        // audit architecture
        {0x15, 1, 0, SeccompX86_64},
        {0x06, 0, 0, SeccompKillProcess},
        {0x20, 0, 0, 0},                        // syscall number
        {0x15, 0, 4, 16},                       // ioctl
        {0x20, 0, 0, 24},                       // low bits of arg 1
        {0x54, 0, 0, 0xff00},
        {0x15, 0, 1, 0xde00},
        {0x06, 0, 0, SeccompErrno | 13},
        {0x06, 0, 0, SeccompAllow},
    };
    SeccompData input{16, SeccompX86_64, 0x123456789ull, {5, 0x30000de22ull}};
    assert(execute(policy, input) == (SeccompErrno | 13));
    input.architecture = 0x40000003;
    assert(execute(policy, input) == SeccompKillProcess);
    input.architecture = SeccompX86_64;
    input.arguments[1] = 0;
    assert(execute(policy, input) == SeccompAllow);

    const SeccompInstruction scratchAndIp[] = {
        {0x20, 0, 0, 12},                       // high bits of instruction pointer
        {0x02, 0, 0, 15},
        {0x61, 0, 0, 15},
        {0x81, 0, 0, 0},
        {0x80, 0, 0, 0},
        {0x1c, 0, 0, 0},                        // A -= X
        {0x61, 0, 0, 15},
        {0x0c, 0, 0, 0},
        {0x16, 0, 0, 0},
    };
    assert(execute(scratchAndIp, input) == 1);

    const SeccompInstruction join[] = {
        {0x15, 1, 0, 0}, {0x02, 0, 0, 0},
        {0x60, 0, 0, 0}, {0x16, 0, 0, 0},
    };
    assert(!verified(join));
    const SeccompInstruction farJump[] = {{0x05, 0, 0, 0xffffffff}, {0x06, 0, 0, 0}};
    const SeccompInstruction farBranch[] = {{0x15, 1, 0, 0}, {0x06, 0, 0, 0}};
    const SeccompInstruction shift[] = {{0x64, 0, 0, 32}, {0x16, 0, 0, 0}};
    const SeccompInstruction opcode[] = {{0x28, 0, 0, 0}, {0x16, 0, 0, 0}};
    const SeccompInstruction scratchRange[] = {{0x02, 0, 0, 16}, {0x06, 0, 0, 0}};
    const SeccompInstruction alignment[] = {{0x20, 0, 0, 3}, {0x16, 0, 0, 0}};
    const SeccompInstruction width[] = {{0x20, 0, 0, 64}, {0x16, 0, 0, 0}};
    const SeccompInstruction fallThrough[] = {{0x20, 0, 0, 0}};
    const SeccompInstruction constantDivide[] = {{0x34, 0, 0, 0}, {0x16, 0, 0, 0}};
    const SeccompInstruction variableDivide[] = {{0x3c, 0, 0, 0}, {0x16, 0, 0, 0}};
    assert(!verified(farJump) && !verified(farBranch) && !verified(shift) && !verified(opcode));
    assert(!verified(scratchRange) && !verified(alignment) && !verified(width));
    assert(!verified(fallThrough) && !verified(constantDivide));
    assert(execute(variableDivide, input) == SeccompKillThread);

    assert(!SeccompVerify(policy, 0));
    assert(!SeccompVerify(policy, SeccompMaxFilterInstructions + 1));
    assert(SeccompSupportedAction(SeccompAllow) && !SeccompSupportedAction(0x7fc00000));
    assert(SeccompSelectAction(SeccompAllow, SeccompErrno | 1) == (SeccompErrno | 1));
    assert(SeccompSelectAction(SeccompErrno | 1, SeccompErrno | 2) == (SeccompErrno | 1));
    assert(SeccompSelectAction(SeccompErrno | 1, SeccompTrap | 3) == (SeccompTrap | 3));
    assert(SeccompSelectAction(SeccompTrap, SeccompKillThread) == SeccompKillThread);
    assert(SeccompSelectAction(SeccompKillThread, SeccompKillProcess) == SeccompKillProcess);
    assert(SeccompSelectAction(SeccompAllow, 0x7fc00000) == SeccompKillProcess);
}
