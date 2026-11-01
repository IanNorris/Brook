#!/usr/bin/env python3
"""Compare RAM-only hardware transfers; retain each transaction receipt."""
import argparse,hashlib,json,os,stat,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware
from hardware_bulk_workflow import BulkRun

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session',required=True);p.add_argument('--key-file',type=Path,required=True)
    p.add_argument('--source',type=Path,required=True,help='Stable random payload, 1 byte to 8 MiB')
    p.add_argument('--output',type=Path,required=True);p.add_argument('--rounds',type=int,default=3)
    p.add_argument('--pace-bytes-per-second',type=int,default=16000000)
    p.add_argument('--port',type=int,default=1236)
    p.add_argument('--driver-snapshot',action='store_true',help='Capture USB driver counters around each transfer (requires bridge network support)')
    a=p.parse_args();source=a.source.resolve(strict=True)
    if not 1<=a.rounds<=5:p.error('rounds must be between 1 and 5')
    if not 1<=a.pace_bytes_per_second<=0x7fffffff:p.error('invalid pacing cap')
    # Freeze a bounded private snapshot so every transaction uses the same
    # bytes even if the caller replaces or grows the original file mid-run.
    fd=os.open(source,os.O_RDONLY|os.O_NONBLOCK|os.O_NOFOLLOW|os.O_CLOEXEC)
    with os.fdopen(fd,'rb') as f:
        before=os.fstat(f.fileno());size=before.st_size
        if not stat.S_ISREG(before.st_mode) or not 1<=size<=8*1024**2:p.error('source must be a regular file between 1 byte and 8 MiB')
        data=f.read(8*1024**2+1);after=os.fstat(f.fileno())
    if len(data)!=size or (before.st_size,before.st_mtime_ns,before.st_ctime_ns)!=(after.st_size,after.st_mtime_ns,after.st_ctime_ns):
        raise RuntimeError('Source changed while taking bounded snapshot')
    a.output.mkdir(parents=True,exist_ok=False,mode=0o700)
    snapshot=a.output/'source.bin';snapshot.write_bytes(data);digest=hashlib.sha256(data).hexdigest()
    h=Hardware();rows=[];session=h.load_session(a.session);boot=session['guest_boot_id']
    health=h.doctor(a.session)
    if not health.get('reachable') or not health.get('identity_matches'):raise RuntimeError('Original boot is not reachable')
    build_id=health.get('guest',{}).get('build_id');rx_depth=None;cursor=0
    for _ in range(8):
        log=h.logs(a.session,cursor,64)
        for line in log.get('lines',[]):
            if 'CDC-ECM network interface ready' in line and 'rx_depth=' in line:
                value=line.split('rx_depth=',1)[1].split()[0]
                if value.isdigit():rx_depth=int(value)
        cursor=log['next_cursor']
        if not log.get('more'):break
    for i in range(a.rounds):
        modes=(('unpaced',None),('paced',a.pace_bytes_per_second))
        if i%2:modes=tuple(reversed(modes))
        for label,pace in modes:
            before_driver=h.command(a.session,'bridge network')['network_driver_snapshot'] if a.driver_snapshot else None
            if a.driver_snapshot and before_driver is None:raise RuntimeError('No network driver snapshot available')
            result=BulkRun(h,a.output/f'{i}-{label}').execute(a.session,a.key_file,'put',snapshot,port=a.port,pace_bytes_per_second=pace)
            after_driver=h.command(a.session,'bridge network')['network_driver_snapshot'] if a.driver_snapshot else None
            transfer=result['transfer']
            if result['guest_boot_id']!=boot:raise RuntimeError('Guest boot changed; inspect receipts')
            if transfer['sha256']!=digest or transfer['bytes']!=size:raise RuntimeError('Source changed or upload digest mismatch; inspect receipts')
            rows.append(dict(round=i,mode=label,requested_pacing_bytes_per_second=pace,
                             bytes=size,sha256=digest,binary_phase_s=result['binary_phase_s'],
                             transaction_id=result['transaction_id'],driver_before=before_driver,driver_after=after_driver))
            print(json.dumps(rows[-1]),flush=True)
    artifact=rows[-1]['transaction_id']
    returned=BulkRun(h,a.output/'download').execute(a.session,a.key_file,'get',artifact_id=artifact,port=a.port)
    if returned['guest_boot_id']!=boot:raise RuntimeError('Guest boot changed; inspect receipts')
    if returned['transfer']['sha256']!=digest or returned['transfer']['bytes']!=size:raise RuntimeError('Download digest mismatch; inspect receipts')
    summary=dict(boot_id=boot,build_id=build_id,rx_depth=rx_depth,uploads=rows,download_seconds=returned['binary_phase_s'],
                 note='Binary-phase wall time includes protocol and hashing; not wire speed. RX depth is null when absent from retained boot logs.')
    (a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('NETWORK_BENCHMARK_PASS',flush=True)
if __name__=='__main__':main()
