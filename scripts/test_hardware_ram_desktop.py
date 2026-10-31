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
p.add_argument('--private-disk-mib',type=int,default=0,help='Build a larger disposable FAT disk by copying the private probe files; 0 reflinks the probe')
p.add_argument('--memory-mb',type=int,default=2048,choices=[2048,4096,8192,16384])
p.add_argument('--capture-network',action='store_true',help='Record the private QEMU network to network.pcap')
p.add_argument('--allow-host-network',action='store_true',help='Permit QEMU user-network access to an explicit test server')
p.add_argument('--sata-image',type=Path,help='Attach a regular-file SATA fixture with a disposable QEMU snapshot overlay')
p.add_argument('--sata-qemu-scratch',type=Path,help='Copy a recognized fixture into this output and attach it with a test-only serial')
p.add_argument('--installer-source',type=Path,help='Copy a recognized regular-file source into the private USB fixture for the test-only installer')
p.add_argument('--installer-package',type=Path,help='Copy a verified compact test package into the private USB fixture')
p.add_argument('--installer-segmented',action='store_true',help='Split the test source into two pinned FAT files')
p.add_argument('--installer-compact',action='store_true',help='Use the compact layout for the recognized 64 MiB QEMU fixture')
p.add_argument('--installer-omit-second',action='store_true',help='Omit the second source segment for a failure test')
p.add_argument('--review-timeout',type=int,default=600,help='Maximum seconds to wait for the test reviewer (60..7200)')
a=p.parse_args();desktop=a.desktop.resolve(strict=True);maintenance=a.maintenance.resolve(strict=True);out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
if not (640<=a.width<=4096 and 480<=a.height<=2160):p.error('Private test display must be 640..4096 by 480..2160')
if not 60<=a.review_timeout<=7200:p.error('review timeout must be 60..7200 seconds')
m=json.loads((desktop/'manifest.json').read_text());mm=json.loads((maintenance/'manifest.json').read_text())
assert m['application_image']['sha256']==mm['files']['APPS.BRK']['sha256']
def run(*argv):subprocess.run(list(map(str,argv)),check=True,cwd=ROOT,stdin=subprocess.DEVNULL)
disk=out/'private.raw'
if a.private_disk_mib:
    if not 768<=a.private_disk_mib<=8192:p.error('Private FAT disk must be 768..8192 MiB')
    partition_bytes=a.private_disk_mib*1024**2
    with disk.open('xb') as stream:stream.truncate(partition_bytes+2*1024**2)
    layout=f'label: dos\nunit: sectors\n\nstart=2048,size={partition_bytes//512},type=c,bootable\n'
    subprocess.run(['sfdisk',str(disk)],input=layout,text=True,check=True,stdout=subprocess.DEVNULL)
    run('mkfs.fat','-F','32','--offset','2048',disk,str(partition_bytes//1024))
    staging=out/'probe-files';staging.mkdir()
    run('mcopy','-s','-i',str(maintenance/'probe.raw')+'@@1048576','::/*',str(staging)+'/')
    for child in staging.iterdir():run('mcopy','-s','-i',str(disk)+'@@1048576',child,'::/')
    shutil.rmtree(staging)
else:run('cp','--reflink=auto',maintenance/'probe.raw',disk)
if a.installer_source and a.installer_package:p.error('choose one installer source')
if a.installer_segmented and not (a.installer_source or a.installer_package):p.error('segmented source requires an installer source')
if a.installer_omit_second and not a.installer_segmented:p.error('omitting a segment requires --installer-segmented')
if a.installer_compact and not a.installer_segmented:p.error('compact source requires --installer-segmented')
if a.installer_package:
    if not a.installer_compact or not a.installer_segmented or not a.sata_qemu_scratch:
        p.error('package requires compact segmented source and SATA scratch')
    from pack_ssd_install_source import verify_package
    package=verify_package(a.installer_package)
    if package['geometry'] != {'disk_sectors':131072,'payload_first':2048,'payload_sectors':126976} or package['segment_bytes']!=32*1024**2:
        p.error('package does not match the recognized QEMU fixture')
    for i,item in enumerate(package['segments']):
        if i==1 and a.installer_omit_second:break
        run('mcopy','-o','-i',str(disk)+'@@1048576',a.installer_package/item['name'],'::'+item['name'])
    marker=out/'INSTCMP.OK';marker.write_text('test-only compact source\n')
    run('mcopy','-o','-i',str(disk)+'@@1048576',marker,'::INSTCMP.OK')
if a.installer_source:
    if not a.installer_segmented:p.error('the installer test kernel requires --installer-segmented')
    if not a.sata_qemu_scratch:p.error('installer source requires the private SATA scratch fixture')
    if a.installer_source.is_symlink() or not a.installer_source.is_file() or a.installer_source.stat().st_size not in (64*1024**2,6*1024**3+2*1024**2):
        p.error('installer source must be a recognized 64 MiB or 6 GiB + 2 MiB regular file')
    disk_bytes=a.installer_source.stat().st_size
    if a.installer_compact and disk_bytes!=64*1024**2:p.error('compact mode only supports the 64 MiB fixture')
    total=(34+126976+33)*512 if a.installer_compact else disk_bytes
    segment=32*1024**2 if disk_bytes==64*1024**2 else 2*1024**3
    with a.installer_source.open('rb') as source:
        if a.installer_compact:
            # The test kernel accepts only this geometry; its GPT preflight
            # independently checks the actual source before any writes.
            ranges=[(0,34*512),(2048*512,126976*512),((131072-33)*512,33*512)]
        else:ranges=[(0,total)]
        range_index=0;within=0
        for i in range((total+segment-1)//segment):
            if i==1 and a.installer_omit_second:break
            part=out/f'installer-part-{i}.tmp'
            with part.open('xb') as target:
                remaining=min(segment,total-i*segment)
                while remaining:
                    start,length=ranges[range_index]
                    n=min(1024**2,remaining,length-within)
                    source.seek(start+within);data=source.read(n)
                    if len(data)!=n:raise ValueError('short installer source')
                    target.write(data)
                    remaining-=n;within+=n
                    if within==length:range_index+=1;within=0
            run('mcopy','-o','-i',str(disk)+'@@1048576',part,f'::INST{i:04}.IMG')
            part.unlink()
    if a.installer_compact:
        marker=out/'INSTCMP.OK';marker.write_text('test-only compact source\n')
        run('mcopy','-o','-i',str(disk)+'@@1048576',marker,'::INSTCMP.OK')
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
with socket.socket() as reserve,socket.socket() as bulk_reserve,socket.socket() as store_reserve:
    reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    bulk_reserve.bind(('127.0.0.1',0));bulk_port=bulk_reserve.getsockname()[1]
    store_reserve.bind(('127.0.0.1',0));store_port=store_reserve.getsockname()[1]
argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m',str(a.memory_mb),'-nodefaults',
    '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],'-drive','if=pflash,format=raw,file='+str(variables),
    '-device','VGA,vgamem_mb=64','-device','qemu-xhci,id=usbc','-drive','if=none,id=stick,format=raw,file='+str(disk),
    '-device','usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1','-device','usb-net,bus=usbc.0,port=2,netdev=network',
    '-netdev',f'user,id=network,restrict={"off" if a.allow_host_network else "on"},hostfwd=tcp:127.0.0.1:{port}-:1234,hostfwd=tcp:127.0.0.1:{bulk_port}-:1236,hostfwd=tcp:127.0.0.1:{store_port}-:1237','-display','none','-serial','file:'+str(out/'serial.log'),
    '-qmp','unix:'+str(out/'qmp.sock')+',server=on,wait=off',
    '-qmp','unix:'+str(out/'qmp-review.sock')+',server=on,wait=off','-no-reboot']
if a.capture_network:argv += ['-object',f'filter-dump,id=netcapture,netdev=network,file={out / "network.pcap"}']
if a.sata_image:
    if a.sata_image.is_symlink() or not a.sata_image.is_file():p.error('SATA test disk must be an existing regular file, not a symlink')
    argv += ['-drive',f'if=none,id=satatest,format=raw,snapshot=on,file={a.sata_image.resolve()}',
             '-device','ide-hd,drive=satatest,bus=ide.0']
if a.sata_qemu_scratch:
    if a.sata_image:p.error('Choose one SATA test disk mode')
    if a.sata_qemu_scratch.is_symlink() or not a.sata_qemu_scratch.is_file() or a.sata_qemu_scratch.stat().st_size not in (64*1024**2,6*1024**3+2*1024**2):
        p.error('Scratch source must be a recognized regular-file test fixture')
    size=a.sata_qemu_scratch.stat().st_size
    if size!=64*1024**2 and (not a.installer_source or a.installer_source.stat().st_size!=size):
        p.error('large scratch requires a matching installer source')
    scratch=out/'sata-scratch.raw'
    run('cp','--reflink=auto','--sparse=always',a.sata_qemu_scratch,scratch)
    serial='BROOK-AHCI-TEST-01' if size==64*1024**2 else 'BROOK-AHCI-INSTALL06'
    argv += ['-drive',f'if=none,id=satatest,format=raw,file={scratch}',
             '-device',f'ide-hd,drive=satatest,bus=ide.0,serial={serial}']
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
        'qemu_pid':owner.pid,'bulk_port':bulk_port,'store_port':store_port,'payload_id':m['payload_id'],'app_snapshot_bytes':m['application_image']['bytes'],
        'review_instruction':'Private headless QEMU only. Inspect semantic apps/windows, launch Mousepad and Files; optionally capture QMP frame. Write finish.json passed=true when finished.'},indent=2)+'\n')
    print('RAM_DESKTOP_QEMU_READY',str(out/'ready.json'),flush=True)
    end=time.monotonic()+a.review_timeout
    while not (out/'finish.json').exists():
        assert owner.poll() is None,'QEMU exited during review'
        assert time.monotonic()<end,'Semantic review timed out'
        time.sleep(.2)
    reviewed=json.loads((out/'finish.json').read_text())
    shutdown=reviewed.get('shutdown_request_id')
    if shutdown:
        assert len(shutdown)==32 and all(c in '0123456789abcdef' for c in shutdown)
        state=rpc.tool('hw_power',{'session':session,'request_id':shutdown,'operation':'status'})
    else:
        shutdown=uuid.uuid4().hex;state=rpc.tool('hw_power',{'session':session,'request_id':shutdown,'operation':'shutdown'})
    end=time.monotonic()+180
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
