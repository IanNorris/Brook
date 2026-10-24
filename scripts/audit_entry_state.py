#!/usr/bin/env python3
"""Audit supported compiled x86-64 entry templates; inventory unsupported returns.

Run in nix-shell. This is a bounded structural check under the SysV callee ABI,
not a proof of arbitrary handler bodies or hardware interrupt delivery.
"""
import argparse
import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
from elftools.elf.elffile import ELFFile

REGS = "rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15".split()
UNMODELED_ENTRIES = {
    "_ZN5brookL19ForkChildTrampolineEv",
    "BrookSyscallDispatcher", "_ZN5brook16SwitchToUserModeEmm",
    "PanicNmiHandlerAsm",
}
SPECIAL_ENTRIES = {"_ZN5brookL17LapicTimerHandlerEv": "timer",
                   "_ZN5brookL19TlbShootdownHandlerEv": "tlb",
                   "_ZN5brookL17ReschedIpiHandlerEv": "resched"}
DECODER = Cs(CS_ARCH_X86, CS_MODE_64)
DECODER.detail = True


class Invalid(ValueError):
    pass


def audit(instructions, error_vector=None, profile="irq"):
    """Recognize the entire template, including branch targets, not a checklist.

    A mismatch fails for explicitly nominated full-exception stubs. Other return
    paths are unsupported until their complete template/control flow is modeled.
    """
    i = 0

    def take(mnemonic, operands=None):
        nonlocal i
        if i >= len(instructions):
            raise Invalid(f"missing {mnemonic} at end")
        ins = instructions[i]
        if ins.mnemonic != mnemonic or (operands is not None and ins.op_str != operands):
            raise Invalid(f"{ins.address:#x}: expected {mnemonic} {operands or ''}; got {ins.mnemonic} {ins.op_str}")
        i += 1
        return ins

    if error_vector == 6:
        take("push", "0")
    for reg in REGS:
        take("push", reg)
    take("mov", "ecx, 0xc0000101")
    take("rdmsr")
    take("test", "edx, 0x80000000")
    kernel_branch = take("jne")
    take("swapgs")
    take("mov", "ebx, 1")
    done_branch = take("jmp")
    kernel_label = take("xor", "ebx, ebx")
    done_label = instructions[i]
    mask = (1 << 64) - 1
    if kernel_branch.operands[0].imm & mask != kernel_label.address or done_branch.operands[0].imm & mask != done_label.address:
        raise Invalid("GS entry branch target changed")
    vector_save = profile != "tlb"
    if vector_save:
        take("mov", "r14, rsp")
    if error_vector is not None:
        take("mov", "rdi, r14")
        take("mov", f"rsi, {hex(error_vector) if error_vector >= 10 else error_vector}")
    if vector_save:
        take("sub", "rsp, 0x480")
        take("and", "rsp, 0xffffffffffffffc0")
        take("mov", "r15, rsp")
        take("xor", "eax, eax")
        for offset in range(512, 576, 8):
            take("mov", f"qword ptr [r15 + {offset:#x}], rax")
        take("mov", "eax, 7")
        take("xor", "edx, edx")
        take("xsave64", "ptr [r15]")
    if profile == "timer":
        take("mov", "rdi, qword ptr [r14 + 0x78]")
        take("mov", "rsi, qword ptr [r14 + 0x80]")
        take("mov", "rdx, qword ptr [r14 + 0x40]")
    if profile == "resched":
        take("mov", "rdi, qword ptr [r14 + 0x80]")
    take("cld")
    call = take("call")
    if len(call.operands) != 1 or call.operands[0].type != X86_OP_IMM:
        raise Invalid("unmodeled indirect C call")
    if profile == "timer":
        take("lea", "rdi, [r14 + 0x78]")
        validate = take("call")
        if len(validate.operands) != 1 or validate.operands[0].type != X86_OP_IMM:
            raise Invalid("unmodeled timer validator call")
    if vector_save:
        take("mov", "eax, 7")
        take("xor", "edx, edx")
        take("xrstor64", "ptr [r15]")
        take("mov", "rsp, r14")
    take("test", "ebx, ebx")
    no_swap = take("je")
    take("swapgs")
    first_pop = instructions[i].address
    if no_swap.operands[0].imm & mask != first_pop:
        raise Invalid("GS exit branch target changed")
    for reg in reversed(REGS):
        take("pop", reg)
    if error_vector is not None:
        take("add", "rsp, 8")
    take("pushfq")
    take("and", "qword ptr [rsp], 0xfffffffffffbbbff")
    take("popfq")
    take("iretq")
    if any(ins.mnemonic not in {"nop", "int3"} for ins in instructions[i:]):
        raise Invalid("unmodeled trailing instructions")
    checks = ["all-GPR-save-and-reverse-restore", "frame-slot-discard",
              "DF-clear-before-C", "GS-entry-exit-branch-targets",
              "live-IRET-flags-normalized"]
    if vector_save:
        checks += ["RSP-restored-from-saved-frame", "C-call-64-byte-alignment",
                   "XSAVE-XRSTOR-pair", "XSAVE-header-zeroed"]
    else:
        checks += ["RSP-balanced-pushes-pops", "C-call-alignment-from-hardware-IRQ-frame"]
    if profile == "timer":
        checks += ["interrupted-RIP-CS-RBP-slot-offsets", "validator-before-vector-restore"]
    if profile == "resched":
        checks += ["interrupted-CS-slot-offset"]
    return {"contract": "full-exception" if error_vector else profile,
            "error_slot_bytes": 8 if error_vector else 0,
            "call_target": hex(call.operands[0].imm & ((1 << 64) - 1)),
            "checks": checks}


def nominated(name):
    return (name in {"ExceptionStub6", "ExceptionStub13", "ExceptionStub14"} or name in SPECIAL_ENTRIES
            or any(part in name for part in ("SharedIrqStub", "KbdIrqHandler", "MouseIrqHandler")) and not name.endswith("Inner")
            or "RegisterTestIrqWrapper" in name)


def functions(path):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        if elf.get_machine_arch() != "x64":
            raise Invalid("x86-64 ELF required")
        symbols = elf.get_section_by_name(".symtab")
        if symbols is None:
            raise Invalid("ELF symbol table required; do not audit a stripped image")
        for symbol in symbols.iter_symbols():
            if symbol["st_info"]["type"] != "STT_FUNC" or not symbol["st_size"]:
                continue
            section_id = symbol["st_shndx"]
            if not isinstance(section_id, int):
                continue
            section = elf.get_section(section_id)
            offset = symbol["st_value"] - section["sh_addr"]
            data = section.data()[offset:offset + symbol["st_size"]]
            ins = list(DECODER.disasm(data, symbol["st_value"]))
            if (not nominated(symbol.name) and symbol.name not in UNMODELED_ENTRIES
                    and not any(x.mnemonic in {"iretq", "sysretq", "sysret"} for x in ins)):
                continue
            if sum(x.size for x in ins) != len(data):
                raise Invalid(f"incomplete decode: {symbol.name}")
            yield symbol.name, data, ins


def scalar_leaf(path, address, mutation=None):
    """Prove the non-XSAVE TLB callee has no vector/FPU operations or callees.

    Deliberately narrow scalar instruction model, including local branch bounds.
    A new instruction or out-of-function tail call requires an explicit review.
    """
    # MFENCE is scalar and preserves registers/FPU state; the memory-barrier
    # epoch shares the TLB IPI's existing GPR-only interrupt wrapper.
    allowed = {"push", "pop", "mov", "cmp", "test", "xor", "bt", "ret",
               "ja", "je", "jmp", "jae", "jbe", "invlpg", "nop", "mfence"}
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name(".symtab")
        symbol = next((s for s in symbols.iter_symbols() if s["st_value"] == address
                       and s["st_info"]["type"] == "STT_FUNC" and s["st_size"]), None)
        if symbol is None:
            raise Invalid("TLB callee lacks a bounded ELF function")
        section = elf.get_section(symbol["st_shndx"])
        offset = address - section["sh_addr"]
        data = section.data()[offset:offset + symbol["st_size"]]
        if mutation:
            original = list(DECODER.disasm(data, address))
            if mutation == "vector-clobber":
                if data[:4] != b"\x55\x48\x89\xe5":
                    raise RuntimeError("scalar mutation prologue fixture changed")
                data = b"\x66\x0f\xef\xc0" + data[4:]  # PXOR XMM0,XMM0
            else:
                target = next(x for x in original if
                              (mutation in {"callee-call", "external-branch"} and x.size >= 5 and x.mnemonic == "mov")
                              or (mutation == "missing-return" and x.mnemonic == "ret"))
                if mutation == "callee-call":
                    replacement = b"\xe8\0\0\0\0" + b"\x90" * (target.size - 5)
                elif mutation == "external-branch":
                    # A fixed rel8 displacement may land inside a larger leaf.
                    # Always target well past this function, regardless of size.
                    displacement=address+len(data)+4096-(target.address+5)
                    replacement=b"\xe9"+displacement.to_bytes(4,"little",signed=True)+b"\x90"*(target.size-5)
                else:
                    replacement = b"\x90" * target.size
                position = target.address - address
                data = data[:position] + replacement + data[position + target.size:]
        ins = list(DECODER.disasm(data, address))
        if sum(x.size for x in ins) != len(data):
            raise Invalid("incomplete scalar-leaf decode")
        starts = {x.address for x in ins}
        for x in ins:
            if x.mnemonic not in allowed:
                raise Invalid(f"TLB leaf unmodeled instruction: {x.mnemonic} {x.op_str}")
            if x.mnemonic.startswith("j"):
                if len(x.operands) != 1 or x.operands[0].type != X86_OP_IMM or (x.operands[0].imm & ((1 << 64) - 1)) not in starts:
                    raise Invalid("TLB leaf branch is not a local instruction boundary")
            if x.mnemonic == "ret" and x.op_str:
                raise Invalid("unmodeled TLB leaf return stack adjustment")
        if not any(x.mnemonic == "ret" for x in ins):
            raise Invalid("TLB leaf has no return")
        return {"function": symbol.name, "instructions": len(ins),
                "proof": "bounded scalar leaf; no calls, external branches or FPU/vector instructions"}


def mutation_checks(name, data, instructions, vector, profile="irq"):
    """Decode mutated machine bytes and require the audit to reject them."""
    results = []
    cases = [
        ("DF-STD", next(x for x in instructions if x.mnemonic == "cld"), b"\xfd"),
        ("wrong-GPR-pop", next(x for x in instructions if x.mnemonic == "pop" and x.op_str == "r15"), b"\x41\x5e"),
        ("wrong-GS-branch", next(x for x in instructions if x.mnemonic == "jne"), None),
    ]
    if profile != "tlb":
        cases.append(("missing-XSAVE", next(x for x in instructions if x.mnemonic == "xsave64"), None))
    if profile == "timer":
        ins = next(x for x in instructions if x.mnemonic == "mov" and x.op_str == "rdi, qword ptr [r14 + 0x78]")
        cases.append(("wrong-interrupted-RIP-slot", ins, bytes(ins.bytes[:-1]) + b"\x70"))
    if profile == "resched":
        ins = next(x for x in instructions if x.mnemonic == "mov" and x.op_str == "rdi, qword ptr [r14 + 0x80]")
        cases.append(("wrong-interrupted-CS-slot", ins, bytes(ins.bytes[:-4]) + b"\x78\0\0\0"))
    if vector is not None:
        cases.append(("wrong-error-discard", next(x for x in instructions if x.mnemonic == "add" and x.op_str == "rsp, 8"), b"\x48\x83\xc4\x10"))
    for label, ins, replacement in cases:
        if label == "wrong-GS-branch":
            changed = bytearray(ins.bytes)
            changed[-1] ^= 1
            replacement = bytes(changed)
        replacement = replacement or b"\x90" * ins.size
        if len(replacement) != ins.size:
            raise Invalid("mutation fixture length changed")
        offset = ins.address - instructions[0].address
        mutated = data[:offset] + replacement + data[offset + ins.size:]
        try:
            audit(list(DECODER.disasm(mutated, instructions[0].address)), vector, profile)
        except Invalid:
            results.append({"function": name, "mutation": label, "rejected": True})
        else:
            raise Invalid(f"mutation accepted: {name}: {label}")
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--mutations", action="store_true")
    parser.add_argument("--require-all", action="store_true", help="Fail if any return path is unsupported")
    args = parser.parse_args()
    report = {"elf": str(args.elf.resolve()), "sha256": hashlib.sha256(args.elf.read_bytes()).hexdigest(),
              "scope": "compiled entry templates under SysV callee ABI; not arbitrary-body verification",
              "checked": [], "failed": [], "unsupported": [], "mutations": []}
    mandatory = {"ExceptionStub6": 6, "ExceptionStub13": 13, "ExceptionStub14": 14}
    seen = set()
    for name, data, ins in functions(args.elf):
        seen.add(name)
        vector = mandatory.get(name)
        profile = SPECIAL_ENTRIES.get(name, "irq")
        # Explicit family nomination: a damaged known wrapper must fail, rather
        # than silently reclassifying itself as unsupported when its prefix changes.
        if not nominated(name):
            report["unsupported"].append(name)
            continue
        try:
            result = audit(ins, vector, profile)
            if profile == "tlb":
                result["scalar_callee"] = scalar_leaf(args.elf, int(result["call_target"], 16))
            report["checked"].append({"function": name, **result})
            if args.mutations:
                report["mutations"].extend(mutation_checks(name, data, ins, vector, profile))
                if profile == "tlb":
                    for mutation in ("vector-clobber", "callee-call", "external-branch", "missing-return"):
                        try:
                            scalar_leaf(args.elf, int(result["call_target"], 16), mutation)
                        except Invalid:
                            report["mutations"].append({"function": result["scalar_callee"]["function"],
                                                        "mutation": mutation, "rejected": True})
                        else:
                            raise Invalid(f"scalar callee mutation accepted: {mutation}")
        except Invalid as error:
            report["failed"].append({"function": name, "reason": str(error)})
    for name in (mandatory.keys() | UNMODELED_ENTRIES | SPECIAL_ENTRIES.keys()) - seen:
        report["failed"].append({"function": name, "reason": "required entry symbol missing"})
    for family, count in (("SharedIrqStub", 4), ("KbdIrqHandler", 1), ("MouseIrqHandler", 1)):
        if sum(family in name and not name.endswith("Inner") for name in seen) != count:
            report["failed"].append({"function": family, "reason": "required handler family missing or changed"})
    report["pass"] = not report["failed"] and (not args.require_all or not report["unsupported"])
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered)
    print(rendered, end="")
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
