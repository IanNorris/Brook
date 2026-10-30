#!/usr/bin/env python3
"""Boot a full offline RAM desktop from private A/B USB slots for semantic QA.

Writes ready.json for the scoped hardware bridge. A reviewer writes finish.json
with passed=true after app checks; the harness then performs checked shutdown.
No physical USB, host desktop, speakers or GPU passthrough. Timeout is bounded.
"""
import argparse,json,os,shutil,socket,subprocess,sys,time,uuid
from pathlib import Path
from test_hardware_power import Qmp,Rpc
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from hardware_update_protocol import record,KERNEL_MAX,INITRD_MAX
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--desktop',required=True,type=Path);p.add_argument('--maintenance',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
p.add_argument('--width',type=int,default=1920);p.add_argument('--height',type=int,default=1080)
a=p.parse_args();desktop=a.desktop.resolve(strict=True);maintenance=a.maintenance.resolve(strict=True);out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
if not (640<=a.width<=4096 and 480<=a.height<=2160):p.error('Private test display must be 640..4096 by 480..2160')
m=json.loads((desktop/'manifest.json').read_text());mm=json.loads((maintenance/'manifest.json').read_text())
assert m['application_image']['sha256']==mm['files']['APPS.BRK']['sha256']
def run(*argv):subprocess.run(list(map(str,argv)),check=True,cwd=ROOT,stdin=subprocess.DEVNULL)
disk=out/'private.raw';run('cp','--reflink=auto',maintenance/'probe.raw',disk)
kernel=(desktop/'kernel.elf').read_bytes();initrd=(desktop/'INITRD.IMG').read_bytes()
for name,capacity,data in [('KERNEL.ELF',KERNEL_MAX,kernel),('INITRD.IMG',INITRD_MAX,initrd)]:
    path=out/name
    with path.open('wb') as f:f.write(data);f.truncate(capacity)
    run('mcopy','-o','-i',str(disk)+'@@1048576',path,'::UPDATES/B/'+name)
selector=out/'SELECT1.BIN';selector.write_bytes(record(2,1,kernel,initrd));run('mcopy','-o','-i',str(disk)+'@@1048576',selector,'::UPDATES/SELECT1.BIN')
# Private test display only. The uploadable initrd remains byte-identical.
config=out/'BROOK.CFG';config.write_text(f'TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nDISPLAY_WIDTH={a.width}\nDISPLAY_HEIGHT={a.height}\nUPDATE_SLOTS=1\n')
run('mcopy','-o','-i',str(disk)+'@@1048576',config,'::BROOK.CFG')
variables=out/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','2048','-nodefaults',
    '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],'-drive','if=pflash,format=raw,file='+str(variables),
    '-device','VGA,vgamem_mb=64','-device','qemu-xhci,id=usbc','-drive','if=none,id=stick,format=raw,file='+str(disk),
    '-device','usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1','-device','usb-net,bus=usbc.0,port=2,netdev=network',
    '-netdev',f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234','-display','none','-serial','file:'+str(out/'serial.log'),
    '-qmp','unix:'+str(out/'qmp.sock')+',server=on,wait=off',
    '-qmp','unix:'+str(out/'qmp-review.sock')+',server=on,wait=off','-no-reboot']
(out/'argv.json').write_text(json.dumps(argv,indent=2)+'\n');log=(out/'qemu.log').open('x');owner=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT);q=None;rpc=None
try:
    end=time.monotonic()+180
    while time.monotonic()<end:
        assert owner.poll() is None,'QEMU exited before desktop boot'
        serial=(out/'serial.log').read_text(errors='replace') if (out/'serial.log').exists() else ''
        assert 'PANIC_HEX:' not in serial,serial[-3000:]
        if 'HARDWARE_NETWORK_READY' in serial and m['payload_id'] in serial and 'DEBUG_TCP: listening' in serial:break
        time.sleep(.1)
    else:raise TimeoutError(serial[-3000:])
    assert 'immutable RAM application snapshot ready' in serial and 'source_closed=1' in serial,serial[-3000:]
    assert 'physical storage probe is read-only' not in serial
    q=Qmp(out/'qmp.sock');rpc=Rpc(out);rpc.request('initialize',{'protocolVersion':'2025-06-18'})
    attached=rpc.tool('hw_attach',{'host':'127.0.0.1','port':port});session=attached['session']
    stats=rpc.tool('hw_stats',{'session':session});(out/'stats.json').write_text(json.dumps(stats,indent=2)+'\n')
    # The harness holds its control monitor. Give reviewers an independent
    # endpoint so their greeting/capture cannot queue behind that connection.
    (out/'ready.json').write_text(json.dumps({**attached,'registry':str(out/'registry'),'qmp':str(out/'qmp-review.sock'),
        'qemu_pid':owner.pid,'payload_id':m['payload_id'],'app_snapshot_bytes':m['application_image']['bytes'],
        'review_instruction':'Private headless QEMU only. Inspect semantic apps/windows, launch Mousepad and Files; optionally capture QMP frame. Write finish.json passed=true when finished.'},indent=2)+'\n')
    print('RAM_DESKTOP_QEMU_READY',str(out/'ready.json'),flush=True)
    end=time.monotonic()+600
    while not (out/'finish.json').exists():
        assert owner.poll() is None,'QEMU exited during review'
        assert time.monotonic()<end,'Semantic review timed out'
        time.sleep(.2)
    reviewed=json.loads((out/'finish.json').read_text())
    shutdown=uuid.uuid4().hex;state=rpc.tool('hw_power',{'session':session,'request_id':shutdown,'operation':'shutdown'});end=time.monotonic()+90
    while state['state']!='ready':
        assert state['state']!='failed' and time.monotonic()<end,state
        state=rpc.tool('hw_power',{'session':session,'request_id':shutdown,'operation':'status'});time.sleep(.1)
    (out/'shutdown.json').write_text(json.dumps(state,indent=2)+'\n');q.call('quit');assert owner.wait(timeout=15)==0
    (out/'result.json').write_text(json.dumps({'passed':reviewed.get('passed') is True,'scope':'private QEMU full offline RAM desktop','uploaded_pair':m['files'],
        'snapshot_bytes':m['application_image']['bytes'],'post_mount_free_bytes':stats['free_memory_bytes'],'review':reviewed,'checked_shutdown':True},indent=2)+'\n')
    assert reviewed.get('passed') is True,reviewed
    print('RAM_DESKTOP_QEMU_PASS')
finally:
    if rpc:rpc.close()
    if q:q.close()
    if owner.poll() is None:owner.terminate();owner.wait(timeout=10)
    log.close()
