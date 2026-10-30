#!/usr/bin/env python3
"""Boot immutable application RAM snapshots from a mutable private QEMU USB.

Positive: overwrite the entire source and detach its USB namespace before loading a
fresh ELF and checking its files. Negative boots retain network/job recovery.
Only private bounded images, no host devices, desktop or speakers.
"""
import argparse,hashlib,json,os,shutil,socket,subprocess,time,uuid
from pathlib import Path
from test_hardware_power import Qmp,Rpc
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
def run(*cmd,**kw):return subprocess.run(list(map(str,cmd)),check=True,cwd=ROOT,stdin=subprocess.DEVNULL,**kw)
source=out/'source';source.mkdir();(source/'private').mkdir(mode=0o700)
run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/'scripts/workloads/compressed_image_elf.c','-o',source/'hello')
(source/'data.bin').write_bytes(bytes((i*73+(i>>8))&255 for i in range(131089)))
os.link(source/'data.bin',source/'hard');(source/'link').symlink_to('data.bin')
(source/'private/denied').write_text('private');shutil.copy2(source/'hello',source/'private/hello')
(source/'padding').write_bytes(os.urandom(2*1024*1024))
image=out/'APPS.BRK';env=dict(os.environ,BROOK_IMAGE_CHECKSUM_LIBRARY=str(ROOT/'build/release/host_tests/libbrook_image_checksum.so'))
run('python3',ROOT/'scripts/compressed_image.py','build',source,image,env=env)
probe=out/'probe';run('python3',ROOT/'scripts/make_uefi_probe.py','--output',probe,'--driver','xhci','--usb-read-only',
    '--media-mount','/media/usb','--partition-table','mbr','--display-width','800','--display-height','600','--application-image',image)
run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/'scripts/workloads/app_snapshot_guest.c','-o',out/'SNAPCHECK.ELF')
run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/'tools/hardware_status.c','-o',out/'HWSTATUS.ELF')
initrd=out/'template.img';run('cp','--reflink=auto',probe/'payload/INITRD.IMG',initrd)
for name in ['usb-read-only','boot-app-image']:run('mdel','-i',initrd,'::ETC/'+name)
for name in ['SNAPCHECK.ELF','HWSTATUS.ELF']:run('mcopy','-i',initrd,out/name,'::BOOT/'+name)
rc=out/'INIT.RC';rc.write_text('set wm\nset vfb none\nkconsole\nrun --console --uid 0 /boot/HWSTATUS.ELF\n');run('mcopy','-o','-i',initrd,rc,'::BOOT/INIT.RC')
digest=hashlib.sha256(image.read_bytes()).hexdigest();results=[]
for case,ram in [('independent',512),('bad-hash',512),('short-source',512),('low-memory',256)]:
    base=out/case;base.mkdir();disk=base/'private.raw';run('cp','--reflink=auto',probe/'probe.raw',disk)
    candidate=base/'INITRD.IMG';run('cp','--reflink=auto',initrd,candidate)
    marker=base/'boot-app-snapshot';size=image.stat().st_size+(4096 if case=='short-source' else 0)
    marker.write_text(f'/media/usb/APPS.BRK /nix {size} '+('0'*64 if case=='bad-hash' else digest)+'\n')
    run('mcopy','-i',candidate,marker,'::ETC/boot-app-snapshot');run('mcopy','-o','-i',str(disk)+'@@1048576',candidate,'::INITRD.IMG')
    variables=base/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m',str(ram),'-nodefaults',
        '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],'-drive','if=pflash,format=raw,file='+str(variables),
        '-device','VGA','-device','qemu-xhci,id=usbc','-drive','if=none,id=stick,format=raw,file='+str(disk),
        '-device','usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1','-device','usb-net,bus=usbc.0,port=2,netdev=network',
        '-netdev',f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234','-display','none','-serial','file:'+str(base/'serial.log'),
        '-qmp','unix:'+str(base/'qmp.sock')+',server=on,wait=off','-no-reboot']
    (base/'argv.json').write_text(json.dumps(argv,indent=2)+'\n');log=(base/'qemu.log').open('x')
    owner=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT);q=None;rpc=None
    try:
        end=time.monotonic()+90
        while time.monotonic()<end:
            assert owner.poll() is None,'QEMU ended early'
            serial=(base/'serial.log').read_text(errors='replace') if (base/'serial.log').exists() else ''
            assert 'PANIC_HEX:' not in serial and 'Assertion failed' not in serial,serial[-3000:]
            if 'DEBUG_TCP: listening' in serial and 'HARDWARE_NETWORK_READY' in serial:break
            time.sleep(.1)
        else:raise TimeoutError(serial[-3000:])
        if case=='independent':assert 'immutable RAM application snapshot ready' in serial and 'source_closed=1' in serial
        else:assert 'RAM snapshot unavailable; applications disabled' in serial and 'immutable RAM application snapshot ready' not in serial
        q=Qmp(base/'qmp.sock');rpc=Rpc(base);rpc.request('initialize',{'protocolVersion':'2025-06-18'})
        attached=rpc.tool('hw_attach',{'host':'127.0.0.1','port':port});session=attached['session']
        stats=rpc.tool('hw_stats',{'session':session});(base/'stats.json').write_text(json.dumps(stats,indent=2)+'\n')
        ident=uuid.uuid4().hex;arguments=['/boot/SNAPCHECK.ELF']+([] if case=='independent' else ['recovery'])
        job=rpc.tool('hw_guest_run',{'session':session,'request_id':ident,'argv':arguments,'cwd':'/tmp','uid':0,'wait_s':2})
        end=time.monotonic()+60
        while job.get('state')!='completed':
            assert job.get('state') not in ['failed','rejected','lost'] and time.monotonic()<end,job
            job=rpc.tool('hw_job',{'session':session,'operation_id':ident,'wait_s':2})
        (base/'job.json').write_text(json.dumps(job,indent=2)+'\n');assert job.get('exit',{}).get('code')==0,job
        text=job.get('stdout',{}).get('text','')
        if case=='independent':assert 'APP_SNAPSHOT_SOURCE_OVERWRITTEN_NAMESPACE_DETACHED' in text and 'COMPRESSED_IMAGE_ELF_PASS' in text,job
        else:assert 'APP_SNAPSHOT_RECOVERY_OK' in text,job
        shutdown=uuid.uuid4().hex;state=rpc.tool('hw_power',{'session':session,'request_id':shutdown,'operation':'shutdown'});end=time.monotonic()+30
        while state['state']!='ready':
            assert state['state']!='failed' and time.monotonic()<end,state
            state=rpc.tool('hw_power',{'session':session,'request_id':shutdown,'operation':'status'});time.sleep(.1)
        (base/'shutdown.json').write_text(json.dumps(state,indent=2)+'\n');q.call('quit');assert owner.wait(timeout=15)==0
        results.append({'case':case,'passed':True,'ram_mib':ram,'post_mount_free_bytes':stats['free_memory_bytes'],'snapshot_bytes':image.stat().st_size if case=='independent' else 0})
    finally:
        if rpc:rpc.close()
        if q:q.close()
        if owner.poll() is None:owner.terminate();owner.wait(timeout=10)
        log.close()
(out/'result.json').write_text(json.dumps({'passed':True,'scope':'private QEMU USB image','cases':results},indent=2)+'\n')
print('APP_SNAPSHOT_QEMU_PASS')
