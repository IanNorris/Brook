#!/usr/bin/env python3
"""Run or resume a fixed Brook browser/audio/input/24-worker scheduler cohort.

Uses an already booted private eight-vCPU/six-GiB review VM, closes Firefox and
shuts down the VM on success. Failures preserve tracked jobs for inspection;
observation timeouts never launch replacement jobs. Audio capture is VM output.
The caller must wait for its owned launcher and verify stopped disks afterward.
"""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from workloads.browser_sched_source import BROWSER_SOURCE

def main():
    from pathlib import Path
    import sys,time,json,uuid,threading,wave,array,math,shutil,os
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import bootstrap_acceptance as b
    import argparse,re
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--review',type=Path,required=True)
    parser.add_argument('--fixture-directory',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--phase',required=True)
    parser.add_argument('--policy',choices=['rr','mlfq','eevdf'],required=True)
    args=parser.parse_args();phase,policy=args.phase,args.policy
    if not re.fullmatch('[a-z0-9-]{1,48}',phase):parser.error('Use a lowercase alphanumeric phase name')
    review=args.review.resolve();metadata=json.loads((review/'candidate.json').read_text())
    assert metadata['private_clones'] and metadata['memory_gib']==6 and metadata['vcpus']==8
    assert metadata['hda_use_timer'] is False,'This cohort requires diagnostic callback HDA mode'
    s=(review/'session.txt').read_text().strip();d=b.server.load_session(s)
    http=args.fixture_directory.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    def save(name,value):b.write(out/(name+'.json'),value);return value
    def read(name):return json.loads((out/(name+'.json')).read_text())
    def has(name):return (out/(name+'.json')).exists()
    def tracked(name,argv):
     folder=out/name;folder.mkdir(exist_ok=True);p=folder/'command-evidence.json'
     if p.exists():argv=json.loads(p.read_text())['request']['argv']
     j=b.TrackedCommand(folder,s,argv)
     if not j.evidence['status']:
      j.evidence['request'].update(uid=1000,cwd='/data/home',env={'HOME':'/data/home'});b.write(j.path,j.evidence);j.poll()
     return j
    def rows(kind='state'):
     p=http/(kind+'.jsonl')
     values=[json.loads(x) for x in p.read_text().splitlines()] if p.exists() else []
     return [x for x in values if x['state'].get('phase')==phase]
    def states():
     values=rows();values.sort(key=lambda x:x['state']['performanceMs']);return values
    def guest_clock():
     t0=time.monotonic_ns();v=b.server.guest(d,'audio stats');return dict(before_ns=t0,after_ns=time.monotonic_ns(),result=v)
    def key(k):
     name='key-'+k
     if not has(name):
      if has(name+'-intent'):raise RuntimeError('Unknown prior input delivery; inspect existing request '+k)
      save(name+'-intent',dict(session=s,key=k,host_before_ns=time.monotonic_ns()))
      result=b.server.input_actions(s,[dict(type='keypress',keys=[k],hold_seconds=.05)])
      save(name,dict(**read(name+'-intent'),host_after_ns=time.monotonic_ns(),result=result))
     return read(name)
    assert b.server.guest(d,'sched policy status')['policy']==policy
    if not has('harness'):save('harness',dict(controller_sha256=b.sha(Path(__file__)),analysis_sha256=b.sha(Path(__file__).with_name('analyze_browser_sched_trace.py')),page_sha256=b.sha(http/'http/index.html'),clip_sha256=b.sha(http/'http/clip.mp4')))
    if not has('identity'):save('identity',dict(session=s,policy=policy,kernel=d['kernel_sha256'],host_load=os.getloadavg(),vm=str(review)))
    else:assert read('identity')['session']==s
    port=json.loads((http/'server.json').read_text())['port'];home='/data/home/timing-browser-mix-'+phase
    source=out/'browser-source.py'
    if not source.exists():
     code=BROWSER_SOURCE
     code=code.replace('HOME)',repr(home)+')').replace('URL]',repr('http://10.0.2.2:'+str(port)+'/index.html?phase='+phase)+']')
     # Capture application logs on the private disk, outside bounded bridge pipes.
     code=code.replace("os.execv('/nix/profile/bin/brook-run'", "log=os.open(str(home/'launcher.log'),os.O_CREAT|os.O_WRONLY|os.O_EXCL,0o600)\nos.dup2(log,1);os.dup2(log,2);os.close(log)\nos.execv('/nix/profile/bin/brook-run'")
     compile(code,'guest-browser-mix','exec');source.write_text(code)
    browser=tracked('browser',['/nix/profile/bin/python3','-c',source.read_text()]);began=time.monotonic()
    if not has('startup'):
     while True:
      browser.poll();assert browser.evidence['status']['state']!='completed','Browser exited; inspect private launcher log'
      available=[r for r in states() if r['state']['ready']>=3 and not r['state']['paused'] and r['state']['mediaTotal']>=3]
      if available:
       w=b.server.windows(s);windows=[x for x in w['windows'] if x.get('app_id')=='firefox']
       if len(windows)==1:
        save('startup',dict(first=available[0],last=available[-1],windows=w,startup_observed_host_s=time.monotonic()-began));break
      assert time.monotonic()-began<150,'Browser readiness deadline; inspect existing job'
      time.sleep(.5)
     print('MIX_BROWSER_READY',phase,flush=True)
    if not has('preflight'):
     key('P');deadline=time.monotonic()+35
     while not any(x['state']['key']=='KeyP' for x in rows('input')):
      assert time.monotonic()<deadline,'No DOM preflight ack; inspect focus before sending more input';time.sleep(.2)
     save('preflight',dict(input=[x for x in rows('input') if x['state']['key']=='KeyP']))
    if not has('idle-begin'):
     b.server.guest(d,'sched timing on');b.server.guest(d,'sched timing reset');save('idle-begin',states()[-1])
    if not has('idle'):
     first=read('idle-begin');deadline=time.monotonic()+90
     while True:
      values=[x for x in states() if x['state']['performanceMs']>=first['state']['performanceMs']]
      if values[-1]['state']['performanceMs']-first['state']['performanceMs']>=10000:break
      assert time.monotonic()<deadline,'Idle telemetry deadline; inspect same page';time.sleep(.3)
     save('idle',dict(first=first,last=values[-1],rows=values,ready_timing=b.server.guest(d,'sched timing status')))
     print('MIX_IDLE_DONE',phase,len(values),flush=True)
    if not has('clock-before'):save('clock-before',b.server.guest(d,'clock status'))
    if not has('load-before'):
     b.server.guest(d,'sched timing reset');save('load-before',guest_clock())
    bench=tracked('cpu',['/boot/BIN/SCHEDMIX','24','30'])
    if not has('load-ready'):
     deadline=time.monotonic()+25
     while 'READY workers=' not in bench.evidence['stdout']:
      bench.poll();assert bench.evidence['status']['state']!='completed';assert time.monotonic()<deadline;time.sleep(.2)
     save('load-ready',dict(host_monotonic_ns=time.monotonic_ns()))
    if not has('capture'):
     if has('capture-intent'):raise RuntimeError('Prior capture delivery unresolved; inspect existing recording before retry')
     save('capture-intent',dict(host_monotonic_ns=time.monotonic_ns()));samples=[];done=threading.Event()
     def observe_audio():
      while not done.is_set():
       try:samples.append(guest_clock())
       except Exception as exc:samples.append(dict(error=str(exc),host_ns=time.monotonic_ns()))
       done.wait(.5)
     observer=threading.Thread(target=observe_audio);observer.start()
     try:save('capture',b.server.audio_capture(s,duration_s=8))
     finally:done.set();observer.join(timeout=10);assert not observer.is_alive();save('audio-progress',samples)
    for k in ('A','B','C','D'):
     key(k);time.sleep(.7)
    deadline=time.monotonic()+100
    while bench.evidence['status'].get('state')!='completed':
     bench.poll();assert time.monotonic()<deadline,'Inspect same CPU job';time.sleep(.3)
    assert bench.evidence['status']['guest']['live_tasks']==0
    if not has('load-after'):save('load-after',dict(clock=guest_clock(),ready_timing=b.server.guest(d,'sched timing status')))
    if not has('input-receipts'):
     deadline=time.monotonic()+40
     while len({x['state']['key'] for x in rows('input')}&{'KeyA','KeyB','KeyC','KeyD'})<4 and time.monotonic()<deadline:time.sleep(.3)
     save('input-receipts',rows('input'))
    if not has('buffered-trace'):
     key('E');deadline=time.monotonic()+60
     while not rows('trace'):
      assert time.monotonic()<deadline,'Trace export pending; inspect existing input receipt';time.sleep(.3)
     save('buffered-trace',rows('trace')[-1])
    result=json.loads(bench.evidence['stdout'].split('RESULT ',1)[1]);save('cpu-result',result)
    a,z=read('load-before'),read('load-after')['clock'];g0=a['result']['guest_clock_ns'];g1=z['result']['guest_clock_ns'];assert g1>g0>0
    clock_low=(g1-g0)/(z['after_ns']-a['before_ns']);clock_high=(g1-g0)/(z['before_ns']-a['after_ns'])
    load_start=a['after_ns']+(result['start_ns']-g0);load_end=z['before_ns']+(result['end_ns']-g1)
    values=states();save('all-states',values)
    loaded=[r for r in values if r['state']['clock']['hostMonotonicNs']>=load_start and r['hostMonotonicNs']<=load_end]
    def media_summary(values):
     if len(values)<2:return dict(samples=len(values),status='insufficient_causal_alignment')
     a,z=values[0],values[-1];x,y=a['state'],z['state'];p=(y['performanceMs']-x['performanceMs'])/1000
     lo=(y['clock']['hostMonotonicNs']-a['hostMonotonicNs'])/1e9;hi=(z['hostMonotonicNs']-x['clock']['hostMonotonicNs'])/1e9
     return dict(samples=len(values),page_s=p,media_s=y['mediaTotal']-x['mediaTotal'],media_per_page_s=(y['mediaTotal']-x['mediaTotal'])/p,presented_callbacks_per_page_s=(y['callbacks']-x['callbacks'])/p,decoded_fps_per_page_s=(y['totalFrames']-x['totalFrames'])/p,dropped_delta=y['droppedFrames']-x['droppedFrames'],callback_host_fps_low=(y['callbacks']-x['callbacks'])/hi if hi>0 else None,callback_host_fps_high=(y['callbacks']-x['callbacks'])/lo if lo>0 else None,host_interval_low_s=lo,host_interval_high_s=hi,ambiguous_media_gaps=y['ambiguousGaps']-x['ambiguousGaps'],audio=x.get('audio') and y.get('audio'))
    inputs=[]
    for k in ('A','B','C','D'):
     request=read('key-'+k);matches=[x for x in read('input-receipts') if x['state']['key']=='Key'+k and x['hostMonotonicNs']>=request['host_before_ns']]
     inputs.append(dict(key=k,dispatch_inside_load=load_start<=request['host_before_ns']<=request['host_after_ns']<=load_end,ack_http_upper_ms=(matches[0]['hostMonotonicNs']-request['host_before_ns'])/1e6 if matches else None,event_to_raf_ms=matches[0]['state']['rafPerfMs']-matches[0]['state']['handledPerfMs'] if matches else None))
    capture=read('capture')
    with wave.open(capture['artifact']) as f:
     rate,ch,width,n=f.getframerate(),f.getnchannels(),f.getsampwidth(),f.getnframes();assert width==2;pcm=array.array('h',f.readframes(n))
    levels=[];size=rate//100*ch
    for i in range(0,len(pcm),size):
     part=pcm[i:i+size];levels.append(math.sqrt(sum(v*v for v in part)/max(1,len(part))))
    longest=current=0
    for v in levels:current=current+1 if v<1 else 0;longest=max(longest,current)
    counts=[x['batches'] for x in result['cpu']];assert len(counts)==24 and min(counts)>0 and all(x['samples']>0 and not x['early'] for x in result['periodic'])
    summary=dict(phase=phase,policy=policy,kernel=d['kernel_sha256'],guest_memory_gib=6,vcpus=8,hda_use_timer=False,scope='headless GPU VM, fresh Firefox profile, generated local video, 24 bounded CPU workers; input ack includes HTTP and DOM RAF opportunity, not physical display latency; load alignment assumes 1x KVM clock checked by reported brackets',host_guest_rate_low=clock_low,host_guest_rate_high=clock_high,load_host_start_latest_ns=load_start,load_host_end_earliest_ns=load_end,idle=media_summary(read('idle')['rows']),loaded=media_summary(loaded),cpu_batches=sum(counts),cpu_service_ratio=max(counts)/min(counts),periodic_p95_ms=max(x['p95_ns'] for x in result['periodic'])/1e6,periodic_max_ms=max(x['max_ns'] for x in result['periodic'])/1e6,inputs=inputs,audio_frames=n,audio_host_s=capture['host_monotonic_interval'][1]-capture['host_monotonic_interval'][0],audio_host_rate_ratio=n/rate/(capture['host_monotonic_interval'][1]-capture['host_monotonic_interval'][0]),audio_longest_silent_10ms_bins=longest)
    save('summary',summary);save('rms-10ms',levels);print('MIX_SUMMARY',json.dumps(summary),flush=True)
    import importlib.util
    spec=importlib.util.spec_from_file_location('trace_analysis',str(Path(__file__).with_name('analyze_browser_sched_trace.py')));analysis=importlib.util.module_from_spec(spec);spec.loader.exec_module(analysis)
    save('buffered-analysis',analysis.analyze(out))
    if not has('screenshot'):
     w=b.server.windows(s);frame=b.server.capture(s,d,boot_id=w['guest_boot_id'])['data'];shutil.copyfile(frame['artifact'],out/'firefox.png');save('screenshot',frame)
    if not has('close'):
     if not has('close-intent'):
      windows=[x for x in b.server.windows(s)['windows'] if x.get('app_id')=='firefox'];assert len(windows)==1
      save('close-intent',dict(reference=windows[0]['reference'],request_id=uuid.uuid4().hex))
     intent=read('close-intent')
     save('close',b.server.window_action(s,intent['reference'],'close',intent['request_id'],wait_for='applied',wait_s=2))
    deadline=time.monotonic()+90
    while browser.evidence['status'].get('state')!='completed':
     browser.poll();assert time.monotonic()<deadline,'Inspect same browser lineage';time.sleep(.3)
    assert browser.evidence['status']['guest']['live_tasks']==0
    save('clock-after',b.server.guest(d,'clock status'))
    save('final-heap',b.server.guest(d,'heap check'));save('final-audio',b.server.guest(d,'audio stats'))
    if not has('shutdown-intent'):save('shutdown-intent',dict(request_id=uuid.uuid4().hex))
    if not has('shutdown'):save('shutdown',b.server.lifecycle_operation(s,'shutdown',read('shutdown-intent')['request_id'],wait_s=10))
    print('MIX_WORKLOAD_DONE',phase,flush=True)

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
