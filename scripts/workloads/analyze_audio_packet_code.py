#!/usr/bin/env python3
"""Decode packet-code-v1 audio; writes JSON, never modifies the input WAV.

Requires NumPy. Packet frequencies are unique and permuted: ascending frequency
is not assumed to mean forward source time. Rates refer to stored PCM duration;
WAV can omit intervals with no delivery, so host timing needs separate receipts.
"""
import argparse
import json
from pathlib import Path
import wave


def decode(pcm,rate,packet_count):
    import numpy as np
    if rate!=44100 or not 1<=packet_count<=450:raise ValueError('Expected 44.1-kHz packet code with 1..450 symbols')
    width=2205;hop=441
    if len(pcm)<width:raise ValueError('Capture too short')
    frames=np.lib.stride_tricks.sliding_window_view(np.asarray(pcm,dtype=float),width)[::hop]
    power=np.abs(np.fft.rfft(frames*np.hanning(width),axis=1))**2
    bins=np.argmax(power[:,1:],axis=1)+1
    center=power[np.arange(len(bins)),bins]
    fraction=center/np.maximum(power.sum(axis=1),1)
    frequencies=np.fft.rfftfreq(width,1/rate)[bins]
    # Sub-bin interpolation reduces sensitivity to small actual pitch errors.
    interior=(bins>0)&(bins<power.shape[1]-1)
    valid_indices=np.where(interior)[0];b=bins[interior]
    left=np.log(np.maximum(power[valid_indices,b-1],1e-12));middle=np.log(np.maximum(center[interior],1e-12));right=np.log(np.maximum(power[valid_indices,b+1],1e-12))
    denominator=left-2*middle+right
    correction=np.divide(.5*(left-right),denominator,out=np.zeros_like(left),where=np.abs(denominator)>1e-12)
    frequencies[interior]+=np.clip(correction,-.5,.5)*rate/width
    candidate=(fraction>.5)&(np.sqrt((frames**2).mean(axis=1))>50)
    if candidate.sum()<3:return dict(status='insufficient_tone_windows',valid_windows=int(candidate.sum()))
    # Fit pitch only to the frequency comb, never to a desired source-time rate.
    scales=np.arange(.98,1.02001,.0001);scores=[]
    for scale in scales:
        code=np.rint((frequencies[candidate]/scale-400)/40)
        residual=np.abs(frequencies[candidate]/scale-(400+40*code))
        scores.append(float(np.exp(-(residual/4)**2)[(code>=0)&(code<463)].sum()))
    scale=float(scales[int(np.argmax(scores))]);code=np.rint((frequencies/scale-400)/40).astype(int)
    residual=np.abs(frequencies/scale-(400+40*code));ids=((code-29)*pow(137,-1,463))%463
    good=candidate&(residual<6)&(code>=0)&(code<463)&(ids<packet_count)
    points=[dict(packet=int(ids[i]),pcm_s=float((i*hop+width/2)/rate),peak_hz=float(frequencies[i]),residual_hz=float(residual[i])) for i in np.where(good)[0]]
    runs=[]
    for point in points:
        if runs and runs[-1]['packet']==point['packet'] and point['pcm_s']-runs[-1]['last_pcm_s']<.1:
            runs[-1]['last_pcm_s']=point['pcm_s'];runs[-1]['windows']+=1
        else:runs.append(dict(packet=point['packet'],first_pcm_s=point['pcm_s'],last_pcm_s=point['pcm_s'],windows=1))
    runs=[r for r in runs if r['windows']>=2]
    for r in runs:r['center_pcm_s']=(r['first_pcm_s']+r['last_pcm_s'])/2
    transitions=[dict(previous=a['packet'],next=b['packet'],pcm_s=b['center_pcm_s']) for a,b in zip(runs,runs[1:])]
    slope=None
    if len(runs)>=3:
        x=np.array([r['center_pcm_s'] for r in runs]);y=np.array([(r['packet']+.5)/10 for r in runs]);x=x-x.mean();slope=float(np.dot(x,y-y.mean())/np.dot(x,x))
    return dict(status='ok' if len(runs)>=3 else 'insufficient_symbol_runs',sample_rate=rate,pcm_s=len(pcm)/rate,
        pitch_scale=scale,fft_window_s=width/rate,fft_hop_s=hop/rate,all_windows=len(frames),decoded_windows=len(points),
        symbol_runs=runs,backwards_or_repeat_transitions=[t for t in transitions if t['next']<=t['previous']],
        unobserved_ids_between_runs=sum(max(0,t['next']-t['previous']-1) for t in transitions),
        approximate_source_seconds_per_pcm_second=slope,
        limits='Approximate symbol timing (100-ms source quantization), not a physical-host playback rate. Unobserved IDs may be skipped source or decoding gaps. Backwards runs may be replay or corruption; inspect coverage/pitch and independent controls. No correctness acceptance is implied.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wav',type=Path);parser.add_argument('--packets',type=int,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    import numpy as np
    with wave.open(str(args.wav)) as f:
        rate,channels,width=f.getframerate(),f.getnchannels(),f.getsampwidth()
        if width!=2 or channels not in (1,2):raise ValueError('Expected mono/stereo s16 PCM')
        pcm=np.frombuffer(f.readframes(f.getnframes()),dtype='<i2').reshape(-1,channels)[:,0]
    result=decode(pcm,rate,args.packets);args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='symbol_runs'}))
    return 0 if result['status']=='ok' else 1


if __name__=='__main__':raise SystemExit(main())
