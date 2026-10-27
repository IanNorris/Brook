import {readFile,writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {repairSelfChains} from './wasm-self-chain.mjs';
const base=new URL('../dist/runtime/',import.meta.url);
let input=await readFile(new URL('qemu-system-x86_64.pollfix.js',base),'utf8');
const needle='const mod=new WebAssembly.Module(wasmBytes);';
if(input.split(needle).length!==2)throw new Error('Unexpected instantiate_wasm shape');
const body=`const repairs=(${repairSelfChains.toString()})(wasmBytes);Module.__brookChainRepairCount=(Module.__brookChainRepairCount||0)+repairs;`;
input=input.replace(needle,body+needle);
await writeFile(new URL('qemu-system-x86_64.chainfix.js',base),input);
await writeFile(new URL('source/CHAINFIX.json',base),JSON.stringify({
 scope:'Experimental generated self-loop removal; publisher Wasm unchanged',
 source:'ktock/qemu-wasm 0ef7b4e2814b231705d8371dd7997f5b72e70baf tcg/wasm32/tcg-target.c.inc goto_ptr/goto_tb',
 sha256:createHash('sha256').update(input).digest('hex'),
 change:'Inside generated TB Wasm code sections only, precise same-target ctx load/compare/top-loop branches become nops, falling through to existing ctx update + DO_INIT + return. PC-relative translation stays enabled.'
},null,2)+'\n');
