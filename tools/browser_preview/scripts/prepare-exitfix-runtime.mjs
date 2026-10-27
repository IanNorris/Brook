import {readFile,writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
const base=new URL('../dist/runtime/',import.meta.url);
const original=await readFile(new URL('qemu-system-x86_64.pollfix.js',base),'utf8');
const before='catch(error){stackRestore(stackPointer);if(error!==error+0)throw error;_setThrew(1,0);return 0}';
if(original.split(before).length!==2)throw new Error('Unexpected publisher SJLJ boundary');
const count='Module.__brookTbExceptions=(Module.__brookTbExceptions||0)+1;if(!Module.__brookFirstTbException)Module.__brookFirstTbException={value:String(error),type:typeof error,ctxPtr,tb:new DataView(HEAP8.buffer).getUint32(ctxPtr+8,true)};';
const after='catch(error){'+count+'stackRestore(stackPointer);throw error}';
const patched=original.replace(before,after);
await writeFile(new URL('qemu-system-x86_64.exitfix.js',base),patched);
await writeFile(new URL('source/EXITFIX.json',base),JSON.stringify({
 originalSha256:createHash('sha256').update(original).digest('hex'),
 patchedSha256:createHash('sha256').update(patched).digest('hex'),
 scope:'Pollfix plus restoring original nonlocal exit propagation at dynamic TB wrapper; no Wasm binary changes',
 source:'publisher patch-wasm-sjlj-boundary.mjs; qemu-wasm tcg/wasm32.c tcg_qemu_tb_exec; accel/tcg/cpu-exec-common.c cpu_loop_exit',
 change:'Restore stack then rethrow original error to the genuine enclosing Emscripten invoke_* boundary. Do not set THREW at the inner dynamic TB wrapper. Count exceptions and retain only the first error/context.'
},null,2)+'\n');
