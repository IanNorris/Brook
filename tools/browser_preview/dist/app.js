import {startBrook} from './runtime/brook-runtime.js';
import {installQmpInput} from './runtime/qmp-input.js';
const $=id=>document.getElementById(id);
const canvas=$('canvas'),start=$('start'),lines=[];
const parameters=new URLSearchParams(location.search);
const profile=parameters.get('profile')||'rom';
const runtimeVariant=parameters.get('runtime')||'exitfix';
const alpineControl=parameters.get('guest')==='alpine';
const romProfile=profile==='rom';
const compatible=profile==='compatible'||profile==='ide-control'||romProfile;
const ideControl=profile==='ide-control';
const chainFix=runtimeVariant==='chainfix';
const exitFix=runtimeVariant==='exitfix';
const pollFix=runtimeVariant==='pollfix'||chainFix||exitFix;
const noChaining=parameters.get('tcg')==='nochain';
const fullNoChain=parameters.get('tcg')==='full-nochain';
const interpreterOnly=parameters.get('tcg')==='interpreter';
const qmpOob=parameters.get('monitor')==='qmp-oob';
const qmpInput=parameters.get('input')!=='sdl';
const qmpSamples=parameters.get('monitor')==='qmp'||qmpOob;
const qmpMonitor=parameters.get('monitor')==='qmp'||qmpOob||qmpInput;
if(qmpInput)window.brookInput=installQmpInput({canvas,getMonitor:()=>window.brook?.brookMonitor});
window.brookProfile={guest:alpineControl?'alpine-control':'brook',id:romProfile?'rom':ideControl?'ide-control':compatible?'compatible':'q35',machine:compatible?'pc':'q35',firmware:romProfile?'edk2-202302-rom':compatible?'edk2-202302':'OVMF-202602',disk:ideControl?'ide':'virtio',runtime:exitFix?'exitfix':chainFix?'chainfix':pollFix?'pollfix':'publisher',monitor:qmpOob?'qmp-oob':qmpMonitor?'qmp':'none',chaining:interpreterOnly?'interpreter':fullNoChain?'full-nochain':noChaining?'direct-disabled':'default'};
window.brookQmp=[];
let started=0,finished=false,drawPending=false,serialOffset=0;
window.brookDiagnostics={phase:'not-started'};
window.brookDebugLog={bytes:0,tail:''};
function detail(text){$('detail').textContent=text;}
function failBoot(error){
  document.body.dataset.state='error';$('state').textContent='Boot stopped';
  detail(error.message||String(error));$('diagnostics').open=true;
}
function log(text){
  if(/Could not open temporary file|Could not load PC BIOS|is not a valid device model name/.test(String(text)))failBoot(new Error(String(text)));
  for(const line of String(text).split('\n'))lines.push(line.slice(0,8000));
  if(lines.length>600)lines.splice(0,lines.length-600);
  if(!finished&&/WM: created window/.test(text)){
    finished=true;document.body.dataset.state='running';$('state').textContent='Kernel booted';
    detail('Click the guest display to interact with Brook.');
  }
  if(!drawPending){drawPending=true;requestAnimationFrame(()=>{drawPending=false;$('log').textContent=lines.join('\n');$('line-count').textContent=`${lines.length} lines`;$('log').scrollTop=$('log').scrollHeight;});}
}
start.addEventListener('click',async()=>{
  start.disabled=true;started=performance.now();document.body.dataset.state='booting';$('state').textContent='Booting Brook';$('restart').disabled=false;
  try{
    if(!crossOriginIsolated)throw new Error('Threaded WebAssembly needs isolation headers. Open this Site in its own browser tab.');
    $('cover').hidden=true;
    window.brook=await startBrook({canvas,print:log,status:detail,monitor:qmpMonitor,monitorOob:qmpOob,monitorSamples:qmpSamples,pollFix,chainFix,exitFix,fullNoChain,interpreterOnly,onQmp:entry=>{window.brookQmp.push(entry);if(window.brookQmp.length>80)window.brookQmp.shift();},onError:error=>{failBoot(error);log(error.message);},assets:[
      ...(alpineControl?[{url:'images/controls/alpine-virt-3.24.1-x86_64.iso',path:'/pack/control.iso'}]:[{url:'images/brook-boot.img.gz',path:'/pack/boot.img',compression:'gzip',uncompressedBytes:67108864}]),
      ...(romProfile?[{url:'images/OVMF_202302_ROM.fd',path:'/pack/OVMF_ROM.fd'}]:[
        {url:compatible?'images/OVMF_202302_CODE.fd':'images/OVMF_CODE.fd',path:'/pack/OVMF_CODE.fd'},
        {url:compatible?'images/OVMF_202302_VARS.fd':'images/OVMF_VARS.fd',path:'/pack/OVMF_VARS.fd'},
      ]),
      {url:'images/vgabios-stdvga.bin',path:'/pack/vgabios-stdvga.bin'},
      {url:'images/kvmvapic.bin',path:'/pack/kvmvapic.bin'},
    ],extraArguments:[
      ...(noChaining?['-d','guest_errors,cpu_reset,nochain']:[]),
      ...(compatible?['-machine','pc','-vga','std']:['-machine','q35','-vga','none','-device','VGA,vgamem_mb=4']),
      ...(romProfile?['-bios','/pack/OVMF_ROM.fd']:[
        '-drive','if=pflash,format=raw,unit=0,readonly=on,file=/pack/OVMF_CODE.fd',
        '-drive','if=pflash,format=raw,unit=1,file=/pack/OVMF_VARS.fd',
      ]),
      '-drive',alpineControl?'if=ide,media=cdrom,format=raw,readonly=on,file=/pack/control.iso':ideControl?'if=ide,format=raw,file=/pack/boot.img':'if=virtio,format=raw,file=/pack/boot.img',
      ...(alpineControl?['-boot','order=d,menu=on']:compatible?['-boot','order=c,menu=on']:[]),
    ]});
    $('fullscreen').disabled=!document.fullscreenEnabled;canvas.focus();
  }catch(error){failBoot(error);log(error.stack??error);}
});
setInterval(()=>{
  if(started&&!finished)$('elapsed').textContent=`${Math.floor((performance.now()-started)/1000)}s`;
  const machine=window.brook;
  if(!machine)return;
  try{
    const serial=machine.FS.readFile('/pack/serial.log',{encoding:'utf8'});
    if(serial.length>serialOffset){log(serial.slice(serialOffset));serialOffset=serial.length;}
  }catch(error){if(error.errno!==44)console.debug('Serial log not available yet:',error.message);}
},1000);
setInterval(()=>{
  const machine=window.brook;
  if(!machine)return;
  try{
    try{
      const path='/pack/qemu-debug.log',size=machine.FS.stat(path).size;
      const length=Math.min(size,4*1024*1024),buffer=new Uint8Array(length);
      const stream=machine.FS.open(path,'r');
      let count=0;
      try{count=machine.FS.read(stream,buffer,0,length,Math.max(0,size-length));}
      finally{machine.FS.close(stream);}
      window.brookDebugLog={bytes:size,tail:new TextDecoder().decode(buffer.subarray(0,count))};
    }catch(error){if(error.errno!==44)window.brookDebugLog={error:error.message};}
    const ready=machine.ccall('linuxlab_is_ready','number',[],[]);
    const running=machine.ccall('linuxlab_is_running','number',[],[]);
    window.brookDiagnostics={phase:'emulator',ready:!!ready,running:!!running,
      virtualClockNs:ready?machine.ccall('linuxlab_virtual_clock_ns','number',[],[]):null,
      elapsedTicks:ready?machine.ccall('linuxlab_elapsed_ticks','number',[],[]):null,
      serialBytes:serialOffset,jitLiveInstances:Atomics.load(machine.HEAP32,8011316>>>2),wallSeconds:Math.floor((performance.now()-started)/1000)};
  }catch(error){window.brookDiagnostics={phase:'diagnostic-error',message:error.message};}
},5000);
$('restart').addEventListener('click',()=>location.reload());
$('fullscreen').addEventListener('click',async()=>{await canvas.parentElement.requestFullscreen();canvas.focus();});
canvas.addEventListener('pointerdown',()=>canvas.focus());
$('save-log').addEventListener('click',()=>{const link=document.createElement('a'),url=URL.createObjectURL(new Blob([lines.join('\n')],{type:'text/plain'}));link.href=url;link.download='brook-browser-boot.log';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);});
