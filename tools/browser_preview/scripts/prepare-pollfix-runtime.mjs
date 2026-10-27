import {readFile,writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
const root=new URL('../',import.meta.url);
const original=await readFile(new URL('dist/runtime/qemu-system-x86_64.js',root),'utf8');
const hash=text=>createHash('sha256').update(text).digest('hex');
if(hash(original)!=='5b06c8c8e96cda2610ed66478b2c8a53510627156c003fa7efd26aa155ef6bbe')throw new Error('Unexpected publisher JS input');
const start=original.indexOf('function xterm_pty_old_poll(');
const end=original.indexOf('var PTY_waitForReadableWithCallback=',start);
if(start<0||end<0)throw new Error('Poll binding not found');
const before=original.slice(start,end);
const probe='stream.stream_ops.poll(stream,-1)';
const completion='return nonzero}catch(e)';
if(before.split(probe).length!==2||before.split(completion).length!==2)throw new Error('Unexpected poll binding shape');
// Probe every fd without blocking. If none is ready, use the existing worker
// wait path with a <=10 ms timeout so pipe writes are observed as well as PTY.
const after=before.replace(probe,'stream.stream_ops.poll(stream,0)').replace(completion,'if(!nonzero&&timeout){PTY_askToWaitAgain(timeout<0?10:Math.min(timeout,10))}return nonzero}catch(e)');
const wrapper='var ___syscall_poll=(fds,nfds,timeout)=>PTY_wrapPoll(()=>xterm_pty_old_poll(fds,nfds,timeout));';
if(original.split(wrapper).length!==2)throw new Error('Unexpected syscall poll wrapper shape');
// PTY_wrapPoll retries once after an input notification. That retry must probe
// without initiating another wait, or its internal -1006 sentinel escapes.
const replacement='var ___syscall_poll=(fds,nfds,timeout)=>{const deadline=timeout>=0?performance.now()+timeout:Infinity;do{let initial=true;const remaining=timeout<0?-1:Math.max(0,deadline-performance.now());let result;const previous=PTY_pollWaitSpec;PTY_pollWaitSpec={fds,nfds,timeout:remaining<0?10:Math.min(remaining,10)};try{result=PTY_wrapPoll(()=>{const requested=initial?remaining:0;initial=false;return xterm_pty_old_poll(fds,nfds,requested)})}finally{PTY_pollWaitSpec=previous}if(result!==0||timeout===0)return result}while(timeout<0||performance.now()<deadline);return 0};';
let patched=(original.slice(0,start)+after+original.slice(end)).replace(wrapper,replacement);
// Pass each wait's fd set and timeout across the existing variadic main-thread
// proxy. A full QMP queue can leave PTY input pending while QEMU polls only its
// notification pipe: that unrelated input must not wake this poll repeatedly.
const replaceOnce=(before,after)=>{if(patched.split(before).length!==2)throw new Error('Unexpected wait binding shape: '+before);patched=patched.replace(before,after)};
replaceOnce('var PTY_waitForReadableWithCallback=callback=>{if(PTY_pollTimeout===0){return callback(PTY.readable?0:2)}', 'var PTY_pollWaitSpec=null;var PTY_waitForReadableWithCallback=(callback,waitFds=-1,waitNfds=0,waitTimeout=PTY_pollTimeout)=>{let watchReadable=waitFds<0;if(!watchReadable){for(let i=0;i<waitNfds;i++){const ptr=waitFds+8*i;const stream=FS.getStream(HEAP32[ptr>>>2]);if(stream&&stream.tty&&(HEAP16[(ptr+4)>>>1]&1)){watchReadable=true;break}}}if(waitTimeout===0){return callback(watchReadable&&PTY.readable?0:2)}');
replaceOnce('handlerReadable=PTY.onReadable(()=>resolve(0));','if(watchReadable)handlerReadable=PTY.onReadable(()=>resolve(0));');
replaceOnce('if(PTY_pollTimeout>=0){timeoutId=setTimeout(resolve,PTY_pollTimeout,2)}','if(waitTimeout>=0){timeoutId=setTimeout(resolve,waitTimeout,2)}');
replaceOnce('handlerReadable.dispose();','if(handlerReadable)handlerReadable.dispose();');
replaceOnce('var PTY_waitForReadableWithAtomicImpl=function(atomicIndex){if(ENVIRONMENT_IS_PTHREAD)return proxyToMainThread(31,0,atomicIndex);','var PTY_waitForReadableWithAtomicImpl=function(atomicIndex,waitFds=-1,waitNfds=0,waitTimeout=-1){if(ENVIRONMENT_IS_PTHREAD)return proxyToMainThread(31,0,atomicIndex,waitFds,waitNfds,waitTimeout);');
replaceOnce('Atomics.notify(HEAP32,atomicIndex)})','Atomics.notify(HEAP32,atomicIndex)},waitFds,waitNfds,waitTimeout)');
replaceOnce('PTY_waitForReadableWithAtomicImpl(PTY_atomicIndex);','const spec=PTY_pollWaitSpec;PTY_waitForReadableWithAtomicImpl(PTY_atomicIndex,spec?spec.fds:-1,spec?spec.nfds:0,spec?spec.timeout:-1);');
const destination=new URL('dist/runtime/qemu-system-x86_64.pollfix.js',root);
await writeFile(destination,patched);
await writeFile(new URL('dist/runtime/source/POLLFIX.json',root),JSON.stringify({
  originalSha256:hash(original),patchedSha256:hash(patched),file:'qemu-system-x86_64.pollfix.js',
  scope:'Generated JS poll binding, syscall retry wrapper, and fd-specific PTY wait binding; Wasm unchanged.',
  change:'Nonblocking per-fd probes and existing PTY sleeps in <=10ms chunks; finite caller deadlines are preserved across rechecks, infinite waits continue until fd readiness; readiness retries cannot leak internal -1006. PTY input only wakes polls requesting readable TTY descriptors; per-wait fd set and timeout travel through the existing main-thread proxy. This remains a sleeping periodic 10ms recheck with experimental CPU overhead.'
},null,2)+'\n');
await writeFile(new URL('dist/runtime/source/prepare-pollfix-runtime.mjs',root),await readFile(new URL(import.meta.url)));
console.log(fileURLToPath(destination));
