import assert from 'node:assert/strict';
import {installQmpInput} from '../dist/runtime/qmp-input.js';
const listeners=new Map(),frames=new Map(),packets=[];let next=0;
const root={addEventListener(name,fn,options){assert.equal(options.capture,true);listeners.set(name,fn)},removeEventListener(name){listeners.delete(name)}};
const document={activeElement:null};
const canvas={width:1024,height:768,ownerDocument:document,getBoundingClientRect:()=>({left:100,top:50,width:512,height:384}),focus(){document.activeElement=canvas}};
const monitor={diagnostics:{capabilities:true,transport:{pendingBytes:0}},send(name,args,id){assert.equal(name,'input-send-event');assert.match(id,/browser-input-/);packets.push(args.events);return true}};
const input=installQmpInput({canvas,getMonitor:()=>monitor,root,schedule:fn=>{frames.set(++next,fn);return next},cancel:id=>frames.delete(id)});
function fire(type,properties={}){const e={type,target:canvas,preventDefault(){this.prevented=true},stopImmediatePropagation(){this.stopped=true},...properties};listeners.get(type)(e);return e}
function frame(){const callbacks=[...frames.values()];frames.clear();callbacks.forEach(fn=>fn())}
const outside=fire('mousemove',{target:{},clientX:1,clientY:1});assert(!outside.stopped);assert.equal(packets.length,0);
fire('mousemove',{clientX:356,clientY:242});fire('mousemove',{clientX:109,clientY:425});assert.equal(frames.size,1);
const click=fire('mousedown',{button:0,clientX:109,clientY:425});assert(click.stopped&&click.prevented);assert.equal(frames.size,0);
assert.deepEqual(packets[0],[{type:'rel',data:{axis:'x',value:18}},{type:'rel',data:{axis:'y',value:750}}]);assert.equal(packets[1][0].data.down,true);
fire('mouseup',{button:0,clientX:109,clientY:425});
fire('keydown',{code:'MetaLeft'});fire('keydown',{code:'Enter'});fire('blur',{target:root});
assert.deepEqual(packets.at(-1).map(e=>e.data.key.data),['meta_l','ret']);assert(packets.at(-1).every(e=>e.data.down===false));
monitor.diagnostics.transport.pendingBytes=20000;
fire('mousemove',{clientX:200,clientY:200});frame();const count=packets.length;
monitor.diagnostics.transport.pendingBytes=0;fire('mousemove',{clientX:110,clientY:425});frame();assert.equal(packets.length,count+1);assert.deepEqual(packets.at(-1),[{type:'rel',data:{axis:'x',value:2}}]);
fire('keydown',{code:'KeyA'});monitor.diagnostics.transport.pendingBytes=20000;fire('keyup',{code:'KeyA'});monitor.diagnostics.transport.pendingBytes=0;fire('focus');assert.equal(packets.at(-1)[0].data.key.data,'a');assert.equal(packets.at(-1)[0].data.down,false);
const wheel=fire('wheel',{deltaY:10000});assert(wheel.stopped);assert.equal(packets.at(-1).length,16);
input.dispose();assert.equal(listeners.size,0);
console.log('Actual scoped QMP input handlers: host-page isolation, capture-phase interception, scaling/coalescing, button ordering, modifier release, backlog recovery, bounded wheel and disposal pass.');
