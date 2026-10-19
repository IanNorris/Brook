#!/usr/bin/env python3
from pathlib import Path
import sys,os,json,subprocess,shutil,time,uuid,struct,re
from elftools.elf.elffile import ELFFile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import bootstrap_acceptance as b
import argparse
p=argparse.ArgumentParser(description='Private Wine/DXVK D3D11 acceptance; run inside nix-shell from repository root')
for name in ('output','seed','nix-seed','control','wine','probe','snake'):p.add_argument('--'+name,required=True,type=Path)
p.add_argument('--instance',type=int,default=514)
p.add_argument('--offscreen-cycles',type=int,choices=range(1,11),default=3)
p.add_argument('--mouse-resize',action='store_true',help='Diagnostic CSD drag-resize instead of Win32 SetWindowPos')
p.add_argument('--trace-proc',action='store_true',help='Diagnostic syscall tracing; performance results are not comparable')
args=p.parse_args()
r=args.output.resolve();r.mkdir(parents=True,exist_ok=False);run=r/'run';run.mkdir()
seed=args.seed.resolve();assert json.loads((seed/'acceptance-audit.json').read_text())['passed']
for p in Path('/proc').glob('[0-9]*/exe'):
 try:t=str(p.resolve(strict=True))
 except (OSError,RuntimeError):continue
 assert 'qemu-system' not in Path(t).name,p
assert b.memory_available()>8*1024**3
for name in ('boot','data','nix','usb','tmp'):subprocess.run(['cp','--reflink=auto','--sparse=always',str((args.nix_seed.resolve()) if name=='nix' else seed/'run'/(name+'.img')),str(run/(name+'.img'))],check=True)
art=r/'artifacts';shutil.copytree(seed/'artifacts',art)
for part in ('kernel/BROOK.elf','kernel/ksym_addr_table.cpp','bootloader/BOOTX64.efi',*[str(p.relative_to('build/release')) for p in Path('build/release/kernel/drivers').glob('*.mod')]):shutil.copy2(Path('build/release')/part,art/part)
abis={}
for module in (art/'kernel/drivers').glob('*.mod'):
 with module.open('rb') as stream:magic,abi=struct.unpack_from('<II',ELFFile(stream).get_section_by_name('.modinfo').data())
 assert magic==0x4b524f42 and abi==33;abis[module.name]=dict(abi=abi,sha256=b.sha(module))
assert len(abis)==12;b.write(r/'module-abi-audit.json',abis)
env=dict(os.environ,BROOK_BUILD_DIR=str(art),BROOK_DISK_IMG=str(run/'boot.img'));b.command([str(b.ROOT/'scripts/update_disk.sh')],r/'update-boot.log',env)
control=json.loads((args.control.resolve()/'manifest.json').read_text());probe=Path(control['probe'])/'bin/virtgpu-context'
for name,target in [('virtgpu-context','VKCTX'),('virtgpu-query','VKQUERY'),('virtgpu-blob','VKBLOB'),('vulkan-probe','VKPROBE'),('virtgpu-sync-file','VKFENCE')]:subprocess.run(['mcopy','-o','-i',str(run/'boot.img'),str(probe.parent/name),'::/BIN/'+target],check=True)
subprocess.run(['mcopy','-o','-i',str(run/'boot.img'),str(args.probe.resolve()/'bin/brook-d3d11.exe'),'::/BIN/D3D11.EXE'],check=True)
b.write(r/'manifest.json',dict(revision=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),seed=str(seed),kernel_sha256=b.sha(art/'kernel/BROOK.elf'),probe_package=control['probe'],control_packages=control,before_fsck=b.offline_fsck(run,'before')))
for key,value in dict(BROOK_GPU_QEMU=control['qemu'],BROOK_GPU_MESA=control['mesa'],BROOK_GPU_GLVND=control['glvnd'],BROOK_GPU_VULKAN=control['vulkanLoader'],BROOK_GPU_RENDERER=control['virglrenderer'],BROOK_GPU_HOST_ICD=control['mesa']+'/share/vulkan/icd.d/intel_icd.x86_64.json',BROOK_GPU_HOSTMEM='1G',BROOK_RENDERNODE='/dev/dri/renderD128',BROOK_GPU_DISPLAY='headless').items():os.environ[key]=value
(r/'source.patch').write_bytes(subprocess.check_output(['git','diff']))
for name in ('entry-state','scheduler-start'):
 source=Path('build/release')/(name+'-audit.json');v=json.loads(source.read_text());assert v['pass'] and v['sha256']==b.sha(art/'kernel/BROOK.elf');shutil.copy2(source,r/source.name)
p,s,d,t=b.launch(run,args.instance,art,cpus=8,memory='4G',gpu='venus',composite='gpu',scenario='desktop');(r/'session.txt').write_text(s);b.write(r/'boot-stats.json',b.server.stats(s,gpu_details=True));print('BROOK_VULKAN_CONTEXT_READY',s,t,flush=True)
renderer=Path(control['virglrenderer'])/'lib/libvirglrenderer.so.1'
loaded=[]
for entry in Path('/proc').glob('[0-9]*/exe'):
 try:
  if 'qemu-system' not in entry.resolve(strict=True).name:continue
  for line in (entry.parent/'maps').read_text().splitlines():
   if 'libvirglrenderer.so' in line:loaded.append(line.split()[-1])
 except OSError:continue
assert loaded and all(Path(path).resolve()==renderer.resolve() for path in loaded)
b.write(r/'host-renderer.json',dict(loaded=sorted(set(loaded)),expected=str(renderer.resolve()),sha256=b.sha(renderer.resolve())))


commands=[]
wine=args.wine.resolve();snake=args.snake.resolve()
b.write(r/'packages.json',dict(wine=str(wine),snake=str(snake),probe=str(args.probe.resolve()),wine_unix_nsi_sha256=b.sha(wine/'lib/wine/x86_64-unix/nsiproxy.so'),probe_sha256=b.sha(args.probe.resolve()/'bin/brook-d3d11.exe'),snake_sha256=b.sha(snake/'bin/brook-snake.exe'),sdl_sha256=b.sha(snake/'bin/SDL3.dll')))
shutil.copy2(__file__,r/'acceptance-driver.py')
def launch(name,argv,env=None):
 out=r/name;out.mkdir();j=b.TrackedCommand(out,s,argv)
 j.evidence['request'].update(uid=1000,cwd='/data/home',env=env or {});b.write(j.path,j.evidence);commands.append(j)
 if args.trace_proc and name.startswith(('offscreen','window','snake','wine-stop')):
  # guest_run creates an independent process, rather than forking the debug
  # server. Enable tracing after it exists; future children inherit the flag.
  try:j.poll()
  except RuntimeError:
   if j.evidence['status'].get('state')!='completed':raise
  b.write(out/'process-trace.json',b.server.guest(d,'strace all proc'))
 return j
def poll(j):
 # Wine services may keep an earlier command's pipes open after its main exit.
 # Drain those original lineages while observing the next app, rather than
 # losing their output or relaunching them under another request identity.
 for current in commands:
  if current is not j and current.evidence['status'].get('state')=='completed':continue
  try:current.poll()
  except RuntimeError:
   if current.evidence['status'].get('state')!='completed':raise
 return j.evidence['status']
def wait(j,main_only=False,seconds=180,allowed=(0,)):
 end=time.monotonic()+seconds
 while True:
  status=poll(j);ex=status.get('exit',{})
  if (ex.get('code') is not None or ex.get('signal') is not None) and (main_only or (status['guest']['live_tasks']==0 and all(status[k]['eof'] for k in ('stdout','stderr')))):break
  if time.monotonic()>end:raise TimeoutError('Keep observing original request '+str(j.path))
  time.sleep(.15)
 assert status['exit']['code'] in allowed and status['exit']['signal'] is None,(j.path,status['exit'],j.evidence['stdout'][-1200:],j.evidence['stderr'][-3000:])
 if not main_only:
  for stream in ('stdout','stderr'):
   v=status[stream];assert v['eof'] and not v['gaps'] and not v['dropped_bytes'] and not v['read_error']
   assert len(j.evidence[stream].encode())==j.evidence[stream+'_offset']==status['guest'][stream+'_bytes']
 print('COMMAND_PASS',j.path.parent.name,j.evidence['stdout'][-1800:],flush=True);return status
def guest_command(name,argv,env=None):
 j=launch(name,argv,env);wait(j);return j.evidence['stdout']
def windows():return b.server.windows(s)['windows']
def window(j,title,predicate=lambda w:True,seconds=60):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  status=poll(j);found=next((w for w in windows() if title in w['title'] and predicate(w)),None)
  if found:return found
  assert status.get('exit',{}).get('code') is None and status.get('exit',{}).get('signal') is None,(str(j.path),status.get('exit'),j.evidence['stdout'][-1200:],j.evidence['stderr'][-2000:])
  time.sleep(.15)
 b.write(r/'window-timeout-observation.json',b.server.observe(s,include=['windows','processes']))
 b.write(r/'window-timeout-stats.json',b.server.stats(s,gpu_details=True,memory_details=True))
 b.write(r/'window-timeout-profile-start.json',b.server.profile(s,action='start',duration_ms=1500))
 time.sleep(2)
 b.write(r/'window-timeout-profile-stop.json',b.server.profile(s,action='stop'))
 time.sleep(1)
 b.write(r/'window-timeout-profile-export.json',b.server.profile(s,action='export'))
 raise TimeoutError('Observe original window '+title)
def action(out,w,name):
 result=b.server.window_action(s,w['reference'],name,uuid.uuid4().hex,restore_target=('previous' if w.get('minimized') else 'normal') if name=='restore' else None,wait_for='applied',wait_s=2)
 b.write(out/(name+'-'+uuid.uuid4().hex[:8]+'.json'),result)
def snapshot(name):
 data=b.server.capture(s,d,boot_id=boot_id)['data'];b.write(r/(name+'-frame.json'),data);shutil.copy2(data['artifact'],r/(name+'.png'));return data
wine_env=dict(WINEPREFIX='/data/home/wine-dxvk/prefix',WINEARCH='win64',HOME='/data/home',DISPLAY='',WAYLAND_DISPLAY='wayland-0',XDG_RUNTIME_DIR='/tmp',WINEDLLOVERRIDES='d3d11,dxgi=n;mscoree,mshtml=',WINEDEBUG='-all,err+all',DXVK_LOG_LEVEL='info',DXVK_LOG_PATH='/data/home/wine-dxvk/logs',DXVK_FILTER_DEVICE_NAME='Venus')
try:
 boot_id=b.server.windows(s)['guest_boot_id']
 diagnostic="""import os,socket,json,tempfile
result={}
for name,fn in [('if_nameindex',socket.if_nameindex),('listxattr',lambda:os.listxattr('/data/home/wine-notepad-acceptance.txt'))]:
 try:result[name]=dict(value=fn())
 except OSError as error:result[name]=dict(errno=error.errno)
with tempfile.TemporaryDirectory(dir='/data/home') as tmp:
 path=tmp+'/file';open(path,'w').write('metadata probe')
 for name,fn in [('chmod',lambda:os.chmod(path,0o600)),('utime',lambda:os.utime(path,None))]:
  try:fn();result[name]=dict(supported=True)
  except OSError as error:result[name]=dict(errno=error.errno)
print(json.dumps(result))
"""
 guest_command('metadata-diagnostic',['/nix/profile/bin/python3','-c',diagnostic])
 code="""
 import os,shutil,json,stat,hashlib
 def_clone_placeholder
 root='/data/home/wine-dxvk';os.mkdir(root);os.mkdir(root+'/app');os.mkdir(root+'/logs')
 clone('/data/home/wine-acceptance/prefix-file-trace',root+'/prefix')
 shutil.copyfile('/boot/BIN/D3D11.EXE',root+'/app/brook-d3d11.exe')
 for name in ('d3d11.dll','dxgi.dll'):shutil.copyfile('/nix/store/ra8zv0lmhrlglpc0908ibmjf13qpm254-dxvk-x86_64-w64-mingw32-2.7.1/bin/'+name,root+'/app/'+name)
 snake_package_placeholder
 print(json.dumps(dict(prepared=True,original_prefix_unchanged=True)))
 """
 import textwrap
 code=textwrap.dedent(code).replace('def_clone_placeholder',"""def clone(src,dst):
 if os.path.islink(src):os.symlink(os.readlink(src),dst);return
 if os.path.isdir(src):
  os.mkdir(dst)
  for name in os.listdir(src):clone(src+'/'+name,dst+'/'+name)
 else:
  with open(src,'rb') as source,open(dst,'wb') as target:shutil.copyfileobj(source,target,1024*1024)
  with open(src,'rb') as source,open(dst,'rb') as target:assert hashlib.sha256(source.read()).digest()==hashlib.sha256(target.read()).digest()
 os.chmod(dst,stat.S_IMODE(os.stat(src).st_mode))""").replace('snake_package_placeholder',"\n".join('shutil.copyfile('+repr(str(snake/'bin'/name))+",root+'/app/"+name+"')" for name in ('brook-snake.exe','SDL3.dll')))
 guest_command('prepare',['/nix/profile/bin/python3','-c',code])
 if args.trace_proc:b.write(r/'process-trace.json',b.server.guest(d,'strace all proc'))
 for i in range(args.offscreen_cycles):
  j=launch('offscreen'+str(i+1),[str(wine/'bin/wine'),'Z:\\data\\home\\wine-dxvk\\app\\brook-d3d11.exe','--offscreen'],wine_env);wait(j,main_only=True)
  result=json.loads(j.evidence['stdout'].splitlines()[-1]);assert result['passed'] and result['offscreen'] and result['frames']==1
  assert 'DXVK: 2.7.1' in j.evidence['stderr'] and 'D3D11_ADAPTER Virtio-GPU Venus' in j.evidence['stdout']
 for i in range(2):
  name='window'+str(i+1);out=r/name;start=time.monotonic()
  j=launch(name,[str(wine/'bin/wine'),'Z:\\data\\home\\wine-dxvk\\app\\brook-d3d11.exe','--frames','6000'],wine_env)
  w=window(j,'Brook D3D11 via DXVK');b.write(out/'first-window.json',dict(window=w,seconds=time.monotonic()-start))
  frame=snapshot(name+'-initial');action(out,w,'focus')
  b.write(out/'input.json',b.server.input_actions(s,[dict(type='keypress',keys=['a'],hold_seconds=.15),dict(type='keypress',keys=['d'],hold_seconds=.15)]))
  # Wine CSD surrounds a 640x360 initial D3D client; native size includes it.
  extra_w=w['buffer_width']-640;extra_h=w['buffer_height']-360
  assert 0<=extra_w<=64 and 0<=extra_h<=96,w
  target_w=1280+extra_w;target_h=720+extra_h
  if args.mouse_resize:
   x=w['x']+w['outer_width']-2;y=w['y']+w['outer_height']-2
   b.write(out/'resize-input.json',b.server.input_actions(s,[dict(type='move',x=x,y=y),dict(type='button_down'),dict(type='wait',seconds=.2),dict(type='move',x=x+target_w-w['width'],y=y+target_h-w['height']),dict(type='button_up')],frame_id=frame['frame_id']))
  else:b.write(out/'resize-input.json',b.server.input_actions(s,[dict(type='keypress',keys=['r'],hold_seconds=.15)]))
  w=window(j,'Brook D3D11 via DXVK',lambda w:w['buffer_width']==target_w and w['buffer_height']==target_h);b.write(out/'720p-window.json',w);snapshot(name+'-720p')
  action(out,w,'minimize');w=window(j,'Brook D3D11 via DXVK',lambda w:w['minimized']);action(out,w,'restore');w=window(j,'Brook D3D11 via DXVK',lambda w:not w['minimized'] and w['visible'])
  action(out,w,'maximize');w=window(j,'Brook D3D11 via DXVK',lambda w:w['maximized'] and w['buffer_width']==w['width'] and w['buffer_height']==w['height']);snapshot(name+'-maximized')
  action(out,w,'restore');w=window(j,'Brook D3D11 via DXVK',lambda w:not w['maximized'] and w['buffer_width']==w['width'] and w['buffer_height']==w['height']);action(out,w,'close');wait(j,main_only=True)
  result=json.loads(j.evidence['stdout'].splitlines()[-1]);assert result['passed'] and result['frames']>0 and result['keys']>=2 and result['size_events']>=3
  assert 'D3D11_READBACK 1280x720' in j.evidence['stdout'],j.evidence['stdout']
 # Upstream Windows SDL3 Snake, with its renderer pinned to Direct3D11.
 game_env=dict(wine_env,SDL_VIDEODRIVER='windows',SDL_AUDIODRIVER='wasapi',SDL_RENDER_DRIVER='direct3d11')
 j=launch('snake',[str(wine/'bin/wine'),'Z:\\data\\home\\wine-dxvk\\app\\brook-snake.exe'],game_env)
 w=window(j,'examples/demo/snake');snapshot('snake-initial');action(r/'snake',w,'focus')
 actions=[dict(type='keypress',keys=[key],hold_seconds=.15) for key in ('up','left','down','right')]
 b.write(r/'snake/input.json',b.server.input_actions(s,actions));time.sleep(2);snapshot('snake-playing')
 b.write(r/'snake/profile.json',b.server.profile(s,action='start',duration_ms=1500))
 time.sleep(2);b.write(r/'snake/profile-stop.json',b.server.profile(s,action='stop'))
 end=time.monotonic()+15
 while 'IDLE' not in b.server.profile(s,action='status').get('reply',''):
  assert time.monotonic()<end,'Profile has not flushed';time.sleep(.2)
 profile=b.server.profile(s,action='export');b.write(r/'snake/profile-export.json',profile)
 assert profile['sample_count']>0 and profile['context_switch_count']>0 and not profile['lossy'],profile
 shutil.copy2(profile['profile'],r/'snake/capture.profile')
 if profile['speedscope']:shutil.copy2(profile['speedscope'],r/'snake/capture.speedscope.json')
 action(r/'snake',w,'minimize');w=window(j,'examples/demo/snake',lambda w:w['minimized']);action(r/'snake',w,'restore');w=window(j,'examples/demo/snake',lambda w:not w['minimized'])
 snapshot('snake-restored');action(r/'snake',w,'close');wait(j,main_only=True)
 assert 'DXVK: 2.7.1' in j.evidence['stderr']
 # Stop background Wine services only when a tracked lineage is still alive.
 pending=[job for job in commands if poll(job)['guest']['live_tasks']]
 if pending:
  job=launch('wine-stop',[str(wine/'bin/wineserver'),'-k'],wine_env);wait(job,allowed=(0,1))
 for job in commands:wait(job,allowed=(0,1) if job.path.parent.name=='wine-stop' else (0,))
 assert not windows()
 name='gpu_egl_readback_check.py';argv=[name,'--brook-guest','--mesa',control['mesa'],'--glvnd',control['glvnd'],'--persistent']
 code='import sys;sys.argv='+repr(argv)+';\n'+Path('scripts/workloads',name).read_text();assert json.loads(guest_command('linux-gl',['/nix/profile/bin/python3','-c',code]))['passed']
 assert json.loads(guest_command('linux-vulkan',['/boot/BIN/VKPROBE','--expect-driver','venus']))['passed']
 end=time.monotonic()+15
 while True:
  gpu=b.server.stats(s,gpu_details=True)['gpu_backing']
  if not any(gpu[k] for k in ('host_blob_resources','host_aperture_leases','host_aperture_bytes','host_blob_quarantined','quarantined_resources','failed_retirements')) and gpu['allocated_mappings']==gpu['released_mappings'] and gpu['allocated_page_bytes']==gpu['released_page_bytes']:break
  assert time.monotonic()<end,gpu;time.sleep(.1)
 heap=b.server.guest(d,'heap check');b.write(r/'heap-check.json',heap);assert heap['reply']=='heap integrity: OK'
finally:
 b.write(r/'final-stats.json',b.server.stats(s,audio_details=True,gpu_details=True,memory_details=True));b.write(r/'final-observation.json',b.server.observe(s,include=['windows','processes']))
 request=dict(session=s,action='shutdown',request_id=uuid.uuid4().hex,wait_s=10);b.write(r/'shutdown-request.json',request);b.write(r/'shutdown-result.json',b.server.lifecycle_operation(**request))
 p.wait(timeout=120);b.wait_launcher(p,r,s);b.wait_existing_vm(d)
 try:b.check_stopped_serial(d,r)
 finally:b.write(r/'after-fsck.json',b.offline_fsck(run,'after'))
 status=b.server.lifecycle_operation(**request);b.write(r/'shutdown-result.json',status);assert status['orderly_shutdown_completed'] and status['qemu_exit_code']==0
 text=(r/'guest-final-serial.log').read_text()
 imports=re.findall(r'virtio_gpu: imported Venus image ctx=(\d+) res=(\d+) (\d+)x(\d+) stride=(\d+) offset=(\d+)',text)
 b.write(r/'presentation-evidence.json',dict(venus_image_imports=imports))
 errors=[line for line in (text+(run/'launch.log').read_text(errors='replace')).splitlines() if re.search(r'\[FAULT\] User fault|Delivering signal 11|context error|failed to dispatch|virtio_gpu_virgl_process_cmd.*error|IOV data size exceeds|retained (scene|image capture) failed',line,re.I)]
 b.write(r/'runtime-errors.json',dict(passed=not errors,errors=errors));assert not errors,errors
 print('WINE_DXVK_CHECKED_STOP',s,flush=True)
b.write(r/'acceptance-complete.json',dict(passed=True,offscreen_cycles=args.offscreen_cycles,diagnostic_trace=args.trace_proc,window_cycles=2,upstream_snake=True,checked_shutdown=True,offline_fsck_passed=True))
