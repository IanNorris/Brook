/** Experimental diagnostic: force generated self-chains through C dispatch.
 * Pinned qemu-wasm tcg/wasm32/tcg-target.c.inc goto_ptr/goto_tb.
 * Length-preserving: br 2/3 becomes nop,nop; all other bytes stay unchanged.
 */
export function repairSelfChains(bytes) {
 const readUleb=offset=>{
  let value=0,shift=0,cursor=offset,byte;
  do{if(cursor>=bytes.length||shift>28)throw new Error('Invalid Wasm section size');byte=bytes[cursor++];value|=(byte&127)<<shift;shift+=7;}while(byte&128);
  return {value:value>>>0,next:cursor};
 };
 if(bytes.length<8||bytes[0]!==0||bytes[1]!==97||bytes[2]!==115||bytes[3]!==109)throw new Error('Not a Wasm module');
 // ctx local0, load32 align0 offset8, compare current TB, if, block-index=0,
 // br to generated top loop. Prefix excludes ordinary intra-TB label branches.
 const prefix=[0x20,0,0x28,0,8,0x46,4,0x40,0x42,0,0x24,16,0x0c];
 let count=0;
 for(let offset=8;offset<bytes.length;){
  const id=bytes[offset++],size=readUleb(offset),start=size.next,end=start+size.value;
  if(end>bytes.length)throw new Error('Truncated Wasm section');
  if(id===10){
   for(let i=start;i+prefix.length+1<end;i++){
    if(!prefix.every((value,j)=>bytes[i+j]===value))continue;
    const depth=bytes[i+13];
    if((depth!==2&&depth!==3)||bytes[i+14]!==0x0b)continue;
    bytes[i+12]=1;bytes[i+13]=1;count++;
   }
  }
  offset=end;
 }
 return count;
}
