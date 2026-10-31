#!/usr/bin/env python3
"""Run one ext2 phase in an already-running *private QEMU* AHCI test VM."""
import argparse
import json
from pathlib import Path
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ready',required=True,type=Path)
    parser.add_argument('--phase',required=True,choices=['create','verify'])
    args=parser.parse_args();ready=json.loads(args.ready.read_text())
    hardware=Hardware(ready['registry']);session=ready['session']
    assert hardware.load_session(session)['host']=='127.0.0.1', 'private QEMU only'
    status=hardware.command(session,'bridge hardware ahci-ro')
    assert status['ready'] and status['reason']=='disposable QEMU test ATA device writable',status
    request=uuid.uuid4().hex
    hardware.guest_run(session,request,['/boot/AHCIQA.ELF',args.phase],cwd='/tmp',uid=0,wait_s=2)
    deadline=time.monotonic()+120
    while time.monotonic()<deadline:
        result=hardware.job(session,request,wait_s=2)
        if result['state']=='completed':break
        assert result['state'] not in ('failed','rejected','lost'),result
    else:raise TimeoutError('Inspect existing guest job '+request)
    (args.ready.parent/('ext2-'+args.phase+'.json')).write_text(json.dumps(result,indent=2)+'\n')
    assert result.get('exit',{}).get('code')==0,result
    marker='AHCI_EXT2_'+args.phase.upper()+'_OK'
    assert marker in result['stdout']['text'],result
    print(marker,flush=True)


if __name__=='__main__':main()
