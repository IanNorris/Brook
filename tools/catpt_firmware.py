"""Offline Intel Broadwell CATPT firmware inventory; never accesses hardware.

Format and WPT memory layout: Linux sound/soc/intel/catpt/{loader.c,device.c,registers.h}.
Validation is not authorization to load firmware on any device.
"""
import argparse,hashlib,json,os,stat,struct
from pathlib import Path

HEADER=struct.Struct('<4s7I')
MODULE=struct.Struct('<4sIIHHIII')
BLOCK=struct.Struct('<IIII')
# WPT masks cover 20 DRAM and 10 IRAM banks of 0x8000 bytes each.
RAM={1:('iram',0x50000,0xa0000),2:('dram',0xa0000,0),3:('instance',0xa0000,0)}
MAX_FILE=16*1024*1024

def inspect(data):
    if len(data)<HEADER.size or len(data)>MAX_FILE:raise ValueError('invalid image size')
    sig,size,count,fmt,*reserved=HEADER.unpack_from(data)
    if sig!=b'$SST' or size!=len(data)-HEADER.size or fmt!=254:raise ValueError('invalid image header')
    if count==0 or count>(len(data)-HEADER.size)//MODULE.size or count>256:raise ValueError('invalid module count')
    offset=HEADER.size;modules=[];ranges={'iram':[],'dram':[]};seen=set()
    for _ in range(count):
        if offset+MODULE.size>len(data):raise ValueError('truncated module header')
        signature,modsize,blocks,slot,modid,entry,persistent,scratch=MODULE.unpack_from(data,offset)
        if signature!=sig:raise ValueError('invalid module signature')
        if modid>15 or modid in seen:raise ValueError('unsupported or duplicate module ID')
        seen.add(modid)
        start=offset+MODULE.size;end=start+modsize
        if end>len(data) or blocks>modsize//BLOCK.size:raise ValueError('invalid module extent')
        offset=start;items=[]
        for _ in range(blocks):
            if offset+BLOCK.size>end:raise ValueError('truncated block header')
            typ,length,ramoffset,rsvd=BLOCK.unpack_from(data,offset);offset+=BLOCK.size
            if typ not in RAM:raise ValueError('unsupported RAM type')
            name,limit,base=RAM[typ]
            if not length or offset+length>end or ramoffset>limit or length>limit-ramoffset:raise ValueError('invalid block extent')
            region='iram' if typ==1 else 'dram'
            if any(ramoffset<e and s<ramoffset+length for s,e in ranges[region]):raise ValueError('overlapping RAM blocks')
            ranges[region].append((ramoffset,ramoffset+length))
            items.append({'type':name,'size':length,'ram_offset':ramoffset,'host_relative_offset':base+ramoffset,'payload_offset':offset})
            offset+=length
        if offset!=end:raise ValueError('module has unaccounted bytes')
        modules.append({'id':modid,'slot':slot,'entry':entry,'persistent':persistent,'scratch':scratch,'blocks':items})
    if offset!=len(data):raise ValueError('image has unaccounted bytes')
    return {'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data),'format':fmt,'module_count':count,'modules':modules,'hardware_validated':False}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('image',type=Path);a=p.parse_args()
    try:
        fd=os.open(a.image,os.O_RDONLY|os.O_NONBLOCK|os.O_CLOEXEC)
        with os.fdopen(fd,'rb') as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):raise ValueError('image must be a regular file')
            result=inspect(stream.read(MAX_FILE+1))
    except ValueError as error:p.error(str(error))
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
