#!/usr/bin/env python3
"""Verify compiled scheduler ticket-wait bodies cannot restore interrupts.

This checks one diagnostic safety property, not general scheduler correctness.
--mutations requires rejection of an inserted STI and a missing wait body.
"""
import argparse
import hashlib
import json
from pathlib import Path
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from elftools.elf.elffile import ELFFile

FORBIDDEN = {'sti', 'popf', 'popfd', 'popfq', 'iret', 'iretd', 'iretq'}


def check(bodies):
    if not bodies:
        raise ValueError('missing compiled SchedLockWaitForTicket body')
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    result = []
    for name, address, data in bodies:
        decoded = list(decoder.disasm(data, address))
        if not decoded or sum(i.size for i in decoded) != len(data):
            raise ValueError('incomplete decode: ' + name)
        for instruction in decoded:
            if instruction.mnemonic in FORBIDDEN:
                raise ValueError(f'{name}: interrupt restore {instruction.mnemonic} at {instruction.address:#x}')
        result.append(dict(name=name, address=hex(address), instructions=len(decoded)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    bodies = []
    with args.elf.open('rb') as stream:
        elf = ELFFile(stream)
        for symbol in elf.get_section_by_name('.symtab').iter_symbols():
            if ('SchedLockWaitForTicket' not in symbol.name or
                    symbol['st_info']['type'] != 'STT_FUNC' or not symbol['st_size']):
                continue
            section = elf.get_section(symbol['st_shndx'])
            start = symbol['st_value'] - section['sh_addr']
            bodies.append((symbol.name, symbol['st_value'],
                           section.data()[start:start + symbol['st_size']]))
    checked = check(bodies)
    mutations = []
    if args.mutations:
        for index, (name, address, data) in enumerate(bodies):
            changed = list(bodies)
            changed[index] = (name, address, b'\xfb' + data)
            try:
                check(changed)
            except ValueError as error:
                assert 'interrupt restore sti' in str(error), str(error)
                mutations.append(dict(body=name, mutation='inserted_sti', rejected=True))
            else:
                raise ValueError('inserted STI mutation was accepted: ' + name)
        try:
            check([])
        except ValueError:
            mutations.append(dict(mutation='missing_body', rejected=True))
        else:
            raise ValueError('missing wait body was accepted')
    result = dict(passed=True, scope='No interrupt restoration in emitted scheduler ticket-wait bodies',
                  kernel_sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest(),
                  bodies=checked, mutations=mutations)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
