#!/usr/bin/env python3
"""Compare two Brook desktop renderer stats snapshots.

The renderer rewrites /tmp/brook-desktop-stats.json about once per second
(and on SIGUSR1). Read it twice over the debug bridge, then run:

    scripts/desktop_stats_diff.py before.json after.json

Rates use the snapshots' monotonic_ns difference. A displayed frame is a
distinct client commit included in a successfully completed present.
Interval percentiles are estimated by linear interpolation inside the
histogram buckets, so they are only as precise as the bucket edges.
"""
import argparse
import json
import sys

DURATIONS = ('count', 'sum_ns', 'min_ns', 'max_ns')


def load(path):
    with open(path, encoding='utf-8') as handle:
        data = json.load(handle)
    if data.get('schema') != 'brook-desktop-stats/1':
        raise SystemExit(f'{path}: unsupported schema {data.get("schema")!r}')
    return data


def delta(after, before, key):
    return after.get(key, 0) - (before or {}).get(key, 0)


def duration_mean_ms(after, before):
    count = delta(after, before, 'count')
    if count <= 0:
        return None, 0
    return delta(after, before, 'sum_ns') / count / 1e6, count


def percentile(hist, edges, max_ms, fraction):
    total = sum(hist)
    if total <= 0:
        return None
    target = fraction * total
    seen = 0
    lower = 0.0
    for index, count in enumerate(hist):
        upper = edges[index] if edges[index] is not None else max(max_ms, lower)
        if count and seen + count >= target:
            return lower + (upper - lower) * (target - seen) / count
        seen += count
        lower = upper
    return lower


def fmt(value, unit='ms'):
    return '-' if value is None else f'{value:.2f}{unit}'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('before')
    parser.add_argument('after')
    args = parser.parse_args()
    before, after = load(args.before), load(args.after)
    restarted = before['pid'] != after['pid'] or after['seq'] < before['seq'] or \
        after['started_ns'] != before['started_ns']
    if restarted:
        print('note: renderer restarted between snapshots; using the second snapshot from its start')
        before = None
        seconds = (after['monotonic_ns'] - after['started_ns']) / 1e9
    else:
        seconds = (after['monotonic_ns'] - before['monotonic_ns']) / 1e9
    if seconds <= 0:
        raise SystemExit('snapshots are not in time order (or identical)')
    edges = after['interval_bucket_le_ms']
    print(f'window: {seconds:.3f}s  seq {"-" if before is None else before["seq"]}->{after["seq"]}  '
          f'mode={after["mode"]} depth={after["depth"]} async_present={after["async_present"]} '
          f'gpu_timestamps={after["gpu_timestamps"]}')

    previous = {}
    if before is not None:
        previous = {(w['window'], w['incarnation']): w for w in before['windows']}
    print('\nper window (displayed = distinct commits at present completion)')
    print(f'{"window":>6} {"incarn":>8} {"src":>4} {"fps":>7} {"displayed":>9} {"repeat":>7} {"stale":>6} '
          f'{"omit":>6} {"unid":>6} {"mean":>8} {"p50":>8} {"p90":>8} {"p99":>8} {"max*":>8}')
    rows = sorted(after['windows'], key=lambda w: (w['window'], w['incarnation']))
    for window in rows:
        old = previous.get((window['window'], window['incarnation']))
        if old is not None and window['displayed'] < old['displayed']:
            old = None  # evicted and re-added: counters restarted
        displayed = delta(window, old, 'displayed')
        hist = [a - (old['hist'][i] if old else 0) for i, a in enumerate(window['hist'])]
        mean, _ = duration_mean_ms(window['interval'], old['interval'] if old else None)
        max_ms = window['interval']['max_ns'] / 1e6
        cells = [percentile(hist, edges, max_ms, f) for f in (0.5, 0.9, 0.99)]
        print(f'{window["window"]:>6} {window["incarnation"]:>8} {window["source"]:>4} {displayed / seconds:>7.2f} '
              f'{displayed:>9} {delta(window, old, "repeated"):>7} {delta(window, old, "stale"):>6} '
              f'{delta(window, old, "omitted"):>6} {delta(window, old, "unidentified"):>6} '
              f'{fmt(mean):>8} {fmt(cells[0]):>8} {fmt(cells[1]):>8} {fmt(cells[2]):>8} {fmt(max_ms):>8}')
        if any(hist):
            labels = [f'<={e}' if e is not None else f'>{edges[-2]}' for e in edges]
            print('       hist ' + ' '.join(f'{label}:{count}' for label, count in zip(labels, hist) if count))
    print('       (* max is lifetime of the table entry, not the interval between snapshots)')

    p_after, p_before = after['presents'], (before or {}).get('presents')
    print('\nglobal rates (per second)')
    for key in ('completed', 'full', 'partial', 'graphics', 'compute', 'quad', 'failed', 'stale', 'stale_sources', 'lease_gaps', 'busy_retries',
                'prepare_failures', 'direct_layers'):
        value = delta(p_after, p_before, key)
        print(f'  presents.{key:<17} {value / seconds:>9.2f}/s  ({value})')
    for name in ('direct', 'nondirect'):
        c_after = after['classes'][name]
        c_before = before['classes'][name] if before else None
        frames = delta(c_after, c_before, 'frames')
        parts = [f'{name:<9} frames {frames / seconds:7.2f}/s ({frames})']
        for key, label in (('gpu_timestamp', 'gpu'), ('cpu_submit_to_fence_observed', 'submit->fence'),
                           ('cpu_prepare', 'prepare')):
            mean, count = duration_mean_ms(c_after[key], c_before[key] if c_before else None)
            parts.append(f'{label} {fmt(mean)} (n={count})')
        print('  ' + '  '.join(parts))
        work_after = c_after.get('composition')
        if work_after:
            work_before = c_before.get('composition') if c_before else None
            n = delta(work_after, work_before, 'frames')
            if n > 0:
                per = {k: delta(work_after, work_before, k) / n for k in
                       ('layers_drawn', 'draws', 'pixels_shaded', 'damage_pixels')}
                print(f'  {"":<9} per frame: layers {per["layers_drawn"]:.2f}  draws {per["draws"]:.1f}  '
                      f'shaded {per["pixels_shaded"] / 1e6:.3f} Mpx  damage {per["damage_pixels"] / 1e6:.3f} Mpx  '
                      f'(max shaded {work_after["max_pixels_shaded"] / 1e6:.3f} Mpx lifetime)')
    o_after, o_before = after['overlay'], (before or {}).get('overlay')
    captures = delta(o_after, o_before, 'captures')
    mean, _ = duration_mean_ms(o_after['wait'], o_before['wait'] if o_before else None)
    polls = delta(o_after, o_before, 'polls')
    print(f'  overlay captures {captures / seconds:.2f}/s ({captures}) failures {delta(o_after, o_before, "failures")} '
          f'mean_wait {fmt(mean)} polls/capture {polls / captures if captures else 0:.2f} '
          f'max_polls(lifetime) {o_after["max_polls"]}')
    if 'idle' in after:
        i_after, i_before = after['idle'], (before or {}).get('idle')
        print(f'  idle polls skipped {delta(i_after, i_before, "skipped_polls") / seconds:.2f}/s  '
              f'keepalive presents {delta(i_after, i_before, "keepalive_presents")}')
    if 'last_frame' in after:
        last = after['last_frame']
        print(f'  last frame: path={last["path"]} direct={last["direct"]} partial={last["partial"]} '
              f'layers={last["layers_drawn"]} draws={last["draws"]} shaded={last["pixels_shaded"]} '
              f'damage={last["damage_pixels"]} submit->fence={last["cpu_submit_to_fence_ns"] / 1e6:.2f}ms')
    if 'app_present' in after:
        a_after, a_before = after['app_present'], (before or {}).get('app_present')
        def sub(group):
            old = (a_before or {}).get(group, {})
            return {k: v - old.get(k, 0) for k, v in a_after[group].items()}
        actions, dominance = sub('actions'), sub('dominance')
        early = a_after['early_notifies'] - (a_before or {}).get('early_notifies', 0)
        print(f'\napp-driven cadence: mode={a_after["mode"]} focus_query={a_after["focus_query"]} '
              f'defer={a_after["defer_us"] / 1000:.1f}ms dominant_window={a_after["dominant"] or "-"} '
              f'early_frame_notifies {early / seconds:.2f}/s ({early})')
        if 'overlay_capture_skips' in a_after:
            skips = a_after['overlay_capture_skips'] - (a_before or {}).get('overlay_capture_skips', 0)
            print(f'  overlay captures reused (app-driven limit) {skips / seconds:.2f}/s ({skips})')
        if 'pointer_only' in a_after:
            po = a_after['pointer_only'] - (a_before or {}).get('pointer_only', 0)
            pd = a_after['pointer_only_driven'] - (a_before or {}).get('pointer_only_driven', 0)
            print(f'  pointer-only frames submitted {po / seconds:.2f}/s ({po}), of which with a dominant window {pd}')
        print('  actions/s   ' + '  '.join(f'{k} {v / seconds:.2f} ({v})' for k, v in actions.items()))
        total = sum(dominance.values())
        print('  dominance   ' + '  '.join(f'{k} {100 * v / total:.0f}%' for k, v in dominance.items() if v) if total
              else '  dominance   (no frames with damage)')
    if 'direct_scanout' in after:
        d_after, d_before = after['direct_scanout'], (before or {}).get('direct_scanout') or {}
        def d(key):
            return d_after[key] - d_before.get(key, 0)
        evaluated = d('evaluated')
        share = f' ({100 * d("eligible") / evaluated:.0f}% of presented frames)' if evaluated else ''
        print(f'direct scanout: mode={d_after["mode"]} evaluated {evaluated} eligible {d("eligible")}{share} '
              f'scanned_out {d("frames")} enters {d("enters")} exits {d("exits")} last={d_after["last"]}')
        old = d_before.get('fallback', {})
        fallback = {k: v - old.get(k, 0) for k, v in d_after['fallback'].items()}
        if any(fallback.values()):
            print('  fallback    ' + '  '.join(f'{k} {v}' for k, v in sorted(fallback.items(), key=lambda kv: -kv[1]) if v))
    evictions = after['window_evictions'] - (before['window_evictions'] if before else 0)
    print(f'  window table evictions {evictions}  publish_failures {after["publish_failures"]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
