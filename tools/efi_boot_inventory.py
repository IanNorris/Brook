#!/usr/bin/env python3
"""Decode a BRKBOOTV1 firmware-variable inventory; never changes firmware."""
import argparse
import json
from pathlib import Path
import re
import struct


def parse_option(data):
    if len(data)<6:raise ValueError('short load option')
    attributes,path_bytes=struct.unpack_from('<IH',data)
    at=6;end=None
    while at+1<len(data):
        if data[at:at+2]==b'\0\0':end=at;break
        at+=2
    if end is None:raise ValueError('unterminated description')
    description=data[6:end].decode('utf-16-le',errors='strict')
    at=end+2
    if path_bytes>len(data)-at:raise ValueError('device path exceeds load option')
    limit=at+path_bytes;nodes=[];files=[];partitions=[];instances=[];ended=False;path_index=0
    while at<limit:
        if limit-at<4:raise ValueError('short device path header')
        typ,subtype,size=struct.unpack_from('<BBH',data,at)
        if size<4 or size>limit-at:raise ValueError('invalid device path node')
        if typ==0x7f and subtype not in (0x01,0xff):raise ValueError('unsupported device path end node')
        node=data[at:at+size];nodes.append({'type':typ,'subtype':subtype})
        if typ==4 and subtype==1:
            if size!=42:raise ValueError('invalid hard drive node')
            partitions.append({'number':struct.unpack_from('<I',node,4)[0],
                               'signature':node[24:40].hex(), 'signature_type':node[41]})
        if typ==4 and subtype==4:
            text=node[4:]
            if len(text)<2 or len(text)%2 or text[-2:]!=b'\0\0':raise ValueError('invalid file path node')
            path=text[:-2].decode('utf-16-le',errors='strict')
            if '\x00' in path:raise ValueError('embedded NUL in file path')
            files.append(path)
        at+=size
        if typ==0x7f:
            if size!=4:raise ValueError('invalid end node')
            if subtype==0x01 and (len(nodes)==1 or at==limit):raise ValueError('empty device path instance')
            if subtype==0xff and instances and len(nodes)==1:raise ValueError('empty device path instance')
            instances.append({'device_path_index':path_index,'nodes':nodes,'partitions':partitions,'files':files})
            nodes=[];partitions=[];files=[]
            if subtype==0xff:
                # EFI_LOAD_OPTION FilePathList may contain more than one
                # complete device path, each terminated by End Entire.
                path_index+=1
                ended=at==limit
    if not ended:raise ValueError('device path lacks end node')
    return {'active':bool(attributes&1),'attributes':attributes,'description':description,
            'instances':instances,
            'optional_data_bytes':len(data)-limit}


def parse_inventory(data):
    if len(data)>128*1024:raise ValueError('inventory exceeds limit')
    lines=data.decode('ascii',errors='strict').splitlines()
    if len(lines)<2 or lines[0]!='BRKBOOTV1' or lines[-1]!='END':raise ValueError('incomplete inventory')
    values={}
    for line in lines[1:-1]:
        name,sep,encoded=line.partition('=')
        if not sep or name in values or not re.fullmatch(r'Boot(?:[0-9A-F]{4}|Order|Current|Next)',name):
            raise ValueError('invalid or duplicate variable')
        if not re.fullmatch(r'(?:[0-9a-f]{2})*',encoded) or len(encoded)>8192:
            raise ValueError('invalid variable encoding')
        values[name]=bytes.fromhex(encoded)
    def number(name):
        if name not in values:return None
        if len(values[name])!=2:raise ValueError('invalid '+name)
        return int.from_bytes(values[name],'little')
    order=values.get('BootOrder')
    if order is None or not order or len(order)%2:raise ValueError('invalid or absent BootOrder')
    order=list(struct.unpack('<'+'H'*(len(order)//2),order))
    if len(set(order))!=len(order):raise ValueError('duplicate BootOrder entry')
    options={};errors={}
    for name,value in values.items():
        if not re.fullmatch(r'Boot[0-9A-F]{4}',name):continue
        try:options[name]=parse_option(value)
        except (ValueError,UnicodeError) as exc:errors[name]=str(exc)
    missing=[f'Boot{x:04X}' for x in order if f'Boot{x:04X}' not in options]
    current=number('BootCurrent');next_boot=number('BootNext')
    referenced=order+[x for x in (current,next_boot) if x is not None]
    missing_referenced=list(dict.fromkeys(f'Boot{x:04X}' for x in referenced if f'Boot{x:04X}' not in options))
    return {'boot_order':[f'Boot{x:04X}' for x in order],
            'boot_current':f'Boot{current:04X}' if current is not None else None,
            'boot_next':f'Boot{next_boot:04X}' if next_boot is not None else None,
            'options':options,'unparsed':errors,'missing_or_unparsed_order_entries':missing,
            'missing_or_unparsed_referenced_entries':missing_referenced}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('inventory',type=Path)
    args=p.parse_args();print(json.dumps(parse_inventory(args.inventory.read_bytes()),indent=2))


if __name__=='__main__':main()
