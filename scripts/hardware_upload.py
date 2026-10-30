#!/usr/bin/env python3
"""Upload a kernel/initrd to an already-started HWUPDATE.ELF maintenance job.

No automatic job launch, activation, retry or reboot. A lost activation reply
requires inspecting the SAME original hw_job transaction before proceeding.
The optional --activate writes inactive slots and the final selection record.
"""
import argparse,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from hardware_update_protocol import upload,read_payload,KERNEL_MAX,INITRD_MAX
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--host',required=True);p.add_argument('--port',type=int,default=1235)
p.add_argument('--key-file',type=Path,required=True);p.add_argument('--boot-id',required=True)
p.add_argument('--transaction-id',required=True);p.add_argument('--kernel',type=Path,required=True)
p.add_argument('--initrd',type=Path,required=True);p.add_argument('--activate',action='store_true')
a=p.parse_args()
print(json.dumps(upload(a.host,a.port,a.key_file.read_text(),a.boot_id,a.transaction_id,
    read_payload(a.kernel,KERNEL_MAX),read_payload(a.initrd,INITRD_MAX),a.activate),indent=2))
