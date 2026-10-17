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
    "_ZN5brookL19ForkChildTrampolineEv", "_ZN5brookL17LapicTimerHandlerEv",
    "_ZN5brookL17ReschedIpiHandlerEv", "_ZN5brookL19TlbShootdownHandlerEv",
    "BrookSyscallDispatcher", "_ZN5brook16SwitchToUserModeEmm",
}
DECODER = Cs(CS_ARCH_X86, CS_MODE_64)
DECODER.detail = True


class Invalid(ValueError):
    pass


def audit(instructions, error_vector=None):
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
    done_label = take("mov", "r14, rsp")
    mask = (1 << 64) - 1
    if kernel_branch.operands[0].imm & mask != kernel_label.address or done_branch.operands[0].imm & mask != done_label.address:
        raise Invalid("GS entry branch target changed")
    if error_vector is not None:
        take("mov", "rdi, r14")
        take("mov", f"rsi, {hex(error_vector) if error_vector >= 10 else error_vector}")
    take("sub", "rsp, 0x480")
    take("and", "rsp, 0xffffffffffffffc0")
    take("mov", "r15, rsp")
    take("xor", "eax, eax")
    for offset in range(512, 576, 8):
        take("mov", f"qword ptr [r15 + {offset:#x}], rax")
    take("mov", "eax, 7")
    take("xor", "edx, edx")
    take("xsave64", "ptr [r15]")
    take("cld")
    call = take("call")
    if len(call.operands) != 1 or call.operands[0].type != X86_OP_IMM:
        raise Invalid("unmodeled indirect C call")
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
    return {"contract": "full-exception" if error_vector else "naked-irq",
            "error_slot_bytes": 8 if error_vector else 0,
            "call_target": hex(call.operands[0].imm & ((1 << 64) - 1)),
            "checks": ["all-GPR-save-and-reverse-restore", "frame-slot-discard",
                       "RSP-restored-from-saved-frame", "C-call-64-byte-alignment",
                       "DF-clear-before-C", "XSAVE-XRSTOR-pair",
                       "XSAVE-header-zeroed", "GS-entry-exit-branch-targets",
                       "live-IRET-flags-normalized"]}


def nominated(name):
    return (name in {"ExceptionStub6", "ExceptionStub13", "ExceptionStub14"}
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


def mutation_checks(name, data, instructions, vector):
    """Decode mutated machine bytes and require the audit to reject them."""
    results = []
    cases = [
        ("DF-STD", next(x for x in instructions if x.mnemonic == "cld"), b"\xfd"),
        ("wrong-GPR-pop", next(x for x in instructions if x.mnemonic == "pop" and x.op_str == "r15"), b"\x41\x5e"),
        ("missing-XSAVE", next(x for x in instructions if x.mnemonic == "xsave64"), None),
        ("wrong-GS-branch", next(x for x in instructions if x.mnemonic == "jne"), None),
    ]
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
            audit(list(DECODER.disasm(mutated, instructions[0].address)), vector)
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
        # Explicit family nomination: a damaged known wrapper must fail, rather
        # than silently reclassifying itself as unsupported when its prefix changes.
        if not nominated(name):
            report["unsupported"].append(name)
            continue
        try:
            result = audit(ins, vector)
            report["checked"].append({"function": name, **result})
            if args.mutations:
                report["mutations"].extend(mutation_checks(name, data, ins, vector))
        except Invalid as error:
            report["failed"].append({"function": name, "reason": str(error)})
    for name in (mandatory.keys() | UNMODELED_ENTRIES) - seen:
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
