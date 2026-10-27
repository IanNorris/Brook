/** Collect debugger stacks from workers in this harness-owned browser only. */
export async function workerStacks(browser) {
 const cdp=await browser.newBrowserCDPSession();
 let nextId=0;
 const pending=new Map(),paused=new Map();
 cdp.on('Target.receivedMessageFromTarget',event=>{
  const message=JSON.parse(event.message),key=`${event.sessionId}:${message.id}`;
  if(message.id&&pending.has(key)){pending.get(key)(message);pending.delete(key);}
  if(message.method==='Debugger.paused'&&paused.has(event.sessionId))paused.get(event.sessionId)(message.params);
 });
 const bounded=(promise,ms)=>new Promise((resolve,reject)=>{
  const timer=setTimeout(()=>reject(new Error('Worker debugger timeout')),ms);
  promise.then(value=>{clearTimeout(timer);resolve(value)},error=>{clearTimeout(timer);reject(error)});
 });
 async function command(sessionId,method,params={}){
  const id=++nextId,key=`${sessionId}:${id}`;
  const response=new Promise(resolve=>pending.set(key,resolve));
  try{
   await cdp.send('Target.sendMessageToTarget',{sessionId,message:JSON.stringify({id,method,params})});
   return await bounded(response,2000);
  }finally{pending.delete(key);}
 }
 const result=[];
 try{
  const {targetInfos}=await cdp.send('Target.getTargets');
  for(const target of targetInfos.filter(t=>t.type==='worker')){
   let sessionId;
   try{
    ({sessionId}=await cdp.send('Target.attachToTarget',{targetId:target.targetId,flatten:false}));
    await command(sessionId,'Debugger.enable');
    const notification=new Promise(resolve=>paused.set(sessionId,resolve));
    // Resume in finally even if an acknowledgement or notification is delayed.
    const pause=command(sessionId,'Debugger.pause');
    const value=await bounded(notification,3000);
    await pause;
    const jsFrame=value.callFrames.find(f=>!f.functionName.startsWith('$')&&f.location.scriptId!==value.callFrames[0].location.scriptId)||value.callFrames.find(f=>!f.functionName.startsWith('$'));
    const state=jsFrame?await command(sessionId,'Debugger.evaluateOnCallFrame',{callFrameId:jsFrame.callFrameId,expression:`(()=>{
      const tb=Module.__wasm32_tb;
      if(!tb)return {cpuWorker:false};
      const memory=new DataView(Module.HEAPU8.buffer),ctx=tb.tb_ptr_ptr-8;
      const env=memory.getUint32(ctx,true),hex=value=>'0x'+value.toString(16);
      // Pinned source tcg/wasm32.h and target/i386/cpu.h: wasm32 pointers;
      // x86_64 CPUArchState begins with 16 uint64 regs, EIP, EFLAGS.
      return {cpuWorker:true,chainRepairs:Module.__brookChainRepairCount||0,tbExceptions:Module.__brookTbExceptions||0,firstTbException:Module.__brookFirstTbException,ctx,env,tb:memory.getUint32(ctx+8,true),
        unwinding:memory.getUint32(ctx+28,true),
        rip:hex(memory.getBigUint64(env+128,true)),
        regs:Array.from({length:16},(_,i)=>hex(memory.getBigUint64(env+i*8,true)))};
    })()`,returnByValue:true}):{error:'No JavaScript closure frame'};
    result.push({target,reason:value.reason,state,frames:value.callFrames.map(f=>({name:f.functionName,url:f.url,location:f.location}))});
   }catch(error){result.push({target,error:error.message});}
   finally{
    if(sessionId){
     paused.delete(sessionId);
     try{await command(sessionId,'Debugger.resume')}catch{}
     try{await cdp.send('Target.detachFromTarget',{sessionId})}catch{}
    }
   }
  }
 }finally{await cdp.detach();}
 return result;
}
