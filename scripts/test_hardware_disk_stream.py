#!/usr/bin/env python3
"""Exercise backup framing, identity and publication without physical disks."""
import hashlib,json,os,socket,stat,struct,sys,tempfile,threading,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from hardware_bulk_protocol import disk_download

class DiskStream(unittest.TestCase):
    def run_case(self,mode):
        key='1'*32;boot='2'*32;tx='3'*32;payload=bytes(range(256))*512
        identity={'sectors':len(payload)//512,'logical_sector_bytes':512,'physical_sector_bytes':4096,'port':0,'model':'QEMU DISK','serial':'TEST001'}
        digest=hashlib.sha256(payload).hexdigest();left,right=socket.socketpair();events=[];got=[]
        def read_exact(s,n):
            data=b''
            while len(data)<n:
                part=s.recv(n-len(data))
                if not part:return data
                data+=part
            return data
        def server():
            try:
                with right:
                    assert len(read_exact(right,104))==104
                    right.sendall(b'{"state":"ready","bulk_api":1}\n')
                    offer=struct.pack('<8sQIII41s21s32s32s',b'BRDISK01',identity['sectors'],512,4096,0,b'QEMU DISK',b'TEST001',boot.encode(),tx.encode())
                    right.sendall(offer)
                    go=read_exact(right,8);got.append(go)
                    if go!=b'READONLY':return
                    right.sendall(payload[:len(payload)//2] if mode=='truncated' else payload)
                    if mode=='truncated':return
                    receipt={'state':'completed','transaction_id':tx,'action':'disk','bytes':len(payload),'sha256':'0'*64 if mode=='bad_hash' else digest,'guest_path':None}
                    right.sendall(json.dumps(receipt).encode()+b'\n')
            except (BrokenPipeError,ConnectionResetError):pass
        worker=threading.Thread(target=server);worker.start()
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'disk.img'
            if mode=='collision':path.write_bytes(b'preserve')
            expected={**identity,**({'serial':'WRONG'} if mode=='wrong_id' else {})}
            try:
                real_fsync=os.fsync
                def fsync(fd):
                    is_dir=stat.S_ISDIR(os.fstat(fd).st_mode)
                    if (mode=='fsync_data' and not is_dir) or (mode=='fsync_dir' and is_dir):raise OSError('injected fsync failure')
                    return real_fsync(fd)
                with patch('hardware_bulk_protocol.socket.create_connection',return_value=left),patch('hardware_bulk_protocol.os.fsync',side_effect=fsync):
                    if mode in ('wrong_id','truncated','bad_hash','collision','fsync_data','fsync_dir'):
                        with self.assertRaises((ValueError,ConnectionError,OSError)):
                            disk_download('unused',1,key,boot,tx,path,expected,len(payload),lambda *x:events.append(x))
                    else:
                        result=disk_download('unused',1,key,boot,tx,path,expected,len(payload),lambda *x:events.append(x))
                        self.assertEqual(path.read_bytes(),payload);self.assertEqual(result['independent_sha256'],digest)
                        self.assertEqual([x[0] for x in events],['offer_validated','transferring','transferring','guest_receipt','verifying','verifying','published'])
                        self.assertEqual(events[2][1],{'bytes_completed':len(payload),'total_bytes':len(payload)})
                        self.assertEqual(events[5][1],{'bytes_completed':len(payload),'total_bytes':len(payload)})
                if mode=='collision':self.assertEqual(path.read_bytes(),b'preserve')
                elif mode=='fsync_dir':
                    self.assertEqual(path.read_bytes(),payload)
                    self.assertEqual(events[-1][0],'published_durability_uncertain')
                elif mode!='ok':self.assertFalse(path.exists())
                self.assertFalse(path.with_name('disk.img.part').exists())
            finally:left.close();worker.join(timeout=5)
            self.assertFalse(worker.is_alive())
            if mode=='wrong_id':self.assertEqual(got,[b''])
    def test_modes(self):
        for mode in ('ok','wrong_id','truncated','bad_hash','collision','fsync_data','fsync_dir'):
            with self.subTest(mode=mode):self.run_case(mode)
if __name__=='__main__':unittest.main()
