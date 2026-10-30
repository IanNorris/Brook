#!/usr/bin/env python3
"""One boot-pinned remote maintenance update with durable receipts.

Existing output directories are never reused. On any error inspect receipt.json
and the SAME hw_job / hw_power request IDs; do not blindly rerun this command.
Activation and restart each require an explicit flag. Restart verification also
requires the payload ID embedded in the new initrd. This supports kernel/initrd
only; it does not write the application image or a whole USB raw image.
"""
import argparse,json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware_update_workflow import UpdateRun
from hardware import Hardware

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session',required=True);p.add_argument('--key-file',type=Path,required=True)
    p.add_argument('--kernel',type=Path,required=True);p.add_argument('--initrd',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--port',type=int,default=1235)
    p.add_argument('--activate',action='store_true');p.add_argument('--restart',action='store_true')
    p.add_argument('--expected-payload-id')
    p.add_argument('--boot-timeout-s',type=int,default=120,help='Observe the same restart for 1..900s; a timeout never retries it')
    a=p.parse_args()
    result=UpdateRun(Hardware(),a.output).execute(a.session,a.key_file,a.kernel,a.initrd,
        a.activate,a.restart,a.expected_payload_id,a.port,a.boot_timeout_s)
    print(json.dumps({'state':result['state'],'receipt':str(a.output.resolve()/'receipt.json'),
        'session':result.get('new_session',a.session),'transaction_id':result['transaction_id']},indent=2))
if __name__=='__main__':main()
