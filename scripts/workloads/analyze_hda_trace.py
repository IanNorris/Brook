#!/usr/bin/env python3
"""Correlate QEMU 10.2.2 HDA log events with a local coded-audio capture.

Read-only analysis; source-specific FIFO sizes/corrections are explicit. Wall
clock/monotonic anchors are point observations, not calibrated uncertainty
bounds. PCM-to-host alignment is approximate and invalid across inactive gaps.
"""
import argparse
from bisect import bisect_right
from datetime import datetime
import json
from pathlib import Path
import re
from statistics import pstdev

FIFO_BYTES = 8192
BYTES_PER_SECOND = 44100 * 4
EVENT = re.compile(r'^(\S+) hda_audio_(adjust|overrun|running) st (\w+)(.*)$')


def events(text):
    result=[]
    for line in text.splitlines():
        m=EVENT.fullmatch(line)
        if not m:
            if 'hda_audio_' in line:raise ValueError('Unrecognized HDA trace event: '+line)
            continue
        stamp,kind,stream,tail=m.groups()
        wall=datetime.fromisoformat(stamp.replace('Z','+00:00'))
        if wall.tzinfo is None:raise ValueError('Trace timestamps need an explicit timezone')
        item=dict(wall_s=wall.timestamp(),kind=kind,stream=stream)
        if kind=='adjust':
            p=re.fullmatch(r', pos (-?\d+)',tail)
            if not p:raise ValueError('Invalid adjustment payload')
            pos=int(p[1]);limit=FIFO_BYTES//8
            if abs(pos)<=limit:raise ValueError('Trace adjustment inside the no-correction zone')
            item.update(position=pos,correction_ms=1 if pos>limit else -4 if pos < -2*limit else -1)
        elif kind=='running':
            p=re.fullmatch(r', nr (\d+), run ([01])',tail)
            if not p:raise ValueError('Invalid running payload')
            item.update(number=int(p[1]),running=bool(int(p[2])))
        elif tail:raise ValueError('Invalid overrun payload')
        result.append(item)
    return result


def summary(rows):
    adjustments=[x['correction_ms'] for x in rows if x['kind']=='adjust']
    overruns=sum(x['kind']=='overrun' for x in rows)
    return dict(adjustments=len(adjustments),positive_1ms=adjustments.count(1),
                negative_1ms=adjustments.count(-1),negative_4ms=adjustments.count(-4),
                summed_origin_correction_ms=sum(adjustments),overruns=overruns,
                codec_buffer_discard_seconds=overruns*FIFO_BYTES/BYTES_PER_SECOND,
                origin_correction_limit='Summed corrections are not an accumulated final timer origin: overruns reset the origin.')


def analyze(rows,capture,before,after,decoded):
    # The codec's output node is dac. Never pool input and output events.
    rows=[x for x in rows if x['stream']=='dac']
    offsets=[(x['realtime_ns']-x['monotonic_ns'])/1e9 for x in (before,after)]
    if abs(offsets[1]-offsets[0])>.005:raise ValueError('Wall/monotonic anchors differ by more than 5 ms; alignment rejected')
    offset=sum(offsets)/2
    low,high=capture['host_monotonic_interval']
    if not low<high:raise ValueError('Invalid host capture interval')
    if not before['monotonic_ns']/1e9 <= low < high <= after['monotonic_ns']/1e9:
        raise ValueError('Capture is not bracketed by anchors')
    if capture['sample_rate']!=44100 or capture['channels']!=2 or capture['sample_width']!=2:
        raise ValueError('Expected stereo 44.1-kHz s16 capture')
    pcm_s=capture['frames']/44100
    if abs(pcm_s-(high-low))>.05:
        raise ValueError('Capture duration differs from host interval by over 50 ms; inactive/gapped alignment is unaccepted')
    for row in rows:row['host_s_estimate']=row['wall_s']-offset
    selected=[x for x in rows if low<=x['host_s_estimate']<=high]
    result=dict(whole_trace=summary(rows),capture_window=summary(selected),
        host_interval_s=high-low,pcm_s=pcm_s,wall_monotonic_offset_change_s=offsets[1]-offsets[0],
        alignment='Timestamp point estimates and PCM duration near host duration; not a per-sample clock calibration or proof of continuous audio.')
    result['capture_window']['drop_only_expected_source_seconds_per_pcm_second']=1+result['capture_window']['codec_buffer_discard_seconds']/pcm_s
    if decoded is not None:
        if decoded.get('status')!='ok' or abs(decoded['pcm_s']-pcm_s)>1e-6:raise ValueError('Decoded capture mismatch')
        runs=decoded['symbol_runs']
        if len(runs)<3:raise ValueError('Too few decoded runs')
        if any(b['packet']<=a['packet'] for a,b in zip(runs,runs[1:])):raise ValueError('Non-forward source symbols need separate analysis')
        overruns=sorted(x['host_s_estimate'] for x in rows if x['kind']=='overrun')
        residual=[];corrected=[]
        for r in runs:
            t=r['center_pcm_s'];source=(r['packet']+.5)/10
            count=bisect_right(overruns,low+t)
            residual.append(source-t);corrected.append(source-t-count*FIFO_BYTES/BYTES_PER_SECOND)
        result['coded_source']=dict(observed_source_seconds_per_pcm_second=decoded['approximate_source_seconds_per_pcm_second'],
            pitch_scale=decoded['pitch_scale'],runs=len(runs),
            residual_std_s=pstdev(residual),after_discard_accounting_residual_std_s=pstdev(corrected),
            limitation='Quantized symbol centers, uncertain FIFO/capture alignment and inactive output can affect residuals. Improved fit is correlation with the source-defined discard path, not proof of every audible glitch.')
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace',type=Path);p.add_argument('--capture',type=Path,required=True)
    p.add_argument('--anchor-before',type=Path,required=True);p.add_argument('--anchor-after',type=Path,required=True)
    p.add_argument('--decoded',type=Path);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();read=lambda path:json.loads(path.read_text())
    result=analyze(events(a.trace.read_text()),read(a.capture),read(a.anchor_before),read(a.anchor_after),read(a.decoded) if a.decoded else None)
    result['source_contract']='QEMU v10.2.2 hw/audio/hda-codec.c; 8192-byte output codec FIFO, 44.1-kHz stereo s16. Record the actual QEMU version separately.'
    a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))


if __name__=='__main__':main()
