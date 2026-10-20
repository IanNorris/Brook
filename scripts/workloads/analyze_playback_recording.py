#!/usr/bin/env python3
"""Estimate playback tempo from a local recording and matching local audio.

Requires numpy and ffmpeg. No downloads, playback or media outputs: decoding and
spectral matching stay in memory; only a JSON report is written. The reference
must contain the same performance; different edits/masters limit interpretation.
Use --controls to verify matching against known FFmpeg atempo transforms.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

# Keep host analysis bounded independently of the VM workload's worker count.
os.environ['OPENBLAS_NUM_THREADS']='1'
os.environ['OMP_NUM_THREADS']='1'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recording',type=Path,required=True)
    parser.add_argument('--reference',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--ffmpeg',default='ffmpeg')
    parser.add_argument('--controls',action='store_true')
    args=parser.parse_args()
    import numpy as np
    ffmpeg=shutil.which(args.ffmpeg)
    if not ffmpeg:parser.error('ffmpeg is required')
    for path in (args.recording,args.reference):
        if not path.is_file():parser.error('Missing local media: '+str(path))
    sample_rate,hop,window=11025,256,2048
    edges=np.geomspace(60,5000,33)
    frequencies=np.fft.rfftfreq(window,1/sample_rate)
    bands=[(frequencies>=a)&(frequencies<b) for a,b in zip(edges[:-1],edges[1:])]

    def features(path,*,start=None,tempo=None):
        command=[ffmpeg,'-v','error','-threads','1']
        if start is not None:command+=['-ss',str(start)]
        command+=['-i',str(path),'-vn']
        if tempo is not None:command+=['-t','17','-af','atempo='+str(tempo)]
        command+=['-ac','1','-ar',str(sample_rate),'-f','f32le','-']
        data=np.frombuffer(subprocess.check_output(command,timeout=60),dtype=np.float32)
        if len(data)<window*4:raise ValueError('Too little audio to match')
        frames=np.lib.stride_tricks.sliding_window_view(data,window)[::hop]
        power=np.abs(np.fft.rfft(frames*np.hanning(window),axis=1))**2
        values=np.stack([np.log1p(power[:,mask].mean(axis=1)) for mask in bands],axis=1)
        values-=values.mean(axis=1,keepdims=True)
        values=(values-values.mean(axis=0))/(values.std(axis=0)+1e-6)
        return values

    reference=features(args.reference)
    query=features(args.recording)
    rates=np.arange(.85,1.655,.005)
    cache={}
    sums=np.concatenate([np.zeros((1,reference.shape[1])),np.cumsum(reference,axis=0)],axis=0)
    energy=np.concatenate([[0],np.cumsum((reference**2).sum(axis=1))])

    def match(query,rates):
        rows=[]
        for speed in rates:
            length=int(len(query)*speed)
            if length>=len(reference):raise ValueError('Reference must exceed the scaled query duration')
            positions=np.arange(length)/speed
            template=np.stack([np.interp(positions,np.arange(len(query)),query[:,i]) for i in range(query.shape[1])],axis=1)
            template-=template.mean(axis=0)
            size=1<<int(np.ceil(np.log2(len(reference)+length)))
            if size not in cache:cache[size]=np.fft.rfft(reference,n=size,axis=0)
            correlation=np.fft.irfft(cache[size]*np.conj(np.fft.rfft(template,n=size,axis=0)),n=size,axis=0).sum(axis=1)[:len(reference)-length+1]
            variance=energy[length:]-energy[:-length]-((sums[length:]-sums[:-length])**2).sum(axis=1)/length
            scores=correlation/np.sqrt(np.maximum(variance,1e-9)*(template**2).sum())
            index=int(np.argmax(scores))
            rows.append(dict(speed=float(speed),reference_start_seconds=index*hop/sample_rate,score=float(scores[index])))
        return sorted(rows,key=lambda x:-x['score'])

    full=match(query,rates)
    windows=[]
    width=int(6*sample_rate/hop)
    for seconds in (0,5,10):
        start=int(seconds*sample_rate/hop)
        if start+width>len(query):continue
        values=match(query[start:start+width],rates)
        windows.append(dict(recording_start_seconds=start*hop/sample_rate,top=values[:5],unit_rate=next(x for x in values if abs(x['speed']-1)<.0001)))
    controls=[]
    if args.controls:
        for expected in (1.,1.3):
            known=features(args.reference,start=full[0]['reference_start_seconds'],tempo=expected)
            values=match(known,rates)
            controls.append(dict(expected=expected,observed=values[0],passed=abs(values[0]['speed']-expected)<=.01))
    def sha(path):
        with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
    report=dict(method='32 log spectral bands, gain normalization, normalized sliding FFT correlation with pitch-preserving temporal resampling',
        rate_grid=dict(low=.85,high=1.65,step=.005),sample_rate=sample_rate,hop=hop,window=window,
        recording=str(args.recording.resolve()),recording_sha256=sha(args.recording),reference=str(args.reference.resolve()),reference_sha256=sha(args.reference),
        recording_feature_seconds=len(query)*hop/sample_rate,top=full[:10],unit_rate=next(x for x in full if abs(x['speed']-1)<.0001),windows=windows,controls=controls,
        limits='A similarity estimate, not a calibrated OS clock measurement. Same performance required; edits, mastered tempo differences, stalls and nonuniform rate can affect the match. A 0.005 grid is not an uncertainty bound.')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(top=report['top'][:3],unit_rate=report['unit_rate'],windows=windows,controls=controls),indent=2))
    if controls and not all(x['passed'] for x in controls):raise SystemExit('Known-rate control failed; do not accept the recording estimate')


if __name__=='__main__':main()
