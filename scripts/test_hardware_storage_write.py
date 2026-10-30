#!/usr/bin/env python3
"""Write/flush a provisioned scratch file, reboot, verify, and check private FAT.

Only a private QEMU image is writable. No host devices, desktop or speakers.
Requires a probe from prepare_hardware_storage_test.py and the nix-shell tools.
"""
import argparse, json, os, shutil, socket, subprocess, time, uuid
from pathlib import Path
from test_hardware_power import Qmp, Rpc
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--probe',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();probe=args.probe.resolve(strict=True);out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
meta=json.loads((probe/'manifest.json').read_text());assert meta['usb_read_only'] is False
disk=out/'private.raw';subprocess.run(['cp','--reflink=auto',str(probe/'probe.raw'),str(disk)],check=True)
results=[]
for operation in ['write','verify']:
    base=out/operation;base.mkdir()
    variables=base/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','2048','-nodefaults',
        '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],
        '-drive','if=pflash,format=raw,file='+str(variables),'-device','VGA','-device','qemu-xhci,id=usbc',
        '-drive','if=none,id=stick,format=raw,file='+str(disk),'-device','usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1',
        '-device','usb-net,bus=usbc.0,port=2,netdev=network',
        '-netdev',f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234',
        '-display','none','-serial','file:'+str(base/'serial.log'),'-qmp','unix:'+str(base/'qmp.sock')+',server=on,wait=off','-no-reboot']
    (base/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
    log=(base/'qemu.log').open('x');owner=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT);q=None;rpc=None
    try:
        end=time.monotonic()+90
        while time.monotonic()<end:
            assert owner.poll() is None,'QEMU exited before boot'
            text=(base/'serial.log').read_text(errors='replace') if (base/'serial.log').exists() else ''
            assert 'PANIC_HEX:' not in text,text[-2000:]
            if 'DEBUG_TCP: listening' in text and 'HARDWARE_NETWORK_READY' in text:break
            time.sleep(.1)
        else:raise TimeoutError(text[-2000:])
        assert 'application file ready' not in text and 'physical storage probe is read-only' not in text
        q=Qmp(base/'qmp.sock');rpc=Rpc(base);rpc.request('initialize',{'protocolVersion':'2025-06-18'})
        attached=rpc.tool('hw_attach',{'host':'127.0.0.1','port':port});session=attached['session']
        ident=uuid.uuid4().hex
        result=rpc.tool('hw_guest_run',{'session':session,'request_id':ident,'argv':['/boot/HWIO.ELF',operation,meta['scratch_path'],meta['scratch_nonce']],'cwd':'/tmp','uid':0,'wait_s':2})
        end=time.monotonic()+90
        while result.get('state')!='completed' and time.monotonic()<end:
            assert result.get('state') not in ['failed','rejected','lost'],result
            result=rpc.tool('hw_job',{'session':session,'operation_id':ident,'wait_s':2})
        (base/'job.json').write_text(json.dumps(result,indent=2)+'\n')
        assert result.get('exit',{}).get('code')==0,result
        expected='HWIO_WRITE_FSYNC_OK' if operation=='write' else 'HWIO_VERIFY_OK'
        assert expected in result['stdout']['text'],result
        # Stop only after the guest checks all filesystem/device flushes.
        shutdown_id=uuid.uuid4().hex
        state=rpc.tool('hw_power',{'session':session,'request_id':shutdown_id,'operation':'shutdown'})
        end=time.monotonic()+60
        while state['state']!='ready' and time.monotonic()<end:
            assert state['state']!='failed',state
            state=rpc.tool('hw_power',{'session':session,'request_id':shutdown_id,'operation':'status'});time.sleep(.1)
        assert state['state']=='ready' and state['status']=='Safe to power off manually',state
        (base/'shutdown.json').write_text(json.dumps(state,indent=2)+'\n')
        q.call('quit');assert owner.wait(timeout=15)==0
        results.append({'operation':operation,'boot_id':attached['guest_boot_id'],'passed':True})
    finally:
        if rpc:rpc.close()
        if q:q.close()
        if owner.poll() is None:
            owner.terminate();owner.wait(timeout=10)
        log.close()
assert results[0]['boot_id']!=results[1]['boot_id']
# Inspect only after both QEMU processes have closed the private disk.
from hardware_storage_image_check import verify_image
checks=verify_image(disk,meta)
(out/'fat-check.log').write_text(checks.pop('fat_check_log'))
(out/'result.json').write_text(json.dumps({'passed':True,'scope':'private QEMU USB image','fresh_boot_readback':True,'phases':results,**checks},indent=2)+'\n')
print('HARDWARE_STORAGE_WRITE_REBOOT_VERIFY_PASS')
