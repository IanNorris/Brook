#!/usr/bin/env python3
"""Reject malformed compressed chunks before publishing a download."""
import hashlib,json,struct,sys,tempfile,unittest,zlib
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from hardware_bulk_protocol import download

class Socket:
    def __init__(self,data):self.data=bytearray(data)
    def __enter__(self):return self
    def __exit__(self,*args):pass
    def settimeout(self,*args):pass
    def sendall(self,*args):pass
    def recv(self,n):
        n=min(n,7,len(self.data));result=bytes(self.data[:n]);del self.data[:n];return result

class TestCompressed(unittest.TestCase):
    def test_bad_chunks_never_publish(self):
        raw=b'A'*1000;packed=zlib.compress(raw);digest=hashlib.sha256(raw).hexdigest()
        meta={'state':'payload','bytes':len(raw),'sha256':digest}
        begin=b'{"state":"ready","bulk_api":1}\n'+json.dumps(meta).encode()+b'\n'
        expanded=zlib.compress(b'A'*1001)
        cases=[b'\x01\x02',
               struct.pack('<IIB',1000,len(packed),1)+packed[:-1],
               struct.pack('<IIB',999,len(packed),1)+packed,
               struct.pack('<IIB',1000,1001,0),
               struct.pack('<IIB',1000,len(packed),7)+packed,
               struct.pack('<IIB',1000,len(packed),1)+bytes(len(packed)),
               struct.pack('<IIB',1000,len(packed)-1,1)+packed[:-1],
               struct.pack('<IIB',1000,len(expanded),1)+expanded,
               struct.pack('<IIB',1000,len(packed)+1,1)+packed+b'x',
               struct.pack('<IIB',1000,len(packed)*2,1)+packed+packed,
               struct.pack('<IIB',1000,1000,0)+b'B'*1000]
        with tempfile.TemporaryDirectory() as tmp:
            for i,body in enumerate(cases):
                target=Path(tmp)/str(i)
                with self.subTest(case=i),patch('hardware_bulk_protocol.socket.create_connection',return_value=Socket(begin+body)):
                    with self.assertRaises((ValueError,zlib.error,ConnectionError)):
                        download('unused',1,'a'*32,'b'*32,'c'*32,target,'getz')
                    self.assertFalse(target.exists());self.assertFalse(target.with_name(target.name+'.part').exists())

    def test_bad_receipt_never_publishes(self):
        raw=b'A'*1000;packed=zlib.compress(raw);digest=hashlib.sha256(raw).hexdigest()
        meta={'state':'payload','bytes':len(raw),'sha256':digest}
        body=(b'{"state":"ready","bulk_api":1}\n'+json.dumps(meta).encode()+b'\n'+
              struct.pack('<IIB',len(raw),len(packed),1)+packed+b'{"state":"completed"}\n')
        with tempfile.TemporaryDirectory() as tmp:
            target=Path(tmp)/'out'
            with patch('hardware_bulk_protocol.socket.create_connection',return_value=Socket(body)):
                with self.assertRaises(ValueError):download('unused',1,'a'*32,'b'*32,'c'*32,target,'getz')
            self.assertFalse(target.exists());self.assertFalse(target.with_name(target.name+'.part').exists())

if __name__=='__main__':unittest.main()
