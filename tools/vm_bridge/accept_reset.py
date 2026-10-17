#!/usr/bin/env python3
"""DESTRUCTIVE reset acceptance on an empty agent VM with private cloned disks.

Optional --panic verifies forced recovery after a checked flush and controlled
panic. The VM remains running afterward for explicit checked shutdown/fsck.
"""
import argparse
import json
from pathlib import Path
import time

import server


p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--session',required=True)
p.add_argument('--out',required=True,type=Path)
p.add_argument('--panic',action='store_true')
a=p.parse_args();s=a.session;out=a.out.resolve();data=server.load_session(s)
if not out.is_relative_to((server.ROOT/'build').resolve()):raise ValueError('Keep evidence under build/')
if data.get('provenance')!='agent' or not data.get('cloned_disks'):
    raise ValueError('Requires an agent VM with private cloned disks')
if server.windows(s)['windows']:raise ValueError('Close applications before destructive reset acceptance')
out.mkdir(parents=True,exist_ok=True);evidence={}
def save():(out/'reset.json').write_text(json.dumps(evidence,indent=2))
def check_reset(name,force=False):
    start=time.monotonic();wire=server.call_tool('vm_control',{'session':s,'action':'reset','force':force,'wait_s':20})
    r=json.loads(wire['content'][-1]['text']);evidence[name]={'result':r,'wall_s':time.monotonic()-start};save()
    assert r['reset_delivery']=='acknowledged' and r['reboot_policy_restored'],r
    assert r['guest_ready'] and r['vm']['running'],r
    assert r['generation']!=r['previous_generation']
    if force:
        assert not r['guest_sync_completed'] and r['forced'] and r['state']=='running_unverified',r
        assert r['previous_guest_boot_id'] is None and not r['restart_verified']
    else:
        assert r['guest_sync_completed'] and r['state']=='restarted' and r['restart_verified'],r
        assert r['current_guest_boot_id']!=r['previous_guest_boot_id']
    print(name,round(time.monotonic()-start,3),r['state'],flush=True)
    return r
r=check_reset('normal')
server.control(s,'pause')
try:
    status=server.reset_status(s);evidence['paused_status']=status;save()
    assert status['restart_verified'] and status['state']=='restarted'
    assert not status['guest_ready'] and not status['vm']['running']
finally:server.control(s,'resume')
if a.panic:
    with server.session_lock(s) as current:
        sync=server.guest(current,'bridge sync',timeout=30);assert sync.get('guest_sync_completed') is True
        evidence['pre_panic_sync']=sync;save()
    evidence['panic_request']=server.control(s,'panic');save();deadline=time.monotonic()+30
    while not server.panic_state(server.load_session(s)):
        assert time.monotonic()<deadline,'Controlled panic deadline'
        time.sleep(.1)
    old_generation=server.load_session(s)['generation']
    try:server.control(s,'reset')
    except RuntimeError as error:evidence['unforced_refusal']=str(error)
    else:raise AssertionError('Panicked reset without force was accepted')
    assert server.load_session(s)['generation']==old_generation
    check_reset('forced_after_panic',force=True)
wire=server.call_tool('vm_observe',{'session':s,'include':['frame','windows','processes','errors']})
evidence['after']=json.loads(wire['content'][-1]['text']);save()
assert evidence['after']['state']=='completed'
print('ACCEPTED reset delivery / new boot / restored reboot policy / historical paused status / post-reset observations',flush=True)
