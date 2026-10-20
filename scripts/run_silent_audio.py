#!/usr/bin/env python3
"""Own an isolated PipeWire null sink for VM tests, without host audio devices.

Logs/state must live under build/. Stop by creating OUTPUT/stop; only this
helper's child is terminated. A timeout also stops that owned child.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/vm_bridge'))
from common import process_identity, write_json


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--timeout',type=int,default=3600,choices=range(10,14401),metavar='10..14400')
    args=parser.parse_args()
    output=args.output.resolve()
    if not output.is_relative_to(ROOT/'build'):
        parser.error('Put silent audio state/logs under the ignored build directory')
    output.mkdir(parents=True,exist_ok=False)
    runtime=Path(tempfile.mkdtemp(prefix='brook-null-audio-'))
    config=output/'pipewire.conf'
    shutil.copy2(ROOT/'tools/linux-workload-reference/pipewire-null.conf',config)
    env=dict(os.environ,PIPEWIRE_RUNTIME_DIR=str(runtime),XDG_RUNTIME_DIR=str(runtime),
             PIPEWIRE_REMOTE='brook-timing',XDG_CONFIG_HOME=str(runtime/'config'),
             XDG_CACHE_HOME=str(runtime/'cache'),XDG_STATE_HOME=str(runtime/'state'),
             DBUS_SESSION_BUS_ADDRESS='')
    with (output/'daemon.log').open('wb') as log:
        child=subprocess.Popen(['pipewire','-c',str(config)],env=env,stdout=log,stderr=subprocess.STDOUT)
    state=dict(pid=child.pid,process_start=process_identity(child.pid),runtime=str(runtime),
               remote='brook-timing',config=str(config),
               scope='private null sink only, no ALSA or device discovery modules')
    write_json(output/'daemon.json',state)
    try:
        deadline=time.monotonic()+8
        while not (runtime/'brook-timing').exists():
            if child.poll() is not None:
                raise RuntimeError('The owned PipeWire daemon exited; inspect daemon.log')
            if time.monotonic()>deadline:
                raise TimeoutError('The owned PipeWire daemon did not expose its private socket')
            time.sleep(.1)
        objects=json.loads(subprocess.check_output(['pw-dump'],env=env,timeout=5))
        nodes=[v.get('info',{}).get('props',{}) for v in objects if v.get('type')=='PipeWire:Interface:Node']
        if len([v for v in nodes if v.get('node.name')=='brook-null'])!=1:
            raise RuntimeError('Expected one private null sink')
        if any('alsa' in str(v).lower() or v.get('media.class')=='Audio/Source' for v in nodes):
            raise RuntimeError('The isolated graph unexpectedly contains an audio device')
        write_json(output/'graph.json',objects)
        print('SILENT_AUDIO_READY',output/'daemon.json',flush=True)
        deadline=time.monotonic()+args.timeout
        while child.poll() is None and time.monotonic()<deadline and not (output/'stop').exists():
            time.sleep(.2)
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill();child.wait(timeout=5)
        write_json(output/'exit.json',dict(exit_code=child.returncode,terminal=True))
    print('SILENT_AUDIO_STOPPED',child.returncode,flush=True)
    return 0 if child.returncode in (0,-15) else child.returncode


if __name__=='__main__':
    raise SystemExit(main())
