/** Browser-local VM input. Capture precedes SDL; host-page input is untouched. */
export function installQmpInput({canvas,getMonitor,root=window,schedule=requestAnimationFrame,cancel=cancelAnimationFrame}){
 let x=0,y=0,pending=null,timer=null,sequence=0,disposed=false;
 const keys=new Map(),buttons=new Set(),listeners=[],deferred=[];
 const named={Enter:'ret',Escape:'esc',Backspace:'backspace',Tab:'tab',Space:'spc',
  ShiftLeft:'shift',ShiftRight:'shift_r',ControlLeft:'ctrl',ControlRight:'ctrl_r',
  AltLeft:'alt',AltRight:'alt_r',MetaLeft:'meta_l',MetaRight:'meta_r',
  ArrowLeft:'left',ArrowRight:'right',ArrowUp:'up',ArrowDown:'down',
  Home:'home',End:'end',PageUp:'pgup',PageDown:'pgdn',Insert:'insert',Delete:'delete',
  Minus:'minus',Equal:'equal',BracketLeft:'bracket_left',BracketRight:'bracket_right',
  Backslash:'backslash',Semicolon:'semicolon',Quote:'apostrophe',Backquote:'grave_accent',
  Comma:'comma',Period:'dot',Slash:'slash',CapsLock:'caps_lock'};
 const qkey=code=>named[code]||(/^Key[A-Z]$/.test(code)?code.slice(3).toLowerCase():/^Digit[0-9]$/.test(code)?code.slice(5):/^F([1-9]|1[0-2])$/.test(code)?code.toLowerCase():null);
 const key=(name,down)=>({type:'key',data:{down,key:{type:'qcode',data:name}}});
 const btn=(name,down)=>({type:'btn',data:{down,button:name}});
 function send(events){
  const monitor=getMonitor();
  if(disposed||!monitor?.diagnostics.capabilities||monitor.diagnostics.transport.pendingBytes>16384)return false;
  const packet=deferred.concat(events);
  if(!packet.length)return true;
  if(!monitor.send('input-send-event',{events:packet},'browser-input-'+(++sequence)))return false;
  deferred.length=0;return true;
 }
 function flush(){
  if(timer!==null){cancel(timer);timer=null;}
  if(!pending)return;
  const next=pending;pending=null;
  const events=[];
  if(next.x!==x)events.push({type:'rel',data:{axis:'x',value:next.x-x}});
  if(next.y!==y)events.push({type:'rel',data:{axis:'y',value:next.y-y}});
  if(send(events)){x=next.x;y=next.y;}
 }
 function locate(event){
  const box=canvas.getBoundingClientRect();
  return {x:Math.max(0,Math.min(canvas.width-1,Math.round((event.clientX-box.left)*canvas.width/box.width))),
          y:Math.max(0,Math.min(canvas.height-1,Math.round((event.clientY-box.top)*canvas.height/box.height)))};
 }
 function suppress(event){event.preventDefault();event.stopImmediatePropagation();}
 const inside=event=>event.target===canvas;
 function motion(event){
  if(!inside(event))return;
  suppress(event);pending=locate(event);
  if(timer===null)timer=schedule(()=>{timer=null;flush();});
 }
 function button(event){
  const name=['left','middle','right'][event.button];
  if(!inside(event)&&!buttons.has(name))return;
  suppress(event);if(inside(event)){canvas.focus();pending=locate(event);}flush();
  if(!name)return;
  const down=event.type==='mousedown';
  if(send([btn(name,down)])){if(down)buttons.add(name);else buttons.delete(name);}
  else if(!down&&buttons.delete(name))deferred.push(btn(name,false));
 }
 function keyboard(event){
  if(canvas.ownerDocument.activeElement!==canvas&&!keys.has(event.code))return;
  suppress(event);flush();const name=qkey(event.code);if(!name)return;
  const down=event.type==='keydown';
  if(send([key(name,down)])){if(down)keys.set(event.code,name);else keys.delete(event.code);}
  else if(!down&&keys.delete(event.code))deferred.push(key(name,false));
 }
 function release(){
  flush();const events=[...keys.values()].map(name=>key(name,false)).concat([...buttons].map(name=>btn(name,false)));
  keys.clear();buttons.clear();
  if(!send(events))deferred.push(...events);
 }
 function wheel(event){
  if(!inside(event))return;suppress(event);flush();
  if(!event.deltaY)return;
  const name=event.deltaY<0?'wheel-up':'wheel-down',count=Math.max(1,Math.min(8,Math.ceil(Math.abs(event.deltaY)/100)));
  send(Array.from({length:count},()=>[btn(name,true),btn(name,false)]).flat());
 }
 function listen(name,handler){root.addEventListener(name,handler,{capture:true,passive:false});listeners.push([name,handler]);}
 for(const name of ['mouseenter','mousemove'])listen(name,motion);
 for(const name of ['mousedown','mouseup'])listen(name,button);
 for(const name of ['keydown','keyup'])listen(name,keyboard);
 listen('wheel',wheel);listen('contextmenu',event=>{if(inside(event))suppress(event);});
 listen('blur',event=>{if(event.target===root||event.target===canvas)release();});
 listen('pointercancel',event=>{if(inside(event))release();});
 listen('focus',event=>{if(event.target===canvas)send([]);});
 return {release,dispose(){release();disposed=true;if(timer!==null)cancel(timer);for(const [name,handler]of listeners)root.removeEventListener(name,handler,true);}};
}
