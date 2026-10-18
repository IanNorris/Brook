#!/usr/bin/env python3
"""Check the compiled first-dispatch call contract, not arbitrary scheduler safety.

Both startup paths must reach context_switch through RestoreFirstProcess and
must not bypass saved continuations by directly entering user mode or a thread
trampoline. --mutations checks rejected redirections of each required edge.
"""
import argparse
import hashlib
import json
from pathlib import Path
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
from elftools.elf.elffile import ELFFile

STARTS = ('_ZN5brook14SchedulerStartEv', '_ZN5brook16SchedulerStartApEv')
RESTORE = '_ZN5brookL19RestoreFirstProcessEPNS_7ProcessEj'
FORBIDDEN = ('_ZN5brook16SwitchToUserModeEmm', '_ZN5brook22KernelThreadTrampolineEv')


def check(edges, addresses):
    for name in STARTS:
        if addresses[RESTORE] not in edges[name]:
            raise ValueError(name + ': missing saved-context restore')
    if addresses['context_switch'] not in edges[RESTORE]:
        raise ValueError('restore helper bypasses context_switch')
    for name in (*STARTS, RESTORE):
        if any(addresses[f] in edges[name] for f in FORBIDDEN):
            raise ValueError(name + ': directly restarts a task')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    with args.elf.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s for s in elf.get_section_by_name('.symtab').iter_symbols()}
        names = (*STARTS, RESTORE, 'context_switch', *FORBIDDEN)
        missing = set(names) - symbols.keys()
        if missing:
            raise ValueError('missing required startup symbol(s): ' + ', '.join(sorted(missing)))
        addresses = {n: symbols[n]['st_value'] for n in names}
        edges = {}
        for name in (*STARTS, RESTORE):
            symbol = symbols[name]
            section = elf.get_section(symbol['st_shndx'])
            offset = symbol['st_value'] - section['sh_addr']
            data = section.data()[offset:offset + symbol['st_size']]
            instructions = list(decoder.disasm(data, symbol['st_value']))
            if sum(i.size for i in instructions) != len(data):
                raise ValueError('incomplete decode: ' + name)
            edges[name] = [i.operands[0].imm & ((1 << 64) - 1) for i in instructions
                           if i.mnemonic in ('call', 'jmp') and i.operands[0].type == X86_OP_IMM]
    check(edges, addresses)
    mutations = []
    if args.mutations:
        for name, target in [*((n, RESTORE) for n in STARTS), (RESTORE, 'context_switch')]:
            changed = {n: list(e) for n, e in edges.items()}
            changed[name] = [addresses[FORBIDDEN[0]] if e == addresses[target] else e for e in changed[name]]
            try:
                check(changed, addresses)
            except ValueError:
                mutations.append({'caller': name, 'redirected_target': target, 'rejected': True})
            else:
                raise ValueError('accepted startup bypass mutation: ' + name)
    result = {'pass': True, 'sha256': hashlib.sha256(args.elf.read_bytes()).hexdigest(),
              'scope': 'direct compiled startup call edges; not full control-flow or register-state proof',
              'checked': [*STARTS, RESTORE], 'mutations': mutations}
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    main()
