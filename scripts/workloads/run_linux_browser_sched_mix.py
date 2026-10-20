#!/usr/bin/env python3
"""One retained Linux control of the Brook media/input/24-worker fixture.

Uses an already owned Linux reference VM. Refuses to overwrite prior receipts;
failures leave the same guest and child SSH jobs for inspection, never relaunch.
Only guest applications and VM QMP receive input; no host desktop APIs are used.
"""
import argparse
import array
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import linux_workload_reference as linux
from workloads.browser_sched_source import BROWSER_SOURCE
from analyze_browser_sched_trace import analyze


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--review', type=Path, required=True)
    parser.add_argument('--fixture-directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--phase', required=True)
    parser.add_argument('--width', type=int, default=800)
    parser.add_argument('--height', type=int, default=600)
    parser.add_argument('--software-vsync', action='store_true', help='Diagnostic Firefox Wayland vsync workaround; separate cohort')
    parser.add_argument('--benchmark', default='/home/brook/benchmark/scheduler-mix')
    args = parser.parse_args()
    if not re.fullmatch('[a-z0-9-]{1,48}', args.phase):
        parser.error('Use a lowercase alphanumeric phase name')
    if not (640 <= args.width <= 1920 and 480 <= args.height <= 1080):
        parser.error('Window dimensions must fit the controlled display')
    review, http, out = args.review.resolve(), args.fixture_directory.resolve(), args.output.resolve()
    data = linux.load(review)
    assert data['audio_backend'] == 'pipewire-null' and data['hda_use_timer'] is False
    out.mkdir(parents=True, exist_ok=False)
    phase = args.phase
    def save(name, value):
        linux.write_json(out / (name + '.json'), value)
        return value
    def read(name):
        return json.loads((out / (name + '.json')).read_text())
    def guest(argv):
        return subprocess.check_output(linux.ssh_argv(review) + [shlex.join(argv)], timeout=20).decode()
    def clock():
        before = time.monotonic_ns()
        value = int(guest(['python3','-c','import time;print(time.monotonic_ns())']))
        return dict(before_ns=before, after_ns=time.monotonic_ns(), guest_ns=value)
    def rows(kind='state'):
        p = http / (kind + '.jsonl')
        values = [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []
        return [v for v in values if v['state'].get('phase') == phase]
    def wait_for(condition, seconds, message):
        deadline = time.monotonic() + seconds
        while not condition():
            linux.load(review)
            if time.monotonic() > deadline:
                raise TimeoutError(message + '; inspect the same VM/job')
            time.sleep(.2)
    def key(letter):
        intent = save('key-' + letter + '-intent', dict(host_before_ns=time.monotonic_ns(), key=letter))
        with linux.qmp(review) as q:
            q.call('send-key', {'keys':[{'type':'qcode','data':letter.lower()}], 'hold-time':50})
        save('key-' + letter, dict(**intent, host_after_ns=time.monotonic_ns()))
    save('identity', dict(session=str(review), phase=phase, kernel=data['packages']['kernelVersion'],
                          host_load=os.getloadavg(), resources=data, software_vsync=args.software_vsync))
    save('harness', dict(controller_sha256=linux.sha(Path(__file__)),
                         page_sha256=linux.sha(http/'http/index.html'), clip_sha256=linux.sha(http/'http/clip.mp4')))
    benchmark_hash = guest(['sha256sum', args.benchmark]).split()[0]
    save('benchmark', dict(path=args.benchmark, sha256=benchmark_hash,
                           expected_brook_sha256='9186eb43eb1673e6734f571dbe779822ec3b0dd4b6ee3263db008c47049253ec'))
    assert benchmark_hash == read('benchmark')['expected_brook_sha256'], 'Use the identical static Brook workload binary'
    home = '/home/brook/timing-browser-mix-' + phase
    port = json.loads((http/'server.json').read_text())['port']
    source = BROWSER_SOURCE.replace('HOME)', repr(home)+')').replace('URL]', repr('http://10.0.2.2:'+str(port)+'/index.html?phase='+phase)+']')
    source = source.replace("os.execv('/nix/profile/bin/brook-run',['/nix/profile/bin/brook-run','/nix/profile/bin/firefox'", "os.environ.update(XDG_RUNTIME_DIR='/run/user/1000',WAYLAND_DISPLAY='wayland-1',DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus')\nos.execv('/run/current-system/sw/bin/firefox',['firefox'")
    if args.software_vsync:
        source=source.replace("import json\n(profile/", "import json\nprefs['widget.wayland.vsync.enabled']=False\n(profile/")
    source=source.replace("'--no-remote','--profile'",repr('--width')+','+repr(str(args.width))+','+repr('--height')+','+repr(str(args.height))+",'--no-remote','--profile'")
    compile(source, 'linux-browser', 'exec')
    (out/'browser-source.py').write_text(source)
    jobs = []
    def spawn(name, argv):
        command = linux.ssh_argv(review) + [shlex.join(argv)]
        save(name+'-intent', dict(argv=argv, host_before_ns=time.monotonic_ns()))
        with (out/(name+'.stdout')).open('wb') as stdout, (out/(name+'.stderr')).open('wb') as stderr:
            child = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        jobs.append((name, child))
        save(name+'-host', dict(pid=child.pid, process_identity=linux.process_identity(child.pid)))
        return child
    browser = spawn('browser', ['python3','-c',source])
    started = time.monotonic_ns()
    try:
        def ready():
            if browser.poll() is not None:
                raise RuntimeError('Firefox exited; inspect its retained stdout/stderr')
            return any(v['state']['ready'] >= 3 and not v['state']['paused'] and v['state']['mediaTotal'] >= 3 for v in rows())
        wait_for(ready, 150, 'Browser readiness deadline')
        save('startup', dict(host_observed_ns=time.monotonic_ns(), elapsed_host_s=(time.monotonic_ns()-started)/1e9, first=rows()[0], last=rows()[-1]))
        print('LINUX_BROWSER_READY', phase, flush=True)
        key('P')
        wait_for(lambda:any(v['state']['key']=='KeyP' for v in rows('input')), 35, 'No DOM preflight acknowledgment')
        idle_first = rows()[-1]
        wait_for(lambda:rows()[-1]['state']['performanceMs']-idle_first['state']['performanceMs'] >= 10000, 90, 'Idle telemetry deadline')
        save('idle', dict(first=idle_first,last=rows()[-1]))
        save('guest-environment', guest(['sh','-c', 'cat /sys/devices/system/clocksource/clocksource0/current_clocksource; cat /proc/sys/kernel/sched_autogroup_enabled; ps -u 1000 -L -o pid,tid,cls,rtprio,ni,comm; wpctl status']))
        before = save('load-before', clock())
        cpu = spawn('cpu', [args.benchmark,'24','30'])
        wait_for(lambda:'READY workers=' in (out/'cpu.stdout').read_text(), 25, 'CPU READY deadline')
        # The common workload starts one second after READY. Ensure this capture
        # is under load, then verify its interval again against the actual result.
        time.sleep(1.1)
        capture_path = out/'audio.wav'
        save('capture-intent', dict(host_before_ns=time.monotonic_ns()))
        with linux.qmp(review) as q:
            prior = set(re.findall(r'(?:^|\n)\[?(\d+)\]?:',q.hmp('info capture')))
            q.hmp(f'wavcapture "{capture_path}" hda0 44100 16 2')
            indices = set(re.findall(r'(?:^|\n)\[?(\d+)\]?:',q.hmp('info capture'))) - prior
            assert len(indices)==1
            capture_before=time.monotonic()
            try:
                time.sleep(8)
            finally:
                q.hmp('stopcapture '+next(iter(indices)))
            capture_after=time.monotonic()
        with wave.open(str(capture_path)) as wav:
            rate,ch,width,n=wav.getframerate(),wav.getnchannels(),wav.getsampwidth(),wav.getnframes()
            assert width==2
            pcm=array.array('h',wav.readframes(n))
        save('capture',dict(artifact=str(capture_path),sample_rate=rate,channels=ch,frames=n,
                             host_monotonic_interval=[capture_before,capture_after]))
        for letter in 'ABCD':
            key(letter);time.sleep(.7)
        cpu.wait(timeout=100)
        assert cpu.returncode==0
        after=save('load-after',clock())
        wait_for(lambda:len({v['state']['key'] for v in rows('input')}&{'KeyA','KeyB','KeyC','KeyD'})==4,40,'Input receipts deadline')
        save('input-receipts',rows('input'))
        key('E')
        wait_for(lambda:bool(rows('trace')),60,'Buffered trace export deadline')
        save('buffered-trace',rows('trace')[-1]);save('all-states',rows())
        result=json.loads((out/'cpu.stdout').read_text().split('RESULT ',1)[1]);save('cpu-result',result)
        counts=[v['batches'] for v in result['cpu']]
        assert len(counts)==24 and min(counts)>0 and all(v['samples'] and not v['early'] for v in result['periodic'])
        load_start=before['after_ns']+result['start_ns']-before['guest_ns']
        load_end=after['before_ns']+result['end_ns']-after['guest_ns']
        clock_delta=after['guest_ns']-before['guest_ns']
        rms=[];step=rate//100*ch
        for i in range(0,len(pcm),step):
            values=pcm[i:i+step];rms.append(math.sqrt(sum(v*v for v in values)/max(1,len(values))))
        summary=dict(phase=phase,policy='Linux default fair class (EEVDF)',kernel=data['packages']['kernelVersion'],
                      guest_memory_gib=6,vcpus=8,hda_use_timer=False,benchmark_sha256=benchmark_hash,
                      firefox_software_vsync=args.software_vsync,window_width=args.width,window_height=args.height,
                      host_guest_rate_low=clock_delta/(after['after_ns']-before['before_ns']),
                      host_guest_rate_high=clock_delta/(after['before_ns']-before['after_ns']),
                      load_host_start_latest_ns=load_start,load_host_end_earliest_ns=load_end,
                      cpu_batches=sum(counts),cpu_service_ratio=max(counts)/min(counts),
                      periodic_p95_ms=max(v['p95_ns'] for v in result['periodic'])/1e6,
                      periodic_max_ms=max(v['max_ns'] for v in result['periodic'])/1e6,
                      audio_frames=n,audio_host_s=capture_after-capture_before,
                      audio_host_rate_ratio=n/rate/(capture_after-capture_before),
                      audio_capture_inside_load=load_start<=capture_before*1e9<=capture_after*1e9<=load_end,
                      audio_nonzero_10ms_bins=sum(v>=1 for v in rms),audio_10ms_bins=len(rms),
                      scope='first Linux smoke boot, fresh profile, same static CPU binary and generated clip; default Linux autogroup/RT/compositor differences recorded; startup is not cold-boot matched')
        save('summary',summary);save('rms-10ms',rms);save('buffered-analysis',analyze(out))
        print('LINUX_MIX_SUMMARY',json.dumps(summary),flush=True)
        save('screenshot',linux.screenshot(review))
        save('close-intent',dict(host_monotonic_ns=time.monotonic_ns()))
        with linux.qmp(review) as q:
            q.call('send-key',{'keys':[{'type':'qcode','data':'ctrl'},{'type':'qcode','data':'q'}],'hold-time':100})
            time.sleep(.25)
            q.call('send-key',{'keys':[{'type':'qcode','data':'ret'}],'hold-time':100})
        browser.wait(timeout=60)
        assert browser.returncode==0
        save('app-close',dict(exit_code=browser.returncode,host_monotonic_ns=time.monotonic_ns()))
        print('LINUX_MIX_APPS_DONE',phase,flush=True)
    finally:
        save('jobs-final', {name:dict(pid=p.pid,exit_code=p.poll()) for name,p in jobs})
    return 0


if __name__=='__main__':
    raise SystemExit(main())
