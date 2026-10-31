#!/usr/bin/env python3
"""Fresh-process boot of an already-provisioned private USB QEMU disk."""
import argparse,json,os,shutil,socket,subprocess,sys,time,uuid
from pathlib import Path
from test_hardware_power import Qmp,Rpc
ROOT=Path(__file__).resolve().parents[1];sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware

def save(path,data):path.write_text(json.dumps(data,indent=2)+'\n')
def wait_job(h,s,ident,timeout=120):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        result=h.job(s,ident,wait_s=2)
        if result['state']=='completed':return result
        assert result['state'] not in ('failed','rejected','lost'),result
    raise TimeoutError('Inspect original job '+ident)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--first-run',required=True,type=Path);parser.add_argument('--desktop',required=True,type=Path);parser.add_argument('--output',required=True,type=Path)
    a=parser.parse_args();first=a.first_run.resolve(strict=True);desktop=a.desktop.resolve(strict=True);out=a.output.absolute();out.mkdir(parents=True,exist_ok=False)
    if not first.is_relative_to((ROOT/'build/hardware').resolve()):raise ValueError('First run must be a private hardware test artifact')
    prior=json.loads((first/'ready.json').read_text());result=json.loads((first/'result.json').read_text())
    assert result.get('checked_shutdown') and result.get('review',{}).get('passed'),result
    old_pid=prior['qemu_pid']
    try:os.kill(old_pid,0)
    except ProcessLookupError:pass
    else:raise RuntimeError('Prior QEMU PID still exists; do not reuse its disk')
    disk=first/'private.raw';assert disk.is_file() and not disk.is_symlink()
    original=json.loads((first/'argv.json').read_text());variables=out/'VARS.fd';shutil.copy2(first/'VARS.fd',variables);variables.chmod(0o600)
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    argv=list(original)
    for i,arg in enumerate(argv):
        if arg.startswith('if=pflash,format=raw,file='):argv[i]='if=pflash,format=raw,file='+str(variables)
        elif arg.startswith('user,id=network,'):argv[i]=f'user,id=network,hostfwd=tcp:127.0.0.1:{port}-:1234'
        elif arg.startswith('file:') and arg.endswith('serial.log'):argv[i]='file:'+str(out/'serial.log')
        elif arg.startswith('unix:') and 'qmp-review.sock' in arg:argv[i]='unix:'+str(out/'qmp-review.sock')+',server=on,wait=off'
        elif arg.startswith('unix:') and 'qmp.sock' in arg:argv[i]='unix:'+str(out/'qmp.sock')+',server=on,wait=off'
    save(out/'argv.json',argv);log=(out/'qemu.log').open('x');owner=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT);q=None;rpc=None
    try:
        end=time.monotonic()+300
        while time.monotonic()<end:
            assert owner.poll() is None,'QEMU exited before boot'
            serial=(out/'serial.log').read_text(errors='replace') if (out/'serial.log').exists() else ''
            assert 'PANIC_HEX:' not in serial,serial[-3000:]
            if 'writable desktop: ready' in serial and 'HARDWARE_NETWORK_READY' in serial and 'DEBUG_TCP: listening' in serial:break
            time.sleep(.2)
        else:raise TimeoutError(serial[-4000:])
        q=Qmp(out/'qmp.sock');rpc=Rpc(out);rpc.request('initialize',{'protocolVersion':'2025-06-18'})
        attach=rpc.tool('hw_attach',{'host':'127.0.0.1','port':port});s=attach['session'];h=Hardware(out/'registry');doctor=h.doctor(s);save(out/'doctor.json',doctor)
        assert doctor['guest']['writable_desktop']['ready'] and not doctor['guest']['app_snapshot']['configured'],doctor
        candidate=json.loads((desktop/'manifest.json').read_text());assert candidate['payload_id'] in serial,serial[-3000:]
        apps=h.apps(s);save(out/'apps.json',apps);titles={x['title'] for x in apps['apps']};assert {'Firefox','Chromium','App Gallery'}<=titles,titles
        token=uuid.uuid4().hex;ident=uuid.uuid4().hex;save(out/'storage-job-request.json',{'request_id':ident,'token':token})
        h.guest_run(s,ident,['/boot/STOREQA.ELF','create',token],cwd='/tmp',uid=1000,wait_s=2);job=wait_job(h,s,ident);save(out/'storage-job.json',job)
        assert job.get('exit',{}).get('code')==0 and 'WRITABLE_DESKTOP_CREATE_PASS' in job['stdout']['text'],job
        ident=uuid.uuid4().hex;state=h.power(s,ident,'shutdown');end=time.monotonic()+120
        while state['state']!='ready':
            assert state['state']!='failed' and time.monotonic()<end,state
            time.sleep(.2);state=h.power(s,ident,'status')
        save(out/'shutdown.json',state);clean=h.command(s,'bridge hardware caps')['writable_desktop'];save(out/'clean.json',clean)
        assert clean['clean_finalized'] and not clean['state_uncertain'] and not clean['final_error'],clean
        q.call('quit');assert owner.wait(timeout=15)==0
        save(out/'result.json',{'passed':True,'scope':'fresh private QEMU process; full provisioned image boot and writable home/mmap/COW','payload_id':candidate['payload_id'],'apps':sorted(titles),'checked_shutdown':True,'clean_finalized':True})
        print('PROVISIONED_DESKTOP_REBOOT_PASS',flush=True)
    finally:
        if rpc:rpc.close()
        if q:q.close()
        if owner.poll() is None:owner.terminate();owner.wait(timeout=15)
        log.close()

if __name__=='__main__':main()
