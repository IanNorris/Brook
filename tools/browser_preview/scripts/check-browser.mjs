import {chromium} from 'playwright';
import {workerStacks} from './worker-stacks.mjs';
import {checkGuestInput} from './check-guest-input.mjs';
import {checkQmpInput} from './check-qmp-input.mjs';
import {writeFile,mkdir} from 'node:fs/promises';

const timeoutMs=Number(process.env.BROOK_BROWSER_TIMEOUT_MS||300000);
const runId=new Date().toISOString().replaceAll(':','-');
const artifactDir=`artifacts/browser-${runId}`;
await mkdir(artifactDir,{recursive:true});
const bounded=async(promise,ms,label)=>{
  let timer;
  try{return await Promise.race([promise,new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error(`${label} exceeded ${ms} ms`)),ms);})]);}
  finally{clearTimeout(timer);}
};
// launchServer gives cleanup an exact process handle if the browser becomes unresponsive.
const server=await chromium.launchServer({headless:true,executablePath:process.env.BROOK_CHROMIUM_PATH||chromium.executablePath(),args:['--no-sandbox','--disable-dev-shm-usage']});
await writeFile(`${artifactDir}/owner.json`,JSON.stringify({harnessPid:process.pid,browserPid:server.process().pid}));
const browser=await chromium.connect(server.wsEndpoint());
const page=await browser.newPage({viewport:{width:1440,height:1200}});
let messages=[],snapshots=[],result,started=0,screenshotFailures=0,diagnosticDone=false;
const record=text=>{messages.push(text);if(messages.length>3000)messages=messages.slice(-3000);};
page.on('console',m=>record(m.type()+': '+m.text()));
page.on('pageerror',e=>record('PAGEERROR '+e.stack));
page.on('requestfailed',r=>record('REQUESTFAILED '+r.url()+' '+r.failure()?.errorText));
const save=async()=>{
  const data={...result,seconds:started?(Date.now()-started)/1000:0,artifactDir,snapshots,messages};
  await writeFile('artifacts/browser-result.json',JSON.stringify(data,null,2));
  await writeFile(`${artifactDir}/result.json`,JSON.stringify(data,null,2));
};
const screenshot=async(label)=>{
  const path=`${artifactDir}/${label}.png`;
  try{
    await bounded(page.screenshot({path,fullPage:false,timeout:3000,animations:'allow'}),4000,`Screenshot ${label}`);
    snapshots.push({path,seconds:started?(Date.now()-started)/1000:0});
  }catch(error){screenshotFailures++;record(`SCREENSHOT ${label}: ${error.message}`);}
};
try{
  await page.goto(process.env.BROOK_PREVIEW_URL||'http://127.0.0.1:38147',{waitUntil:'networkidle',timeout:15000});
  console.log(JSON.stringify({isolation:await bounded(page.evaluate(()=>crossOriginIsolated),5000,'Isolation check'),artifactDir}));
  await screenshot('before-boot');
  await page.getByRole('button',{name:'Start Brook',exact:true}).click({timeout:10000});
  started=Date.now();let nextScreenshot=15000;
  while(Date.now()-started<timeoutMs){
    await new Promise(resolve=>setTimeout(resolve,1000));
    try{
      result=await bounded(page.evaluate(()=>{
        const canvas=document.getElementById('canvas');
        const sdl=window.brook?.SDL2;
        const samplePixels=data=>{
          const stride=Math.max(4,Math.floor(data.length/4096/4)*4);
          let nonBlack=0,opaque=0,samples=0,signature=2166136261;
          for(let i=0;i+3<data.length;i+=stride){
            if(data[i]||data[i+1]||data[i+2])nonBlack++;
            if(data[i+3])opaque++;
            samples++;
            signature=Math.imul(signature^data[i],16777619);
            signature=Math.imul(signature^data[i+1],16777619);
            signature=Math.imul(signature^data[i+2],16777619);
          }
          return {samples,nonBlack,opaque,signature:signature>>>0};
        };
        let pixels;
        try{
          const probe=window.__brookPixelProbe||(window.__brookPixelProbe=document.createElement('canvas'));
          probe.width=32;probe.height=24;
          const context=probe.getContext('2d',{willReadFrequently:true});
          context.drawImage(canvas,0,0,32,24);
          pixels=samplePixels(context.getImageData(0,0,32,24).data);
        }catch(error){pixels={error:error.message};}
        return {state:document.body.dataset.state,detail:document.getElementById('detail').textContent,
          log:document.getElementById('log').textContent,diagnostics:window.brookDiagnostics,
          profile:window.brookProfile,
          qmp:window.brookQmp,
          qmpTransport:window.brook?.brookMonitor?.diagnostics?.transport,
          debugLog:window.brookDebugLog?{bytes:window.brookDebugLog.bytes,error:window.brookDebugLog.error,tail:window.brookDebugLog.tail?.slice(-65536)}:null,
          canvas:{width:canvas.width,height:canvas.height,pixels},
          sdl:sdl?{width:sdl.w,height:sdl.h,canvasMatches:sdl.ctxCanvas===canvas,
            image:sdl.image?{width:sdl.image.width,height:sdl.image.height,pixels:samplePixels(sdl.image.data)}:null}:null};
      }),5000,'Browser state evaluation');
    }catch(error){record(`UNRESPONSIVE ${error.message}`);await save();break;}
    await save();
    if(Date.now()-started>=nextScreenshot){
      await screenshot(`boot-${Math.floor((Date.now()-started)/1000)}s`);
      nextScreenshot+=30000;
      await save();
    }
    if(!diagnosticDone&&process.env.BROOK_BROWSER_DIAG_AFTER_MS&&Date.now()-started>=Number(process.env.BROOK_BROWSER_DIAG_AFTER_MS)){
      diagnosticDone=true;
      const stacks=await workerStacks(browser);
      await writeFile(`${artifactDir}/worker-stacks.json`,JSON.stringify(stacks,null,2));
      const cpus=stacks.map(w=>w.state?.result?.result?.value).filter(v=>v?.cpuWorker);
      // Read-only RAMBlock candidate discovery, validated by exact RAM size
      // and in-bounds host span. Layout is from the pinned wasm32 source.
      const ram=await bounded(page.evaluate(()=>{
        const bytes=window.brook.HEAPU8,view=new DataView(bytes.buffer),matches=[];
        for(let i=bytes.indexOf(112);i>=0;i=bytes.indexOf(112,i+1)){
          if(i<40||bytes[i+1]!==99||bytes[i+2]!==46||bytes[i+3]!==114||bytes[i+4]!==97||bytes[i+5]!==109||bytes[i+6]!==0)continue;
          const base=i-40,host=view.getUint32(base+12,true),size=view.getUint32(base+24,true);
          if(size===384*1024*1024&&host&&host+Number(size)<=bytes.length)matches.push({base,host,size:Number(size)});
        }
        return matches;
      }),5000,'RAM block discovery');
      await writeFile(`${artifactDir}/ram-candidates.json`,JSON.stringify(ram,null,2));
      const progress=[];
      for(let i=0;i<10;i++){
        progress.push(await bounded(page.evaluate(({cpus,ram})=>{
          const view=new DataView(window.brook.HEAPU8.buffer);
          return {at:performance.now(),cpus:cpus.map(cpu=>({env:cpu.env,
            tb:view.getUint32(cpus.find(c=>c.env===cpu.env).ctx+8,true),
            rip:'0x'+view.getBigUint64(cpu.env+128,true).toString(16),
            physicalBytes:ram.map(r=>{const pc=Number(view.getBigUint64(cpu.env+128,true));return pc+32<r.size?Array.from(window.brook.HEAPU8.slice(r.host+pc,r.host+pc+32)):null}),
            regs:Array.from({length:16},(_,i)=>'0x'+view.getBigUint64(cpu.env+i*8,true).toString(16))}))};
        },{cpus,ram}),2000,'Shared CPU state sample'));
        await new Promise(resolve=>setTimeout(resolve,200));
      }
      await writeFile(`${artifactDir}/cpu-progress.json`,JSON.stringify(progress,null,2));
      const before=await bounded(page.evaluate(()=>window.brook.ccall('linuxlab_is_running','number',[],[])),2000,'Pre-pause state');
      await bounded(page.evaluate(()=>window.brook.ccall('linuxlab_pause',null,[],[])),2000,'Pause request');
      await new Promise(resolve=>setTimeout(resolve,2000));
      const after=await bounded(page.evaluate(()=>window.brook.ccall('linuxlab_is_running','number',[],[])),2000,'Post-pause state');
      await writeFile(`${artifactDir}/pause-probe.json`,JSON.stringify({before,after,completed:before===1&&after===0},null,2));
      if(after===0)await bounded(page.evaluate(()=>window.brook.ccall('linuxlab_resume',null,[],[])),2000,'Resume request');
    }
    if(result.state==='error'||messages.some(x=>x.includes('PAGEERROR')))break;
    if(/Could not open temporary file|Could not load PC BIOS|is not a valid device model name/.test(result.log||'')){
      record('Stopping immediately after a fatal QEMU initialization error.');break;
    }
    if(result.state==='running'&&result.log.includes('WM: created window')&&result.canvas?.pixels?.nonBlack>0){
      await screenshot('kernel-booted');
      await page.waitForTimeout(1500);
      await page.locator('#canvas').screenshot({path:`${artifactDir}/guest-desktop.png`,timeout:5000});
      if(process.env.BROOK_BROWSER_QMP_INPUT_CHECK==='1'){
        const input=await bounded(checkQmpInput(page,artifactDir),20000,'QMP guest input acceptance');
        await writeFile(`${artifactDir}/input-result.json`,JSON.stringify(input,null,2));result.input=input;
      }
      if(process.env.BROOK_BROWSER_INPUT_CHECK==='1'){
        const input=await bounded(checkGuestInput(page,artifactDir),20000,'Guest input acceptance');
        await writeFile(`${artifactDir}/input-result.json`,JSON.stringify(input,null,2));
        result.input=input;
      }
      break;
    }
    if(screenshotFailures>=3){record('Stopping after three screenshot failures.');break;}
  }
  await screenshot('boot-result');
  await save();
  console.log(JSON.stringify({state:result?.state,detail:result?.detail,diagnostics:result?.diagnostics,profile:result?.profile,qmp:result?.qmp,qmpTransport:result?.qmpTransport,canvas:result?.canvas,sdl:result?.sdl,debugLog:result?.debugLog,logTail:result?.log?.slice(-2500),snapshots,messages:messages.slice(-12)}));
  if(result?.state!=='running'||!result?.log?.includes('WM: created window')||!(result?.canvas?.pixels?.nonBlack>0))process.exitCode=1;
}catch(error){record(`RUNERROR ${error.stack||error}`);await save();throw error;}
finally{
  try{await bounded(browser.close(),5000,'Browser close');}
  catch(error){record(`CLEANUP ${error.message}`);}
  try{await bounded(server.kill(),5000,'Tracked browser termination');}
  catch(error){record(`CLEANUP ${error.message}`);}
}
