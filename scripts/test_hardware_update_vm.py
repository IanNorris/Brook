#!/usr/bin/env python3
"""Actual RAM receiver -> USB slots -> fresh EFI boot, on one private disk.

No host devices, desktop or speakers. Preserves the prepared profile and checks
all non-slot files after both guest filesystems have completed checked sync.
"""
import argparse,json,os,shutil,socket,subprocess,sys,time,uuid
from pathlib import Path
from test_hardware_power import Qmp,Rpc
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from hardware_update_protocol import upload
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--workflow',action='store_true',help='Exercise durable host orchestration and live guest-triggered reset')
a=p.parse_args();probe=a.probe.resolve(strict=True);out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
meta=json.loads((probe/'manifest.json').read_text());disk=out/'private.raw'
subprocess.run(['cp','--reflink=auto',str(probe/'probe.raw'),str(disk)],check=True)
new_initrd=out/'candidate.img';subprocess.run(['cp','--reflink=auto',str(probe/'maintenance.img'),str(new_initrd)],check=True)
identity='maintenance-remote-'+uuid.uuid4().hex;identity_file=out/'hardware-payload-id';identity_file.write_text(identity+'\n')
subprocess.run(['mcopy','-o','-i',str(new_initrd),str(identity_file),'::ETC/hardware-payload-id'],check=True)
key=Path(meta['update_key_file']).read_text();results=[]
def free_port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
for phase in (['upload'] if a.workflow else ['upload','verify']):
    base=out/phase;base.mkdir();variables=base/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
    port=free_port();data_port=free_port()
    argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','2048','-nodefaults',
        '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],
        '-drive','if=pflash,format=raw,file='+str(variables),'-device','VGA','-device','qemu-xhci,id=usbc',
        '-drive','if=none,id=stick,format=raw,file='+str(disk),'-device','usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1',
        '-device','usb-net,bus=usbc.0,port=2,netdev=network',
        '-netdev',f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234,hostfwd=tcp:127.0.0.1:{data_port}-:1235',
        '-display','none','-serial','file:'+str(base/'serial.log'),'-qmp','unix:'+str(base/'qmp.sock')+',server=on,wait=off','-no-reboot']
    if a.workflow:argv=argv[:-1]+['-action','reboot=reset']
    (base/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
    log=(base/'qemu.log').open('x');owner=subprocess.Popen(argv,stdout=log,stderr=subprocess.STDOUT);q=None;rpc=None
    try:
        end=time.monotonic()+90
        while time.monotonic()<end:
            assert owner.poll() is None,'QEMU exited before boot'
            text=(base/'serial.log').read_text(errors='replace') if (base/'serial.log').exists() else ''
            assert 'PANIC_HEX:' not in text,text[-2000:]
            if 'DEBUG_TCP: listening' in text and 'HARDWARE_NETWORK_READY' in text:break
            time.sleep(.1)
        else:raise TimeoutError(text[-2000:])
        expected=meta['maintenance_payload_id'] if phase=='upload' else identity
        assert 'HARDWARE_PAYLOAD_ID ' in text and expected in text,text[-2500:]
        assert 'application file ready' not in text and 'physical storage probe is read-only' not in text
        q=Qmp(base/'qmp.sock');rpc=Rpc(base);rpc.request('initialize',{'protocolVersion':'2025-06-18'})
        attached=rpc.tool('hw_attach',{'host':'127.0.0.1','port':port});session=attached['session'];boot=attached['guest_boot_id']
        if phase=='upload':
            if a.workflow:
                sys.path.insert(0,str(ROOT/'tools/vm_bridge'))
                from hardware import Hardware
                from hardware_update_workflow import UpdateRun
                result=UpdateRun(Hardware(registry=base/'registry'),base/'workflow').execute(
                    session,Path(meta['update_key_file']),probe/'payload/KERNEL/BROOK.ELF',new_initrd,
                    activate=True,restart=True,expected_payload_id=identity,port=data_port)
                assert result['state']=='boot_verified',result
                assert result['guest_boot_id']!=result['new_guest_boot_id'],result
                session=result['new_session'];boot=result['new_guest_boot_id'];expected=identity
            else:
                tx=uuid.uuid4().hex
                started=rpc.tool('hw_guest_run',{'session':session,'request_id':tx,'argv':['/boot/HWUPDATE.ELF',boot,tx],'cwd':'/tmp','uid':0,'wait_s':2})
                (base/'start.json').write_text(json.dumps(started,indent=2)+'\n')
                end=time.monotonic()+30
                while 'HWUPDATE_LISTENING' not in started.get('stdout',{}).get('text',''):
                    assert started.get('state') not in ['failed','rejected','completed','lost'],started
                    assert time.monotonic()<end,started
                    started=rpc.tool('hw_job',{'session':session,'operation_id':tx,'wait_s':2})
                result=upload('127.0.0.1',data_port,key,boot,tx,(probe/'payload/KERNEL/BROOK.ELF').read_bytes(),new_initrd.read_bytes(),True)
                assert result['state']=='activated' and result['generation']==2 and result['slot']==1,result
                (base/'activation.json').write_text(json.dumps(result,indent=2)+'\n')
                end=time.monotonic()+30
                while started.get('state')!='completed':
                    assert started.get('state') not in ['failed','rejected','lost'],started
                    assert time.monotonic()<end,started
                    started=rpc.tool('hw_job',{'session':session,'operation_id':tx,'wait_s':2})
                (base/'job.json').write_text(json.dumps(started,indent=2)+'\n')
                assert started.get('exit',{}).get('code')==0 and 'HWUPDATE_ACTIVATED generation=2' in started['stdout']['text'],started
        shutdown=uuid.uuid4().hex
        state=rpc.tool('hw_power',{'session':session,'request_id':shutdown,'operation':'shutdown'})
        end=time.monotonic()+60
        while state['state']!='ready':
            assert state['state']!='failed' and time.monotonic()<end,state
            state=rpc.tool('hw_power',{'session':session,'request_id':shutdown,'operation':'status'});time.sleep(.1)
        assert state['status']=='Safe to power off manually',state
        (base/'shutdown.json').write_text(json.dumps(state,indent=2)+'\n');q.call('quit');assert owner.wait(timeout=15)==0
        results.append({'phase':phase,'boot_id':boot,'payload_id':expected,'passed':True})
    finally:
        if rpc:rpc.close()
        if q:q.close()
        if owner.poll() is None:owner.terminate();owner.wait(timeout=10)
        log.close()
if not a.workflow:assert results[0]['boot_id']!=results[1]['boot_id']
from hardware_storage_image_check import verify_image
checks=verify_image(disk,meta);(out/'fat-check.log').write_text(checks.pop('fat_check_log'))
(out/'result.json').write_text(json.dumps({'passed':True,'scope':'private QEMU USB image','remote_upload_activation':True,
    'fresh_firmware_boot_of_updated_initrd':True,'live_guarded_reset_workflow':a.workflow,'phases':results,**checks},indent=2)+'\n')
print('HARDWARE_UPDATE_VM_PASS')
