import {readFile} from 'node:fs/promises';
import assert from 'node:assert/strict';
const runtime=new URL('../dist/runtime/',import.meta.url);
const original=await readFile(new URL('qemu-system-x86_64.js',runtime),'utf8');
const patched=await readFile(new URL('qemu-system-x86_64.pollfix.js',runtime),'utf8');
function fixture(text,readinessType=0,afterWait=()=>{}){
  const begin=text.indexOf('function xterm_pty_old_poll('),end=text.indexOf('var PTY_waitForReadableWithCallback=',begin);
  assert(begin>=0&&end>begin);
  const memory=new ArrayBuffer(32),HEAP32=new Int32Array(memory),HEAP16=new Int16Array(memory);
  HEAP32[0]=3;HEAP16[2]=1;HEAP32[2]=4;HEAP16[6]=1;
  const waits=[];let clock=0;
  const wait=timeout=>{waits.push(timeout);throw{name:'ErrnoError',errno:1006};};
  const terminal={stream_ops:{poll:(_stream,timeout)=>{if(timeout)wait(timeout);return 4;}}};
  let pipeReady=true;
  const pipe={stream_ops:{poll:()=>pipeReady?1:0}};
  const FS={getStream:fd=>fd===3?terminal:fd===4?pipe:null};
  const wrapperBegin=text.indexOf('var PTY_wrapPoll='),wrapperEnd=text.indexOf('function ___syscall_readlinkat',wrapperBegin);
  assert(wrapperBegin>=0&&wrapperEnd>wrapperBegin);
  const handleSleep=callback=>{let result;callback(value=>{result=value;});return result;};
  const waitSpecs=[];let functions;
  const waitReadable=callback=>{waitSpecs.push(functions.getSpec());clock+=Math.max(0,waits.at(-1)||0);afterWait();callback(readinessType);};
  functions=new Function('FS','SYSCALLS','HEAP32','HEAP16','PTY_askToWaitAgain','ENVIRONMENT_IS_PTHREAD','PTY_handleSleep','PTY_waitForReadable','performance',text.slice(begin,end)+text.slice(wrapperBegin,wrapperEnd)+';return {poll:xterm_pty_old_poll,wrapped:___syscall_poll,getSpec:()=>typeof PTY_pollWaitSpec===`undefined`?null:PTY_pollWaitSpec};')(FS,{DEFAULT_POLLMASK:5},HEAP32,HEAP16,wait,false,handleSleep,waitReadable,{now:()=>clock});
  return{...functions,waits,waitSpecs,setPipeReady:ready=>{pipeReady=ready;},get elapsed(){return clock;}};
}
const old=fixture(original);
assert.equal(old.poll(0,1,0),-1006,'Publisher binding blocks even with zero requested timeout');
assert.deepEqual(old.waits,[-1]);
assert.equal(old.poll(0,2,0),-1006,'Empty terminal prevents publisher binding from inspecting a ready pipe');
assert.equal(old.poll(0,0,1000),0,'Publisher binding also skips a requested idle wait without descriptors');
const fixed=fixture(patched);
assert.equal(fixed.poll(0,1,0),0,'A zero-timeout poll must return immediately');
assert.equal(fixed.poll(0,2,0),1,'A ready pipe must be observed despite empty terminal');
assert.equal(fixed.poll(0,0,1000),-1006,'Descriptor-free finite waits must also sleep');
assert.equal(fixed.poll(0,1,1000),-1006);
assert.equal(fixed.waits.at(-1),10,'Finite waits periodically recheck all descriptors');
assert.equal(fixed.poll(0,1,2),-1006);
assert.equal(fixed.waits.at(-1),2,'A shorter caller timeout must be preserved');
assert.equal(fixed.poll(0,1,-1),-1006);
assert.equal(fixed.waits.at(-1),10,'Infinite waits must recheck non-PTY descriptors');
const originalRetry=fixture(original);
assert.equal(originalRetry.wrapped(0,1,1000),-1006,'Original actual wrapper can expose its retry sentinel on a spurious wake');
const correctedRetry=fixture(patched);
assert.equal(correctedRetry.wrapped(0,1,10),0,'Actual patched wrapper retry must not begin a second wait');
assert.deepEqual(correctedRetry.waitSpecs,[{fds:0,nfds:1,timeout:10}],'Actual syscall wrapper passes fd set and bounded timeout into its wait bridge');
assert.equal(correctedRetry.getSpec(),null,'Wait specification is cleared after the syscall');
assert.deepEqual(correctedRetry.waits,[10],'Only the initial probe may enter the bounded sleep');
const finite=fixture(patched,2);
assert.equal(finite.wrapped(0,1,1000),0,'A finite idle timeout returns zero ready descriptors');
assert.equal(finite.elapsed,1000,'Positive finite timeout must not return early after its first chunk');
assert.equal(finite.waits.length,100,'A long finite timeout uses sleeping 10ms rechecks');
const short=fixture(patched,2);
assert.equal(short.wrapped(0,1,2),0);
assert.equal(short.elapsed,2,'A short finite timeout preserves its complete duration');
assert.equal(fixture(patched,1).wrapped(0,1,1000),-27,'Signal callback preserves EINTR');
assert.equal(fixture(patched).wrapped(0,2,1000),1,'Already-ready pipe bypasses sleeping entirely');
let infinite;
infinite=fixture(patched,2,()=>infinite.setPipeReady(true));
infinite.setPipeReady(false);
assert.equal(infinite.wrapped(0,2,-1),1,'An infinite wait returns when a non-PTY descriptor becomes ready');
assert.equal(infinite.elapsed,10,'Infinite waits sleep between descriptor probes');
let arrival;
arrival=fixture(patched,2,()=>{if(arrival.elapsed>=30)arrival.setPipeReady(true);});
arrival.setPipeReady(false);
assert.equal(arrival.wrapped(0,2,1000),1,'A finite timeout returns readiness before its deadline');
assert.equal(arrival.elapsed,30);
console.log('Actual generated poll binding reproduced invalid blocking; patched binding passes zero-timeout, ready-pipe, finite/infinite-wait and actual-wrapper retry/signal/timeout checks.');

// Exercise the actual UI wait binding with the real PTY adapter, including the
// pending-input-before-subscribe condition observed when QMP fills its queue.
const {createMonitorPty}=await import('../dist/runtime/monitor-pty.js');
const monitor=createMonitorPty({enabled:true,print:()=>{}});
monitor.send('query-status');
assert(monitor.diagnostics.transport.pendingBytes>0);
const waitStart=patched.indexOf('var PTY_pollWaitSpec=null;');
const waitEnd=patched.indexOf('var PTY_waitForReadable=PTY_waitForReadableWithAtomic;',waitStart);
assert(waitStart>=0&&waitEnd>waitStart);
const heap=new Int32Array(new SharedArrayBuffer(64)),shorts=new Int16Array(heap.buffer);
heap[0]=4;shorts[2]=1;heap[2]=3;shorts[6]=1;
const proxyCalls=[];
const bindings=new Function('FS','HEAP32','HEAP16','PTY','PTY_pollTimeout','ENVIRONMENT_IS_PTHREAD','proxyToMainThread','Atomics','_malloc',patched.slice(waitStart,waitEnd)+';return {wait:PTY_waitForReadableWithCallback,atomic:PTY_waitForReadableWithAtomic,setSpec:value=>{PTY_pollWaitSpec=value;}};')(
  {getStream:fd=>fd===3?{tty:{}}:{stream_ops:{}}},heap,shorts,monitor.pty,-1,false,()=>{},Atomics,()=>48);
const registrations=monitor.diagnostics.transport.registrations;
let completed=false;
const unrelated=new Promise(resolve=>bindings.wait(type=>{completed=true;resolve(type);},0,1,10));
await Promise.resolve();await Promise.resolve();
assert.equal(completed,false,'Pending PTY input must not wake a poll containing only a pipe');
assert.equal(monitor.diagnostics.transport.registrations,registrations,'Unrelated fd poll must not subscribe to PTY readability');
assert.equal(await unrelated,2,'Unrelated fd wait must complete via its bounded timer');
assert.equal(await new Promise(resolve=>bindings.wait(resolve,8,1,1000)),0,'A readable TTY in the requested fd set must wake promptly');
assert.equal(await new Promise(resolve=>bindings.wait(resolve,-1,0,1000)),0,'Non-poll terminal reads preserve normal readiness');
assert.equal(monitor.diagnostics.transport.activeReaders,0,'Completed wait subscriptions must be disposed');
monitor.dispose();

// Execute the actual worker-side bridge: the variadic existing proxy must carry
// this specific poll's fd set and timeout, rather than main-thread global state.
const worker=new Function('FS','HEAP32','HEAP16','PTY','PTY_pollTimeout','ENVIRONMENT_IS_PTHREAD','proxyToMainThread','Atomics','_malloc',patched.slice(waitStart,waitEnd)+';return {atomic:PTY_waitForReadableWithAtomic,setSpec:value=>{PTY_pollWaitSpec=value;}};')(
  {},heap,shorts,{},-1,true,(...args)=>proxyCalls.push(args),{wait:()=>{heap[12]=2;}},()=>48);
worker.setSpec({fds:8,nfds:1,timeout:7});
let result;worker.atomic(value=>{result=value;});
assert.deepEqual(proxyCalls.pop(),[31,0,12,8,1,7]);assert.equal(result,2);
worker.setSpec(null);worker.atomic(()=>{});
assert.deepEqual(proxyCalls.pop(),[31,0,12,-1,0,-1],'Ordinary terminal reads retain indefinite signal-aware waits');
console.log('Actual callback + real PTY adapter prove unrelated pending input cannot wake a pipe-only wait; actual worker bridge forwards per-wait fd set and deadline.');
