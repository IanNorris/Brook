#!/usr/bin/env python3
"""Check the compiled first-dispatch call contract, not arbitrary scheduler safety.

Both startup paths must reach context_switch through RestoreFirstProcess and
must not bypass saved continuations by directly entering user mode or a thread
trampoline. --mutations checks rejected redirections of each required edge.
--boot-log requires the four opt-in test-kernel continuation/stack cases.
"""
import argparse
import hashlib
import json
import re
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


def check_boot_log(text, addresses, resume_rip):
    dispatches = re.findall(r'SCHED: CPU(\d+) first dispatch pid=(\d+) saved_rip=(0x[0-9a-f]+)', text)
    selected = {}
    for cpu, pid, rip in dispatches:
        cpu = int(cpu)
        if cpu in selected:
            raise ValueError('duplicate first dispatch for CPU' + str(cpu))
        selected[cpu] = (int(pid), int(rip, 16))
    arrivals = re.findall(r'SCHED_START_TEST PASS cpu=(\d+) kind=([a-z-]+) pid=(\d+) rsp=(0x[0-9a-f]+)', text)
    expected = {1: 'resumed-user', 2: 'resumed-kernel', 3: 'fresh-kernel', 4: 'idle'}
    seen = set()
    for cpu, kind, pid, rsp in arrivals:
        cpu, pid = int(cpu), int(pid)
        if cpu in seen or expected.get(cpu) != kind or cpu not in selected:
            raise ValueError('unexpected or duplicate case arrival')
        seen.add(cpu)
        wanted_rip = (resume_rip if cpu <= 2 else addresses[FORBIDDEN[1]] if cpu == 3
                      else addresses['_ZN5brookL8IdleLoopEv'])
        if selected[cpu] != (pid, wanted_rip) or bool(pid) != (cpu != 4):
            raise ValueError('case did not restore the required selected continuation')
    if seen != expected.keys():
        raise ValueError('missing first-dispatch case(s): ' + str(sorted(expected.keys() - seen)))
    return {'cases': expected, 'first_dispatches': len(selected), 'checked_arrivals': len(arrivals)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--boot-log', type=Path, help='Require all four controlled test-kernel cases')
    args = parser.parse_args()
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    with args.elf.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s for s in elf.get_section_by_name('.symtab').iter_symbols()}
        names = (*STARTS, RESTORE, 'context_switch', *FORBIDDEN)
        if args.boot_log:
            names += ('_ZN5brookL8IdleLoopEv',)
        missing = set(names) - symbols.keys()
        if missing:
            raise ValueError('missing required startup symbol(s): ' + ', '.join(sorted(missing)))
        addresses = {n: symbols[n]['st_value'] for n in names}
        edges = {}
        resume_rip = None
        for name in (*STARTS, RESTORE, 'context_switch'):
            symbol = symbols[name]
            section = elf.get_section(symbol['st_shndx'])
            offset = symbol['st_value'] - section['sh_addr']
            data = section.data()[offset:offset + symbol['st_size']]
            instructions = list(decoder.disasm(data, symbol['st_value']))
            if sum(i.size for i in instructions) != len(data):
                raise ValueError('incomplete decode: ' + name)
            if name == 'context_switch':
                returns = [i.address for i in instructions if i.mnemonic == 'ret']
                if len(returns) != 1:
                    raise ValueError('unmodeled context-switch resume return')
                resume_rip = returns[0]
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
    if args.boot_log:
        boot = args.boot_log.read_text()
        result['boot_log'] = check_boot_log(boot, addresses, resume_rip)
        if args.mutations:
            for cpu in range(1, 5):
                changed = re.sub(r'SCHED_START_TEST PASS cpu=' + str(cpu) + r'[^\n]*', '', boot)
                try:
                    check_boot_log(changed, addresses, resume_rip)
                except ValueError:
                    mutations.append({'missing_case_cpu': cpu, 'rejected': True})
                else:
                    raise ValueError('accepted missing runtime case')
            for cpu in range(1, 5):
                changed = re.sub(r'(SCHED: CPU' + str(cpu) + r' first dispatch pid=\d+ saved_rip=)0x[0-9a-f]+',
                                 r'\g<1>0x0', boot)
                try:
                    check_boot_log(changed, addresses, resume_rip)
                except ValueError:
                    mutations.append({'wrong_continuation_cpu': cpu, 'rejected': True})
                else:
                    raise ValueError('accepted wrong runtime continuation')
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    main()
