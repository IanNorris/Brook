import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {repairSelfChains} from './wasm-self-chain.mjs';
const original=new Uint8Array(await readFile(new URL('./fixtures/self-chain.wasm',import.meta.url)));
assert(WebAssembly.validate(original));
const baseline=new WebAssembly.Instance(new WebAssembly.Module(original));
new DataView(baseline.exports.memory.buffer).setUint32(8,7,true);
assert.equal(baseline.exports.dispatch(0,8),42); // non-self path
const patched=original.slice();assert.equal(repairSelfChains(patched),1);
assert.equal(patched.length,original.length);assert(WebAssembly.validate(patched));
const changed=Array.from(patched.keys()).filter(i=>patched[i]!==original[i]);
assert.equal(changed.length,2);assert.equal(changed[1],changed[0]+1);
const fixed=new WebAssembly.Instance(new WebAssembly.Module(patched));
new DataView(fixed.exports.memory.buffer).setUint32(8,7,true);
assert.equal(fixed.exports.dispatch(0,7),42); // original would repeat its loop
assert.equal(fixed.exports.dispatch(0,8),42);
assert.equal(repairSelfChains(patched),0); // idempotent
// Identical marker in a custom section must be preserved.
const marker=original.slice(changed[0]-12,changed[0]+3);
const payload=[1,120,...marker];
const extended=new Uint8Array([...original,0,payload.length,...payload]);
assert(WebAssembly.validate(extended));assert.equal(repairSelfChains(extended),1);
assert.deepEqual(extended.slice(original.length),new Uint8Array([0,payload.length,...payload]));
// Different context field must not match the self-target comparison.
const other=original.slice();other[changed[0]-8]=12;
assert.equal(repairSelfChains(other),0);
assert.throws(()=>repairSelfChains(new Uint8Array([0,97,115,109,1,0,0,0,10,127])));
console.log('Self-chain repair: valid Wasm execution, same/different targets, exact two-byte edit, idempotence, custom-section isolation and bounds checks pass.');
