/** Source-correlated diagnostic build substitute; never alters publisher files. */
import {readFile,writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
const base=new URL('../dist/runtime/',import.meta.url);
const original=new Uint8Array(await readFile(new URL('qemu-system-x86_64.wasm.part0',base)));
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
if(hash(original)!=='fbb8f2c84b4a6dacdad867709126fce3ae42ab616bd611f26f806a415f49f9f1')throw new Error('Unexpected Wasm input');
// func4325 x86_cpu_realizefn, CPUState.tcg_cflags at offset512.
// Source cpu.c: cs->tcg_cflags |= CF_PCREL; followed by apic_id == -1 check.
// Change only its OR immediate 0x20000 -> 0x20600 (retain PCREL, add
// CF_NO_GOTO_TB|CF_NO_GOTO_PTR). Disassembly and neighboring source checks
// are recorded in artifacts/x86-realize-candidate.txt.
const prefix=[0x20,0,0x20,0,0x28,2,0x80,4,0x41,0x80,0x80,8,0x72,0x36,2,0x80,4];
const matches=[];
for(let i=0;i+prefix.length<=original.length;i++)if(prefix.every((v,j)=>original[i+j]===v))matches.push(i);
if(matches.length!==1||matches[0]+8!==0x303fbd)throw new Error('CPU realization pattern mismatch');
const patched=original.slice();patched[matches[0]+10]=0x8c;
if(!WebAssembly.validate(original)||!WebAssembly.validate(patched))throw new Error('Invalid Wasm module');
const changes=Array.from(patched.keys()).filter(i=>patched[i]!==original[i]);
if(changes.length!==1)throw new Error('Unexpected byte changes');
const file='qemu-system-x86_64.nochain.wasm.part0';
await writeFile(new URL(file,base),patched);
const manifest=JSON.parse(await readFile(new URL('runtime.json',base),'utf8'));
manifest.parts=[{file,bytes:patched.length,sha256:hash(patched)}];
manifest.patchedSha256=hash(patched);
manifest.patch+=' Experimental source-correlated CPU realization flag immediate: retain CF_PCREL, add CF_NO_GOTO_TB|CF_NO_GOTO_PTR.';
manifest.diagnostic={byteOffset:changes[0],before:original[changes[0]],after:patched[changes[0]],scope:'Unpublished full-nochain control; not an upstream release or source-rebuilt artifact'};
await writeFile(new URL('runtime-nochain.json',base),JSON.stringify(manifest,null,2)+'\n');
console.log(JSON.stringify(manifest.diagnostic));
