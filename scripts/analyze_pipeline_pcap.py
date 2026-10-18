#!/usr/bin/env python3
"""Summarize the controlled cache flow in a package-pipeline QEMU PCAP.

No packet payload is printed. Reports data gaps and intervening advertised
windows, not a causal diagnosis. TCP sequence numbers are raw 32-bit values.
"""
import argparse
import collections
import json
from pathlib import Path
import struct
from urllib.parse import urlparse


def packets(path):
    with path.open('rb') as stream:
        header = stream.read(24)
        formats = {b'\xd4\xc3\xb2\xa1': ('<', 1e6), b'\xa1\xb2\xc3\xd4': ('>', 1e6),
                   b'\x4d\x3c\xb2\xa1': ('<', 1e9), b'\xa1\xb2\x3c\x4d': ('>', 1e9)}
        endian, precision = formats[header[:4]]
        assert struct.unpack(endian+'I', header[20:24])[0] == 1, 'Ethernet PCAP required'
        while record := stream.read(16):
            sec, fraction, length, _ = struct.unpack(endian+'IIII', record)
            raw = stream.read(length)
            assert len(raw) == length
            if len(raw)<34 or raw[12:14] != b'\x08\x00':continue
            offset = 14+(raw[14]&15)*4
            if raw[23]!=6 or len(raw)<offset+20:continue
            total = struct.unpack('!H',raw[16:18])[0]
            ip_length = (raw[14]&15)*4
            src, dst, seq, ack = struct.unpack('!HHII',raw[offset:offset+12])
            tcp_length=(raw[offset+12]>>4)*4
            yield {'time':sec+fraction/precision,'source':src,'destination':dst,
                   'seq':seq,'ack':ack,'flags':raw[offset+13],
                   'window':struct.unpack('!H',raw[offset+14:offset+16])[0],
                   'data_bytes':total-ip_length-tcp_length}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('run',type=Path)
    args=ap.parse_args()
    evidence=json.loads((args.run/'command-evidence.json').read_text())
    port=urlparse(evidence['request']['argv'][-2]).port
    flows=collections.defaultdict(list)
    for p in packets(args.run/'network.pcap'):
        if p['source']==port:p['direction']='host';key=p['destination']
        elif p['destination']==port:p['direction']='guest';key=p['source']
        else:continue
        flows[key].append(p)
    report=[]
    for key, events in sorted(flows.items(),key=lambda item:item[1][0]['time']):
        incoming=[p for p in events if p['direction']=='host' and p['data_bytes']>0]
        if not incoming:continue
        start=events[0]['time']
        gaps=[]
        for previous, current in zip(incoming,incoming[1:]):
            if current['time']-previous['time']<.05:continue
            acks=[dict(t=round(p['time']-start,6),ack=p['ack'],window_raw=p['window'])
                for p in events if p['direction']=='guest' and previous['time']<=p['time']<=current['time']]
            gaps.append({'seconds':round(current['time']-previous['time'],6),
                'before_time':round(previous['time']-start,6),
                'next_time':round(current['time']-start,6),
                'previous_seq':previous['seq'],'previous_bytes':previous['data_bytes'],
                'next_seq':current['seq'],'acks_between':acks[-8:]})
        entry={'guest_port':key,'wire_data_bytes_including_retransmits':sum(p['data_bytes'] for p in incoming),
               'seconds_first_to_last_data':round(incoming[-1]['time']-incoming[0]['time'],6),
               'zero_window_ack_count':sum(p['direction']=='guest' and p['window']==0 for p in events),
               'data_gap_count':len(gaps),'gap_seconds_sum':round(sum(g['seconds'] for g in gaps),6),
               'largest_gaps':sorted(gaps,key=lambda g:g['seconds'],reverse=True)[:8]}
        report.append(entry)
    result={'cache_port':port,'note':'Window values are raw: apply negotiated TCP scale before comparing byte counts. Gaps include wire idle, scheduling and capture effects; no causal claim.', 'flows':report}
    (args.run/'packet-summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
