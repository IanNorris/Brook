#!/usr/bin/env python3
"""Binary transfer: metadata/receipts only on stdout, never file bytes/base64."""
import argparse,json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware
from hardware_bulk_workflow import BulkRun
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session',required=True);p.add_argument('--key-file',type=Path,required=True)
    p.add_argument('--action',choices=['put','get','frame','disk'],required=True)
    p.add_argument('--source',type=Path);p.add_argument('--artifact-id');p.add_argument('--request-id')
    p.add_argument('--keep-raw',action='store_true',help='Retain diagnostic BGRA bytes in addition to the PNG (frame only)')
    p.add_argument('--output',type=Path,required=True);p.add_argument('--port',type=int,default=1236)
    p.add_argument('--disk-identity',type=Path,help='JSON with independently observed same-boot disk identity')
    p.add_argument('--max-bytes',type=int,help='Explicit disk export size ceiling')
    a=p.parse_args();identity=json.loads(a.disk_identity.read_text()) if a.disk_identity else None
    r=BulkRun(Hardware(),a.output).execute(a.session,a.key_file,a.action,a.source,a.artifact_id,a.port,a.request_id,a.keep_raw,identity,a.max_bytes)
    print(json.dumps({'state':r['state'],'transaction_id':r['transaction_id'],'receipt':str(a.output.resolve()/'receipt.json'),
        'artifact_path':r.get('artifact_path'),'binary_phase_s':r['binary_phase_s'],'transfer':r['transfer']},indent=2))
if __name__=='__main__':main()
