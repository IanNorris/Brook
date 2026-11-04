#!/usr/bin/env python3
"""One bounded, read-only probe of a user-specified Brook hardware endpoint."""
import argparse
import ipaddress
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/vm_bridge'))
from guest_transport import exchange


def run_host_command(argv,timeout=4):
    try:
        result=subprocess.run(argv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=timeout,check=False)
        return {'returncode':result.returncode,'stdout':result.stdout.strip(),'stderr':result.stderr.strip()}
    except (OSError,subprocess.TimeoutExpired) as error:
        return {'error':str(error)}


def probe(host,port=1234,timeout=5):
    address=ipaddress.IPv4Address(host)
    if address.is_multicast or address.is_unspecified or address.is_reserved or int(address)==0xffffffff:
        raise ValueError('Expected an explicit unicast IPv4 address')
    if isinstance(port,bool) or not 1<=port<=65535 or not 0<timeout<=30:
        raise ValueError('Invalid port or timeout')
    host=str(address)
    result={'host':host,'port':port,'route':run_host_command(['ip','route','get',host]),
            'ping':run_host_command(['ping','-n','-c','1','-W','2',host]),
            'bridge':{'stage':'not_started','completed_frames':0}}
    result['neighbor']=run_host_command(['ip','neigh','show','to',host])
    bridge=result['bridge']
    def progress(stage,frames):
        bridge['stage']=stage;bridge['completed_frames']=frames
    start=time.monotonic()
    try:
        replies=exchange(host,port,['bridge hardware caps'],timeout,progress=progress)
        caps=replies[0]
        if type(caps.get('hardware_api')) is int and caps['hardware_api']==1 and caps.get('boot_guard') is True:
            bridge['stage']='capabilities_received';bridge['hardware_api']=1
            bridge['boot_id']=caps.get('boot_id')
        else:
            bridge['stage']='invalid_capabilities'
    except (OSError,ValueError,RuntimeError) as error:
        bridge['error']=str(error)
        if isinstance(error,OSError) and error.errno is not None:bridge['errno']=error.errno
    bridge['elapsed_ms']=round((time.monotonic()-start)*1000)
    result['note']='A failed probe cannot distinguish link loss, a guest hang or a panic; a successful TCP handshake does not prove bridge health.'
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host',help='Explicit IPv4 address; no network discovery is performed')
    parser.add_argument('--port',type=int,default=1234)
    parser.add_argument('--timeout',type=float,default=5)
    parser.add_argument('--count',type=int,default=1,help='Number of independent probes (1–60)')
    parser.add_argument('--interval',type=float,default=2,help='Seconds between probes (0–60)')
    args=parser.parse_args()
    if not 1<=args.count<=60 or not 0<=args.interval<=60:parser.error('Invalid count or interval')
    for index in range(args.count):
        if index:time.sleep(args.interval)
        result=probe(args.host,args.port,args.timeout)
        result['sample']=index+1
        print(json.dumps(result,indent=2 if args.count==1 else None),flush=True)


if __name__=='__main__':main()
