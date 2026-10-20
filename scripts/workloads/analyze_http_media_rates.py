#!/usr/bin/env python3
"""Bound media rates using server-clock causality, without a 1x page-clock model.

A page sample occurs after its /clock reply was generated and before its /state
POST reaches the same server. Retain both endpoints; HTTP arrival is not the
sample time. This complements, and never relaxes, buffered-trace clock checks.
"""
import argparse
import json
from pathlib import Path


def interval_summary(rows):
    if len(rows)<2:return dict(status='insufficient_samples',samples=len(rows))
    a,z=rows[0],rows[-1];first,last=a['state'],z['state']
    lower=(last['clock']['hostMonotonicNs']-a['hostMonotonicNs'])/1e9
    upper=(z['hostMonotonicNs']-first['clock']['hostMonotonicNs'])/1e9
    if lower<=0 or upper<lower:
        return dict(status='insufficient_temporal_separation',samples=len(rows),host_interval_low_s=lower,host_interval_high_s=upper)
    elapsed=last['mediaTotal']-first['mediaTotal']
    callbacks=last['callbacks']-first['callbacks']
    if elapsed<0 or callbacks<0:raise ValueError('Media counters regressed within one epoch')
    page=(last['performanceMs']-first['performanceMs'])/1000
    return dict(status='ok',samples=len(rows),host_interval_low_s=lower,host_interval_high_s=upper,
        media_s=elapsed,media_host_rate_low=elapsed/upper,media_host_rate_high=elapsed/lower,
        page_s=page,page_host_rate_low=page/upper,page_host_rate_high=page/lower,
        callbacks=callbacks,callback_host_fps_low=callbacks/upper,callback_host_fps_high=callbacks/lower,
        paused_samples=sum(r['state']['paused'] for r in rows),
        unplayable_samples=sum(r['state']['ready']<3 for r in rows),
        playback_rates=sorted(set(r['state']['rate'] for r in rows)),
        ambiguous_media_gaps=last['ambiguousGaps']-first['ambiguousGaps'])


def analyze_rows(summary,rows,epoch):
    rows=[r for r in rows if r['state']['epoch']==epoch]
    rows.sort(key=lambda r:r['state']['sequence'])
    if len({r['state']['sequence'] for r in rows})!=len(rows):raise ValueError('Duplicate page sequence')
    for r in rows:
        if r['state']['clock']['hostMonotonicNs']>r['hostMonotonicNs']:
            raise ValueError('Clock reply generated after state POST: causality violated')
    start,end=summary['load_host_start_latest_ns'],summary['load_host_end_earliest_ns']
    loaded=[r for r in rows if r['state']['clock']['hostMonotonicNs']>=start and r['hostMonotonicNs']<=end]
    return dict(schema=1,phase=summary['phase'],epoch=epoch,whole=interval_summary(rows),loaded=interval_summary(loaded),
        load_interval_assumption='Supplied summary CPU boundaries are used as-is; this analyzer does not certify the controller guest-CPU-clock to host conversion.',
        scope='Server timestamp lower/upper bounds on each sample; whole-run rates need no page/host clock model. No recovery of dropped samples; callbacks are opportunities, not physical presentation. Loaded samples must lie wholly within the supplied CPU interval.')


def analyze(root):
    read=lambda name:json.loads((root/(name+'.json')).read_text())
    return analyze_rows(read('summary'),read('all-states'),read('buffered-trace')['state']['epoch'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase_directory',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args();result=analyze(args.phase_directory)
    payload=json.dumps(result,indent=2)+'\n'
    if args.output:args.output.write_text(payload)
    else:print(payload,end='')
    return 0 if result['loaded']['status']=='ok' else 1


if __name__=='__main__':raise SystemExit(main())
