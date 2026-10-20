#!/usr/bin/env python3
"""Offline analysis of bounded ready-lock observations, never a full event journal."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

U64 = (1 << 64) - 1
U32 = (1 << 32) - 1
STAGES = [('reservedNs', 'reservedSteal'), ('acquiredNs', 'acquiredSteal'),
          ('releaseBeforeNs', 'releaseBeforeSteal'), ('releaseAfterNs', 'releaseAfterSteal')]


def uint(value, name, maximum=U64):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f'{name}: expected unsigned integer <= {maximum}')
    return value


def boolean(value, name):
    if type(value) is not bool:
        raise ValueError(f'{name}: expected boolean')
    return value


def identity(record):
    return record['cpu'], record['epoch'], record['serial']


def merge(previous, record):
    """Same event may be read before completion and again in a retained slot."""
    common = ['ticket', 'caller', 'reservedNs', 'reservedSteal']
    stage = min(previous['phase'], record['phase'])
    for wall, steal in STAGES[1:stage]:
        common += [wall, steal]
    mask = ((1 << stage) - 1) | 16
    if any(previous[k] != record[k] for k in common) or (previous['flags'] & mask) != (record['flags'] & mask):
        raise ValueError('conflicting observations for the same CPU/epoch/serial')
    chosen = dict(record if record['phase'] > previous['phase'] else previous)
    chosen['slots'] = sorted(set(previous['slots'] + record['slots']))
    return chosen


def interval(record, first, last):
    a, b = STAGES[first][0], STAGES[last][0]
    mask = (1 << first) | (1 << last)
    if record['phase'] <= last or record['flags'] & mask != mask:
        return None
    if record[b] < record[a]:
        return None
    return record[b] - record[a]


def analyze(snapshot):
    if snapshot.get('lock') != 'ready':
        raise ValueError('only ready-lock trace format is supported')
    epoch = uint(snapshot['epoch'], 'epoch')
    if epoch & 1:
        raise ValueError('window epoch must be even')
    enabled = boolean(snapshot['enabled'], 'enabled')
    exhausted = boolean(snapshot['epoch_exhausted'], 'epoch_exhausted')
    unchanged = boolean(snapshot['window_unchanged'], 'window_unchanged')
    end_state = uint(snapshot['end_state'], 'end_state')
    if unchanged and not exhausted and end_state != (epoch | int(enabled)):
        raise ValueError('unchanged window disagrees with end state')
    for name in ['next_ticket', 'serving_before', 'serving_after']:
        uint(snapshot[name], name, U32)
    active = uint(snapshot['active_cpu_mask'], 'active_cpu_mask')
    cpus = snapshot['cpus']
    if not isinstance(cpus, list) or len(cpus) > 64:
        raise ValueError('cpus must contain at most 64 rows')
    seen = set()
    records = {}
    quality = dict(lifetime_completed=0, lifetime_lost=0, lifetime_clock_errors=0,
                   inconsistent_rows=0, old_window_rows=0, partial_rows=0,
                   window_unchanged=unchanged, epoch_exhausted=exhausted)
    for cpu_row in cpus:
        cpu = uint(cpu_row['cpu'], 'cpu', 63)
        if cpu in seen:
            raise ValueError('duplicate CPU row')
        seen.add(cpu)
        for name, output in [('completed', 'lifetime_completed'), ('lost', 'lifetime_lost'), ('clock_errors', 'lifetime_clock_errors')]:
            quality[output] += uint(cpu_row[name], name)
        for slot in ['current', 'max_wait', 'max_hold']:
            raw = cpu_row[slot]
            consistent = boolean(raw['consistent'], 'consistent')
            in_window = boolean(raw['in_window'], 'in_window')
            if not consistent:
                if in_window:
                    raise ValueError('inconsistent publication cannot be in-window')
                quality['inconsistent_rows'] += 1
                continue
            record = {k: uint(raw[k], k) for k in ['epoch', 'serial', 'ticket', 'phase', 'flags'] +
                      [name for pair in STAGES for name in pair]}
            if record['ticket'] > U32 or record['phase'] > 4 or record['flags'] & ~31:
                raise ValueError('unsupported ticket, phase or sample flag bits')
            if record['flags'] & ~(((1 << record['phase']) - 1) | 16):
                raise ValueError('sample flags refer to an unobserved future phase')
            caller = raw['caller']
            if not isinstance(caller, str) or not re.fullmatch(r'0x[0-9a-fA-F]+', caller):
                raise ValueError('caller must be a hexadecimal string')
            record['caller'] = hex(uint(int(caller, 16), 'caller'))
            if in_window != bool(record['phase'] and record['epoch'] == epoch):
                raise ValueError('in-window marker disagrees with record identity')
            if not record['phase']:
                if any(value for key, value in record.items() if key != 'caller') or record['caller'] != '0x0':
                    raise ValueError('empty publication contains nonempty payload')
                continue
            if not record['epoch'] or record['epoch'] & 1 or not record['serial']:
                raise ValueError('nonempty record has invalid epoch/serial')
            if slot != 'current' and record['phase'] != 4:
                raise ValueError('retained maximum must be a released observation')
            if not in_window:
                quality['old_window_rows'] += 1
                continue
            record.update(cpu=cpu, slots=[slot])
            key = identity(record)
            records[key] = merge(records[key], record) if key in records else record

    mask = sum(1 << cpu for cpu in seen)
    if active & ~mask:
        raise ValueError('active mask refers to an absent CPU')
    events = sorted(records.values(), key=identity)
    for record in events:
        stages = range(record['phase'])
        valid_stages = [i for i in stages if record['flags'] & (1 << i)]
        issues = []
        if len(valid_stages) != record['phase']:
            issues.append('missing_clock_sample')
        for column, label in [(0, 'wall_clock_rewind'), (1, 'steal_counter_rewind')]:
            if column and not record['flags'] & 16:
                continue
            values = [record[STAGES[i][column]] for i in valid_stages]
            if any(after < before for before, after in zip(values, values[1:])):
                issues.append(label)
        inconsistent = any(name.endswith('rewind') for name in issues)
        record['clock_consistent'] = False if inconsistent else None if issues else True
        record['timing_issues'] = issues
        record['wait_observation_ns'] = None if inconsistent else interval(record, 0, 1)
        record['hold_observation_ns'] = None if inconsistent else interval(record, 1, 2)
        record['release_bracket_ns'] = None if inconsistent else interval(record, 2, 3)
        record['hold_reported_steal_ns'] = None
        record['hold_elapsed_minus_reported_steal_ns'] = None
        if record['phase'] != 4:
            quality['partial_rows'] += 1
        if record['flags'] & 16 and record['hold_observation_ns'] is not None:
            delta = record['releaseBeforeSteal'] - record['acquiredSteal']
            if delta >= 0:
                record['hold_reported_steal_ns'] = delta
                if delta <= record['hold_observation_ns']:
                    record['hold_elapsed_minus_reported_steal_ns'] = record['hold_observation_ns'] - delta
                else:
                    record['timing_issues'].append('hold_steal_delta_exceeds_elapsed')

    candidates = []
    for successor in events:
        if successor['phase'] < 2 or successor['clock_consistent'] is not True:
            continue
        predecessors = [p for p in events if p['phase'] == 4 and p['clock_consistent'] is True and p['release_bracket_ns'] is not None
                        and (p['ticket'] + 1) & U32 == successor['ticket']
                        and p['releaseBeforeNs'] <= successor['acquiredNs'] and identity(p) != identity(successor)]
        if not predecessors:
            continue
        item = dict(successor=list(identity(successor)), observed_candidate_count=len(predecessors),
                    multiple_observed_candidates=len(predecessors) != 1, conditional_bounds=[])
        for predecessor in predecessors:
            item['conditional_bounds'].append(dict(predecessor=list(identity(predecessor)),
                unlock_to_acquired_observation_ns_lower=max(0, successor['acquiredNs'] - predecessor['releaseAfterNs']),
                unlock_to_acquired_observation_ns_upper=successor['acquiredNs'] - predecessor['releaseBeforeNs']))
        candidates.append(item)
    holders = [list(identity(r)) for r in events if unchanged and not exhausted and r['phase'] in (2, 3) and
               r['ticket'] == snapshot['serving_before'] == snapshot['serving_after']]
    quality['reported_cpu_ids'] = sorted(seen)
    quality['retained_clock_inconsistent'] = sum(e['clock_consistent'] is False for e in events)
    quality['retained_clock_unknown'] = sum(e['clock_consistent'] is None for e in events)
    return dict(epoch=epoch, enabled=enabled, quality=quality, events=events,
                candidate_holders=holders, candidate_handoffs=candidates,
                scope='Retained observations only, not a journal or atomic CPU snapshot. Ticket-neighbor matches and bounds are conditional: unretained events and full uint32 ticket cycles are not ruled out. Acquisition observations include publication delay; release brackets are not unlock instants. Wall-minus-reported-steal is not exact execution time. No performance or audio acceptance is implied.')


def resolve_callers(path, expected_sha256, events):
    blob = path.read_bytes()
    if hashlib.sha256(blob).hexdigest() != expected_sha256:
        raise ValueError('kernel SHA-256 mismatch; do not symbolize against another build')
    if len(blob) < 64 or blob[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected a little-endian ELF64 kernel')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', blob)
    if header[2] != 62 or header[9] < 56 or header[5] + header[9] * header[10] > len(blob):
        raise ValueError('invalid x86-64 program header table')
    segments = [struct.unpack_from('<IIQQQQQQ', blob, header[5] + i * header[9]) for i in range(header[10])]
    for p in segments:
        if p[0] == 1 and p[2] + p[5] > len(blob):
            raise ValueError('ELF load segment exceeds file')
    for record in events:
        address = int(record['caller'], 16)
        record['caller_name'] = None
        for p in segments:
            if p[0] == 1 and p[3] <= address < p[3] + p[5]:
                offset = p[2] + address - p[3]
                data = blob[offset:min(offset + 256, p[2] + p[5])]
                if b'\0' in data:
                    record['caller_name'] = data.split(b'\0', 1)[0].decode('utf-8', errors='replace')
                break


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--expected-kernel-sha256')
    args = parser.parse_args()
    if bool(args.kernel) != bool(args.expected_kernel_sha256):
        parser.error('--kernel and --expected-kernel-sha256 must be supplied together')
    raw = args.snapshot.read_bytes()
    result = analyze(json.loads(raw))
    result['snapshot_sha256'] = hashlib.sha256(raw).hexdigest()
    if args.kernel:
        resolve_callers(args.kernel, args.expected_kernel_sha256, result['events'])
        result['kernel_sha256'] = args.expected_kernel_sha256
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(output=str(args.output), retained_events=len(result['events']),
                         candidate_handoffs=len(result['candidate_handoffs']), quality=result['quality'])))


if __name__ == '__main__':
    main()
