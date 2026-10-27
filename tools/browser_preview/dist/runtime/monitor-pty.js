/** Readable PTY and optional QMP diagnostics for this browser-local QEMU only. */
export function createMonitorPty({enabled=false,oob=false,samples=true,print=console.log,onMessage=()=>{},schedule=setTimeout,unschedule=clearTimeout}={}) {
  const decoder=new TextDecoder(),encoder=new TextEncoder();
  const readableListeners=new Set(),signalListeners=new Set(),input=[];
  const timers=[];
  const started=Date.now();
  let pending='',disposed=false,scheduled=false;
  let termios={iflag:0x6500,oflag:0x0005,cflag:0x00bf,lflag:0x8a3b,
    cc:[3,28,127,21,4,0,1,0,17,19,26,0,18,15,23,22,...Array(16).fill(0)]};
  const transport={pendingBytes:0,readableChecks:0,readCalls:0,bytesRead:0,writeCalls:0,bytesWritten:0,
    registrations:0,activeReaders:0,notifications:0,lastReadSeconds:null,lastWriteSeconds:null,lastNotificationSeconds:null};
  const diagnostics={enabled,greeting:false,capabilities:false,oobOffered:false,oobEnabled:false,messages:[],transport};
  function notify(callback){
    if(disposed||!input.length||!readableListeners.has(callback))return;
    transport.notifications++;transport.lastNotificationSeconds=(Date.now()-started)/1000;
    callback();
  }
  function record(direction,message){
    const entry={direction,seconds:(Date.now()-started)/1000,message};
    diagnostics.messages.push(entry);
    if(diagnostics.messages.length>80)diagnostics.messages.shift();
    onMessage(entry);
  }
  function send(execute,args,id=execute,outOfBand=false){
    if(!enabled||disposed)return false;
    const command=outOfBand?{'exec-oob':execute,id}:{execute,id};
    if(args)command.arguments=args;
    const bytes=encoder.encode(JSON.stringify(command)+'\n');
    input.push(...bytes);transport.pendingBytes=input.length;
    record('sent',command);
    queueMicrotask(()=>{for(const callback of [...readableListeners])notify(callback);});
    return true;
  }
  function scheduleSamples(){
    if(scheduled)return;
    scheduled=true;
    for(const seconds of [5,20,60])timers.push(schedule(()=>{
      if(diagnostics.oobEnabled)send('query-yank',undefined,`oob-ping-${seconds}s`,true);
      send('query-status',undefined,`status-${seconds}s`);
      send('human-monitor-command',{'command-line':'info registers','cpu-index':0},`registers-${seconds}s`);
      if(diagnostics.oobEnabled&&seconds>=20)send('human-monitor-command',{'command-line':'x/80bx $pc-16','cpu-index':0},`pc-bytes-${seconds}s`);
      send('query-blockstats',undefined,`blockstats-${seconds}s`);
    },seconds*1000));
  }
  function receive(line){
    const text=line.trim();
    if(!text)return;
    let message;
    try{message=JSON.parse(text);}catch{print(line);return;}
    record('received',message);
    if(message.QMP){
      diagnostics.greeting=true;
      diagnostics.oobOffered=message.QMP.capabilities?.includes('oob')||false;
      const enableOob=oob&&diagnostics.oobOffered;
      send('qmp_capabilities',enableOob?{enable:['oob']}:undefined,'brook-capabilities');
    }
    if(message.id==='brook-capabilities'&&Object.hasOwn(message,'return')){
      diagnostics.capabilities=true;diagnostics.oobEnabled=oob&&diagnostics.oobOffered;if(samples)scheduleSamples();
    }
  }
  const subscribe=(set,callback)=>{
    set.add(callback);
    if(set===readableListeners){
      transport.registrations++;transport.activeReaders=set.size;
      // The Emscripten wait wrapper subscribes after its poll operation.
      // Preserve readiness if bytes arrived between that poll and subscription.
      if(input.length)queueMicrotask(()=>notify(callback));
    }
    return{dispose(){set.delete(callback);transport.activeReaders=readableListeners.size;}};
  };
  const pty={
    get readable(){transport.readableChecks++;return input.length>0;},
    get writable(){return !disposed;},
    read(length){
      transport.readCalls++;transport.lastReadSeconds=(Date.now()-started)/1000;
      const bytes=Uint8Array.from(input.splice(0,Math.max(0,length)));
      transport.bytesRead+=bytes.length;transport.pendingBytes=input.length;
      return bytes;
    },
    write(bytes){
      if(!bytes?.length)return;
      transport.writeCalls++;transport.bytesWritten+=bytes.length;transport.lastWriteSeconds=(Date.now()-started)/1000;
      const text=decoder.decode(Uint8Array.from(bytes),{stream:true});
      if(!enabled){print(text.replace(/\n$/,''));return;}
      pending+=text;
      let end;
      while((end=pending.indexOf('\n'))!==-1){receive(pending.slice(0,end));pending=pending.slice(end+1);}
      if(pending.length>65536){print(pending.slice(0,65536));pending='';}
    },
    onReadable:callback=>subscribe(readableListeners,callback),
    onSignal:callback=>subscribe(signalListeners,callback),
    ioctl(command,value){
      if(command==='TCGETS')return {...termios,cc:termios.cc.slice()};
      if(command==='TCSETS'){termios={...value,cc:Array.from(value.cc)};return;}
      if(command==='TIOCGWINSZ')return[80,25];
      return 0;
    },
  };
  return{pty,diagnostics,send,dispose(){
    disposed=true;for(const timer of timers)unschedule(timer);
    readableListeners.clear();signalListeners.clear();input.length=0;transport.pendingBytes=0;transport.activeReaders=0;
    if(pending){print(pending);pending='';}
  }};
}
