import {readFile} from 'node:fs/promises';
import assert from 'node:assert/strict';
const load=async file=>{
 const text=await readFile(new URL('../dist/runtime/'+file,import.meta.url),'utf8');
 const begin=text.indexOf('const sjljSafeStart='),end=text.indexOf(';const fidx=',begin);
 assert(begin>0&&end>begin);
 return text.slice(begin,end).replace('const sjljSafeStart=','');
};
const original=await load('qemu-system-x86_64.pollfix.js'),fixed=await load('qemu-system-x86_64.exitfix.js');
function invoke(code,start){
 const state={restores:[],threw:[],Module:{}},heap=new Int8Array(64);new DataView(heap.buffer).setUint32(16,123,true);
 const fn=new Function('start','stackSave','stackRestore','_setThrew','Module','HEAP8','return ('+code+')')(start,()=>99,p=>state.restores.push(p),(...args)=>state.threw.push(args),state.Module,heap);
 return {state,fn};
}
const bad=invoke(original,()=>{throw 7});assert.equal(bad.fn(8),0);assert.deepEqual(bad.state.threw,[[1,0]]);
const good=invoke(fixed,()=>{throw 7});assert.throws(()=>good.fn(8),e=>e===7);assert.deepEqual(good.state.restores,[99]);assert.deepEqual(good.state.threw,[]);assert.equal(good.state.Module.__brookTbExceptions,1);assert.equal(good.state.Module.__brookFirstTbException.tb,123);
const normal=invoke(fixed,p=>p+2);assert.equal(normal.fn(8),10);assert.deepEqual(normal.state.restores,[]);
const error=new Error('fixture');const object=invoke(fixed,()=>{throw error});assert.throws(()=>object.fn(8),e=>e===error);
console.log('Actual generated TB wrapper reproduces swallowed numeric exit; repaired wrapper restores stack, propagates original exits/errors, preserves normal return, and leaves THREW ownership to outer boundary.');

const infinity=invoke(fixed,()=>{throw Infinity});assert.throws(()=>infinity.fn(8),e=>e===Infinity);assert.deepEqual(infinity.state.threw,[]);
