#!/usr/bin/env python3
"""Known observations and adversarial ambiguity cases for the offline analyser."""
from copy import deepcopy
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parent/'workloads'))
from analyze_sched_lock_trace import analyze, resolve_callers


def empty():
    result = dict(consistent=True, in_window=False, epoch=0, serial=0, ticket=0,
                  phase=0, caller='0x0', flags=0)
    for key in ['reservedNs', 'acquiredNs', 'releaseBeforeNs', 'releaseAfterNs',
                'reservedSteal', 'acquiredSteal', 'releaseBeforeSteal', 'releaseAfterSteal']:
        result[key] = 0
    return result


def event(ticket=10, serial=1, times=(1, 2, 10, 14)):
    result = empty()
    result.update(in_window=True, epoch=2, serial=serial, ticket=ticket, phase=4,
                  caller='0x1000', flags=31)
    for key, value in zip(['reservedNs', 'acquiredNs', 'releaseBeforeNs', 'releaseAfterNs'], times):
        result[key] = value
    return result


def snapshot(*events):
    return dict(lock='ready', epoch=2, enabled=False, end_state=2, epoch_exhausted=False,
                window_unchanged=True, next_ticket=99, serving_before=99, serving_after=99,
                active_cpu_mask=0, cpus=[dict(cpu=i, completed=1, lost=0, clock_errors=0,
                    current=deepcopy(e), max_wait=empty(), max_hold=empty()) for i, e in enumerate(events)])


class LockTraceAnalysisTests(unittest.TestCase):
    def test_known_intervals_and_overlapping_release_bracket(self):
        result = analyze(snapshot(event(), event(ticket=11, times=(7, 12, 18, 19))))
        self.assertEqual(result['events'][0]['wait_observation_ns'], 1)
        self.assertEqual(result['events'][0]['hold_observation_ns'], 8)
        handoff = result['candidate_handoffs'][0]
        self.assertFalse(handoff['multiple_observed_candidates'])
        self.assertEqual(handoff['conditional_bounds'][0]['unlock_to_acquired_observation_ns_lower'], 0)
        self.assertEqual(handoff['conditional_bounds'][0]['unlock_to_acquired_observation_ns_upper'], 2)
        self.assertIn('not a journal', result['scope'])

    def test_positive_conditional_bounds_and_missing_predecessor(self):
        result = analyze(snapshot(event(), event(ticket=11, times=(7, 20, 25, 26))))
        bound = result['candidate_handoffs'][0]['conditional_bounds'][0]
        self.assertEqual((bound['unlock_to_acquired_observation_ns_lower'], bound['unlock_to_acquired_observation_ns_upper']), (6, 10))
        self.assertEqual(analyze(snapshot(event(ticket=12)))['candidate_handoffs'], [])

    def test_duplicate_slots_and_partial_to_complete_merge(self):
        whole = event()
        partial = deepcopy(whole)
        partial.update(phase=2, flags=19, releaseBeforeNs=0, releaseAfterNs=0)
        capture = snapshot(partial)
        capture['cpus'][0].update(max_wait=whole, max_hold=deepcopy(whole))
        result = analyze(capture)
        self.assertEqual(len(result['events']), 1)
        self.assertEqual(result['events'][0]['phase'], 4)
        self.assertEqual(len(result['events'][0]['slots']), 3)
        capture['cpus'][0]['max_hold']['ticket'] += 1
        with self.assertRaisesRegex(ValueError, 'conflicting'):
            analyze(capture)

    def test_ticket_wrap_and_multiple_observed_candidates_remain_ambiguous(self):
        result = analyze(snapshot(event(ticket=(1 << 32)-1),
                                  event(ticket=(1 << 32)-1, times=(15, 16, 20, 21)),
                                  event(ticket=0, times=(22, 30, 35, 36))))
        handoff = result['candidate_handoffs'][0]
        self.assertTrue(handoff['multiple_observed_candidates'])
        self.assertEqual(handoff['observed_candidate_count'], 2)
        self.assertIn('full uint32 ticket cycles', result['scope'])

    def test_future_predecessor_and_other_windows_are_not_matched(self):
        capture = snapshot(event(times=(50, 60, 70, 80)), event(ticket=11))
        self.assertEqual(analyze(capture)['candidate_handoffs'], [])
        capture['cpus'][0]['current'].update(epoch=4, in_window=False)
        result = analyze(capture)
        self.assertEqual(result['quality']['old_window_rows'], 1)
        self.assertEqual(len(result['events']), 1)

    def test_missing_reversed_clocks_and_lagged_steal_are_not_cpu_time(self):
        missing = event()
        missing['flags'] &= ~4
        self.assertIsNone(analyze(snapshot(missing))['events'][0]['hold_observation_ns'])
        reversed_clock = event(times=(30, 20, 40, 50))
        result = analyze(snapshot(reversed_clock))
        self.assertIsNone(result['events'][0]['wait_observation_ns'])
        self.assertIsNone(result['events'][0]['hold_observation_ns'])
        lagged = event(times=(900, 1000, 1010, 1011))
        lagged.update(acquiredSteal=100, releaseBeforeSteal=115, releaseAfterSteal=115)
        result = analyze(snapshot(lagged))['events'][0]
        self.assertEqual(result['hold_reported_steal_ns'], 15)
        self.assertIsNone(result['hold_elapsed_minus_reported_steal_ns'])
        self.assertIn('hold_steal_delta_exceeds_elapsed', result['timing_issues'])

    def test_known_rewind_excludes_handoff_bounds_for_either_record(self):
        for predecessor, successor in [(event(), event(ticket=11, times=(30, 20, 25, 26))),
                                        (event(times=(5, 20, 10, 14)), event(ticket=11, times=(15, 20, 25, 26)))]:
            with self.subTest(predecessor=predecessor, successor=successor):
                result = analyze(snapshot(predecessor, successor))
                self.assertEqual(result['candidate_handoffs'], [])
                self.assertEqual(result['quality']['retained_clock_inconsistent'], 1)

    def test_inconsistent_partial_and_loss_are_visible(self):
        waiting = event()
        waiting.update(phase=1, flags=17, acquiredNs=0, releaseBeforeNs=0, releaseAfterNs=0)
        capture = snapshot(waiting)
        capture['cpus'][0].update(lost=3, clock_errors=2)
        capture['cpus'][0]['max_wait'].update(consistent=False)
        result = analyze(capture)
        self.assertEqual(result['quality']['lifetime_lost'], 3)
        self.assertEqual(result['quality']['lifetime_clock_errors'], 2)
        self.assertEqual(result['quality']['inconsistent_rows'], 1)
        self.assertEqual(result['quality']['partial_rows'], 1)
        self.assertIsNone(result['events'][0]['wait_observation_ns'])

    def test_malformed_identity_schema_and_masks_are_rejected(self):
        changes = [lambda s: s.update(epoch=3), lambda s: s.update(end_state=4),
                   lambda s: s.update(active_cpu_mask=2),
                   lambda s: s['cpus'].append(deepcopy(s['cpus'][0])),
                   lambda s: s['cpus'][0].update(cpu=True),
                   lambda s: s['cpus'][0]['current'].update(flags=63),
                   lambda s: s['cpus'][0]['current'].update(phase=1, flags=31),
                   lambda s: s['cpus'][0]['current'].update(ticket=1 << 32),
                   lambda s: s['cpus'][0]['current'].update(caller=123),
                   lambda s: s['cpus'][0]['current'].update(serial=0),
                   lambda s: s['cpus'][0]['current'].update(consistent=False)]
        for change in changes:
            with self.subTest(change=change):
                capture = snapshot(event())
                change(capture)
                with self.assertRaises(ValueError):
                    analyze(capture)

    def test_matching_served_record_is_only_a_candidate_holder(self):
        acquired = event()
        acquired.update(phase=2, flags=19, releaseBeforeNs=0, releaseAfterNs=0)
        capture = snapshot(acquired)
        capture.update(serving_before=10, serving_after=10, active_cpu_mask=1)
        self.assertEqual(analyze(capture)['candidate_holders'], [[0, 2, 1]])
        capture['serving_after'] = 11
        self.assertEqual(analyze(capture)['candidate_holders'], [])
        capture.update(serving_after=10, window_unchanged=False, end_state=4)
        self.assertEqual(analyze(capture)['candidate_holders'], [])

    def test_symbolization_requires_exact_elf_identity(self):
        name = b'SchedulerYield\0'
        ident = b'\x7fELF\x02\x01\x01' + bytes(9)
        header = struct.pack('<16sHHIQQQIHHHHHH', ident, 2, 62, 1, 0x1000, 64, 0, 0, 64, 56, 1, 0, 0, 0)
        segment = struct.pack('<IIQQQQQQ', 1, 4, 256, 0x1000, 0, len(name), len(name), 4096)
        blob = (header + segment).ljust(256, b'\0') + name
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'kernel.elf'
            path.write_bytes(blob)
            events = analyze(snapshot(event()))['events']
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                resolve_callers(path, '0'*64, events)
            resolve_callers(path, hashlib.sha256(blob).hexdigest(), events)
            self.assertEqual(events[0]['caller_name'], 'SchedulerYield')
            path.write_bytes(b'not ELF')
            with self.assertRaisesRegex(ValueError, 'ELF64'):
                resolve_callers(path, hashlib.sha256(path.read_bytes()).hexdigest(), events)


if __name__ == '__main__':
    unittest.main()
