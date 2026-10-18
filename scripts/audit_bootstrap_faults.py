#!/usr/bin/env python3
"""Reject unexpected visible faults; identify narrowly proven user-copy probes.

Serial is best-effort evidence, not a lossless event stream. Only recovered
main-process REP probes are recognised; other/partial records fail closed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def probe_symbols(elf, load_bias=0x200000):
    symbols = {}
    for line in subprocess.check_output(['nm', '-n', str(elf)], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16) + load_bias
    handlers = [value for name, value in symbols.items() if name.startswith('_ZL7recover')]
    if len(handlers) != 1:
        raise ValueError('Expected one test_user_copy recovery handler')
    return {
        'handler': handlers[0],
        'from_instruction': symbols['BrookCopyFromUserInstruction'],
        'from_fixup': symbols['BrookCopyFromUserFixup'],
        'to_instruction': symbols['BrookCopyToUserInstruction'],
        'to_fixup': symbols['BrookCopyToUserFixup'],
    }


def audit_bytes(data, symbols, passed_user_copy_tests=0):
    starts = list(re.finditer(rb'\[FAULT\] User fault', data))
    records = []
    for index, match in enumerate(starts):
        block = data[match.start():starts[index + 1].start() if index + 1 < len(starts) else len(data)]
        next_exec = re.search(rb"sys_execve:[^\r\n]*path='", block)
        if next_exec:
            block = block[:next_exec.start()]
        executions = list(re.finditer(rb"sys_execve:[^\r\n]*path='([^'\r\n]+)'", data[:match.start()]))
        if not executions or not executions[-1][1].endswith(b'/host_tests/test_user_copy'):
            raise ValueError(f'Fault outside the known user-copy test at byte {match.start()}')
        def field(pattern):
            found = re.search(pattern, block)
            if not found:
                raise ValueError(f'Incomplete fault evidence at byte {match.start()}')
            return found
        vector = int(field(rb'User fault vec=(0x[0-9a-fA-F]+)')[1], 16)
        rip = int(field(rb'(?m)^  RIP (0x[0-9a-fA-F]+)')[1], 16)
        error = int(field(rb'(?m)^  ERR (0x[0-9a-fA-F]+)')[1], 16)
        if vector != 14:
            raise ValueError('Only deliberate page-fault probes are recognised')
        if rip == symbols['from_instruction'] and error == 4:
            fixup, direction = symbols['from_fixup'], 'from'
        elif rip == symbols['to_instruction'] and error in (6, 7):
            fixup, direction = symbols['to_fixup'], 'to'
        else:
            raise ValueError(f'Unexpected fault instruction/error: {rip:#x}/{error:#x}')
        delivered = field(rb'\[SIG\] Delivering signal (\d+) to pid (\d+) \(handler=(0x[0-9a-f]+) faultAddr=(0x[0-9a-f]+) RIP=(0x[0-9a-f]+)\)')
        signal, pid = int(delivered[1]), int(delivered[2])
        if signal != 11 or int(delivered[3], 16) != symbols['handler'] or int(delivered[5], 16) != rip:
            raise ValueError('Fault is not delivered to the pinned recovery handler')
        source = int(field(rb'(?m)^  RSI (0x[0-9a-fA-F]+)')[1], 16)
        target = int(field(rb'(?m)^  RDI (0x[0-9a-fA-F]+)')[1], 16)
        if int(delivered[4], 16) != (source if direction == 'from' else target):
            raise ValueError('Fault address is not the intentional untrusted operand')
        returned = re.search(rb'SIGRETURN: pid ' + str(pid).encode() + rb' rip=(0x[0-9a-f]+)', block[delivered.end():])
        recovery = 'serial_exact_fixup'
        if not returned:
            # ProcessCheckSignals deliberately logs restore registers only for
            # RAX <= 512. The final R11-edit probe retains a large RAX sentinel;
            # require that exact case plus a separately completed CTest receipt.
            rax = int(field(rb'(?m)^  RAX (0x[0-9a-fA-F]+)')[1], 16)
            r14 = int(field(rb'(?m)^  R14 (0x[0-9a-fA-F]+)')[1], 16)
            if not (direction == 'from' and rax == 0x1122334455667788 and
                    r14 == 0x8877665544332211 and passed_user_copy_tests > 0):
                raise ValueError('No matching recovery to the exact REP fixup')
            recovery = 'completed_test_assertions; restore_log_suppressed_for_large_rax'
        elif int(returned[1], 16) != fixup:
            raise ValueError('Recovery does not target the exact REP fixup')
        records.append({'offset': match.start(), 'pid': pid, 'direction': direction,
                        'instruction': hex(rip), 'fixup': hex(fixup), 'recovery_evidence': recovery})
    return {'visible_user_faults': len(starts), 'recognised_recovered_probes': records,
            'scope': 'Complete visible fault records only; serial interleaving/loss can hide events. Test exit/lineage/EOF, repeated suites, kernel errors and filesystem gates remain separate.'}


def completed_test_passes(command):
    status = command['status']
    guest = status['guest']
    if (status['state'] != 'completed' or status.get('effect') != 'confirmed' or
            status['exit']['code'] != 0 or status['exit']['signal'] is not None or
            guest['live_tasks'] != 0 or not guest['leader_exited'] or
            not guest['exit_collected'] or not guest['stdout_eof'] or not guest['stderr_eof']):
        raise ValueError('Incomplete or failed native-test command')
    for stream in ('stdout', 'stderr'):
        output = status[stream]
        if (not output['eof'] or output.get('gaps') or output.get('dropped_bytes') or
                output.get('read_error') or output.get('read_timed_out')):
            raise ValueError('Native-test output is incomplete')
    passes = len(re.findall(r'Test\s+#\d+: test_user_copy\s+\.+\s+Passed', command['stdout']))
    if not passes:
        raise ValueError('No passing user-copy test in the completed receipt')
    return passes


def audit_files(test_elf, pairs):
    symbols = probe_symbols(test_elf)
    results = {}
    for path, receipt in pairs:
        command = json.loads(receipt.read_text())
        passes = completed_test_passes(command)
        result = audit_bytes(path.read_bytes(), symbols, passes)
        result.update(completed_user_copy_tests=passes, command_evidence=str(receipt),
                      command_sha256=hashlib.sha256(receipt.read_bytes()).hexdigest(),
                      serial_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        results[str(path)] = result
    return {'test_elf': str(test_elf),
            'test_elf_sha256': hashlib.sha256(test_elf.read_bytes()).hexdigest(),
            'symbols': symbols, 'serial': results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', type=Path, action='append', required=True)
    parser.add_argument('--test-elf', type=Path, required=True)
    parser.add_argument('--command-evidence', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if len(args.serial) != len(args.command_evidence):
        parser.error('Each serial requires its matching completed command receipt')
    report = audit_files(args.test_elf, zip(args.serial, args.command_evidence))
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({path: value['visible_user_faults'] for path, value in report['serial'].items()}))


if __name__ == '__main__':
    main()
