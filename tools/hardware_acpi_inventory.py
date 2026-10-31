#!/usr/bin/env python3
"""Collect bounded validated ACPI tables from a boot-pinned hardware bridge.

Files are private local diagnostic artifacts. This reads no arbitrary physical
addresses and does not execute AML or access device registers.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/vm_bridge'))
from hardware import Hardware

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',required=True);p.add_argument('--port',type=int,default=1234)
    p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();out=a.output.absolute();out.mkdir(parents=True,exist_ok=False,mode=0o700)
    bridge=Hardware(out/'registry');attached=bridge.attach(a.host,a.port);session=attached['session']
    info=bridge.command(session,'bridge hardware acpi-info')
    if info.get('acpi_api')!=1 or not isinstance(info.get('tables'),int) or not 1<=info['tables']<=256:
        raise ValueError('Unsupported or excessive ACPI inventory')
    result={'guest_boot_id':attached['guest_boot_id'],'tables':[],'unavailable':[]};total=0
    for index in range(info['tables']):
        meta=bridge.command(session,f'bridge hardware acpi {index} 0 0')
        if 'acpi_error' in meta:
            result['unavailable'].append(index);continue
        length=meta.get('length')
        if meta.get('acpi_api')!=1 or meta.get('index')!=index or not isinstance(length,int) or not 36<=length<=1024*1024:
            raise ValueError('Invalid ACPI table metadata')
        total+=length
        if total>16*1024*1024:raise ValueError('ACPI inventory exceeds size bound')
        content=bytearray()
        while len(content)<length:
            offset=len(content);n=min(512,length-offset)
            part=bridge.command(session,f'bridge hardware acpi {index} {offset} {n}')
            if part.get('acpi_api')!=1 or part.get('index')!=index or part.get('offset')!=offset or part.get('length')!=length:
                raise ValueError('ACPI table changed or response mismatched')
            block=bytes.fromhex(part['hex'])
            if len(block)!=n:raise ValueError('Short ACPI response')
            content.extend(block)
        if struct.unpack_from('<I',content,4)[0]!=length or sum(content)&255:
            raise ValueError('ACPI table checksum or length mismatch')
        signature=bytes(content[:4]).decode('ascii',errors='replace')
        safe=''.join(c if c.isascii() and c.isalnum() else '_' for c in signature)
        name=f'{index:03}-{safe}.aml';fd=os.open(out/name,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(fd,'wb') as f:f.write(content)
        result['tables'].append({'index':index,'signature':signature,'bytes':length,'file':name,'sha256':hashlib.sha256(content).hexdigest()})
    path=out/'manifest.json';fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'w') as f:json.dump(result,f,indent=2);f.write('\n')
    print(f"Collected {len(result['tables'])} tables; {len(result['unavailable'])} unavailable. Artifacts: {out}")

if __name__=='__main__':main()
