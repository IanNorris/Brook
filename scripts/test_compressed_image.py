#!/usr/bin/env python3
"""Host tools and differential Python/C++ BRKPACK1 validation."""
import json
import os
from pathlib import Path
import random
import stat
import subprocess
import tarfile
import tempfile
import unittest
import compressed_image as ci

LIBRARY = os.environ.get('BROOK_IMAGE_TEST_LZ4_LIBRARY')
CPP = Path(os.environ.get('BROOK_IMAGE_TEST_CPP', 'build/release/host_tests/test_compressed_image')).resolve()

class Images(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.source = self.base/'source';self.source.mkdir()
        (self.source/'a').mkdir();(self.source/'a'/'b').mkdir()
        self.content = bytes(range(256))*600 + b'final'
        (self.source/'a'/'b'/'data').write_bytes(self.content)
        (self.source/'empty').write_bytes(b'')
        os.link(self.source/'a'/'b'/'data', self.source/'alias')
        (self.source/'relative').symlink_to('a/b/data')
        (self.source/'escape').symlink_to(str(self.base/'outside'))
        self.outside = self.base/'outside';self.outside.mkdir();(self.outside/'sentinel').write_bytes(b'unchanged')
        (self.source/'executable').write_bytes(b'ELF-ish')
        (self.source/'executable').chmod(0o4755)
        self.path = self.base/'image.brk'
        ci.build(self.source, self.path, LIBRARY)

    def tearDown(self):
        self.tmp.cleanup()

    def image(self):
        return ci.Image(self.path, LIBRARY)

    def cpp(self, path):
        self.assertTrue(CPP.exists(), 'build test_compressed_image before running host tool tests')
        return subprocess.run([str(CPP),str(path)], capture_output=True, text=True)

    def rehash(self, data):
        h = list(ci.HEADER.unpack_from(data));h[17] = ci.checksum(data[ci.HEADER.size:h[15]])
        h[18] = 0;data[:ci.HEADER.size] = ci.HEADER.pack(*h)
        h[18] = ci.checksum(data[:136]);data[:ci.HEADER.size] = ci.HEADER.pack(*h)
        return data

    def both(self, data, expected):
        path = self.base/'mutation.brk';path.write_bytes(data)
        python = True
        try:
            image = ci.Image(path, LIBRARY)
            try:
                image.verify()
            finally:
                image.close()
        except (ValueError,OSError):
            python = False
        result = self.cpp(path)
        self.assertEqual(python, result.returncode == 0, result.stderr)
        self.assertEqual(python, expected)

    def test_roundtrip_determinism_hardlinks_symlinks_tar(self):
        second = self.base/'second.brk';ci.build(self.source,second,LIBRARY)
        self.assertEqual(second.read_bytes(),self.path.read_bytes())
        image = self.image()
        try:
            image.verify();self.assertGreater(image.inspect()['compressed_blocks'],0)
            result = self.cpp(self.path);self.assertEqual(result.returncode,0,result.stderr)
            expected = []
            for inode,node in enumerate(image.nodes):
                if node.kind == ci.FILE:
                    expected.append(dict(inode=inode,size=node.size,checksum=ci.checksum(image.read(inode,0,node.size))))
            self.assertEqual(json.loads(result.stdout),expected)
            destination=self.base/'extracted';ci.extract(image,destination)
            self.assertEqual((destination/'a'/'b'/'data').read_bytes(),self.content)
            self.assertEqual((destination/'alias').stat().st_ino,(destination/'a'/'b'/'data').stat().st_ino)
            self.assertEqual(os.readlink(destination/'relative'),'a/b/data')
            self.assertEqual(os.readlink(destination/'escape'),str(self.outside))
            self.assertEqual((self.outside/'sentinel').read_bytes(),b'unchanged')
            self.assertEqual(stat.S_IMODE((destination/'executable').stat().st_mode),0o755)
            target=self.base/'plain.tar';ci.export_tar(image,target)
            with tarfile.open(target) as archive:
                self.assertEqual(archive.extractfile('a/b/data').read(),self.content)
                self.assertEqual(archive.getmember('escape').linkname,str(self.outside))
        finally:
            image.close()

    def test_no_overwrite_or_existing_destination(self):
        before=self.path.read_bytes()
        with self.assertRaises(ValueError):ci.build(self.source,self.path,LIBRARY)
        image=self.image()
        try:
            destination=self.base/'existing';destination.mkdir();(destination/'a').symlink_to(self.outside)
            with self.assertRaises(FileExistsError):ci.extract(image,destination)
            self.assertEqual((self.outside/'sentinel').read_bytes(),b'unchanged')
            with self.assertRaises(FileExistsError):ci.export_tar(image,self.path)
        finally:image.close()
        self.assertEqual(self.path.read_bytes(),before)

    def test_raw_empty_tree_and_special_file_rejection(self):
        raw=self.base/'raw.brk';ci.build(self.source,raw,compress=False)
        image=ci.Image(raw)
        try:self.assertEqual(image.verify()['compressed_blocks'],0)
        finally:image.close()
        self.assertEqual(self.cpp(raw).returncode,0)
        empty=self.base/'empty-dir';empty.mkdir();path=self.base/'empty.brk';ci.build(empty,path,compress=False)
        image=ci.Image(path)
        try:self.assertEqual(image.verify()['inodes'],1)
        finally:image.close()
        self.assertEqual(self.cpp(path).returncode,0)
        os.mkfifo(self.source/'fifo')
        with self.assertRaises(ValueError):ci.build(self.source,self.base/'fifo.brk',compress=False)

    def test_truncation_checksum_payload_and_reserved(self):
        data=self.path.read_bytes()
        for length in (0,159,160,4095,len(data)-1):self.both(data[:length],False)
        damaged=bytearray(data);damaged[10]^=1;self.both(damaged,False)
        damaged=bytearray(data);h=ci.HEADER.unpack_from(data);damaged[h[15]]^=0xff;self.both(damaged,False)
        damaged=bytearray(data);damaged[144]=1;self.both(self.rehash(damaged),False)
        image=self.image()
        try:final_at=image.blocks[-1][0]
        finally:image.close()
        damaged=bytearray(data);damaged[final_at]^=1;self.both(damaged,False)

    def test_structure_mutations_and_random_metadata(self):
        seed=self.path.read_bytes();h=ci.HEADER.unpack_from(seed)
        image=self.image()
        try:original_entries=image.entries[:];strings=image.strings
        finally:image.close()
        roots=[i for i,e in enumerate(original_entries) if e[0]==0]
        # Duplicate names and invalid parent/root-child references, after valid checksums.
        for mode in ('duplicate','file_parent','root_child','cycle','orphan','directory_hardlink'):
            data=bytearray(seed);entries=original_entries[:]
            e=list(entries[roots[1]])
            if mode=='duplicate':e[2:4]=entries[roots[0]][2:4]
            if mode=='file_parent':e[0]=next(x[1] for x in entries if strings[x[2]:x[2]+x[3]]==b'data')
            if mode=='root_child':e[1]=0
            if mode in ('orphan','directory_hardlink'):
                image=self.image()
                try:
                    empty=next(i for i,n in enumerate(image.nodes) if n.kind==ci.FILE and n.size==0)
                    target=next(i for i,n in enumerate(image.nodes) if n.kind==(ci.DIR if mode=='directory_hardlink' else ci.FILE) and i and i!=empty)
                finally:image.close()
                index=next(i for i,x in enumerate(entries) if x[1]==empty)
                e=list(entries[index]);e[1]=target;roots_index=index
            if mode=='cycle':
                a=next(x[1] for x in entries if strings[x[2]:x[2]+x[3]]==b'a')
                b=next(x[1] for x in entries if strings[x[2]:x[2]+x[3]]==b'b')
                index=next(i for i,x in enumerate(entries) if x[1]==a)
                e=list(entries[index]);e[0]=b;roots_index=index
            elif mode not in ('orphan','directory_hardlink'):roots_index=roots[1]
            entries[roots_index]=tuple(e)
            entries.sort(key=lambda x:(x[0],strings[x[2]:x[2]+x[3]]))
            for i,entry in enumerate(entries):ci.DIRENT.pack_into(data,h[9]+i*ci.DIRENT.size,*entry)
            self.both(self.rehash(data),False)
        # Differential rejection includes structure changes with valid hashes,
        # rather than only exercising the checksum gate.
        rng=random.Random(0x42524b)
        for unused in range(80):
            data=bytearray(seed);at=rng.randrange(ci.HEADER.size,h[15]);data[at]^=1<<rng.randrange(8)
            self.rehash(data)
            path=self.base/'mutation.brk';path.write_bytes(data)
            accepted=True
            try:
                image=ci.Image(path,LIBRARY)
                try:image.verify()
                finally:image.close()
            except (ValueError,OSError):accepted=False
            self.assertEqual(accepted,self.cpp(path).returncode==0)
        for field in (3,4,7,8,9,10,11,12,13,14,15):
            data=bytearray(seed);header=list(h);header[field]=(1<<64)-1 if field!=3 else 1
            header[18]=0;data[:ci.HEADER.size]=ci.HEADER.pack(*header);header[18]=ci.checksum(data[:136]);data[:ci.HEADER.size]=ci.HEADER.pack(*header)
            self.both(data,False)

    def test_selected_top_level_paths(self):
        subset=self.base/'subset.brk'
        ci.build(self.source,subset,LIBRARY,paths=[self.source/'a',self.source/'executable'])
        image=ci.Image(subset,LIBRARY)
        try:
            self.assertEqual([p for p,i in image.paths()],[b'',b'a',b'a/b',b'a/b/data',b'executable'])
            image.verify()
        finally:image.close()
        self.assertEqual(self.cpp(subset).returncode,0)
        with self.assertRaises(ValueError):ci.build(self.source,self.base/'bad-subset.brk',LIBRARY,paths=[self.outside])

    def test_maximum_depth_and_path_length(self):
        def tree(root,names,leaf):
            root.mkdir();fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY)
            try:
                for name in names:
                    os.mkdir(name,dir_fd=fd);nxt=os.open(name,os.O_RDONLY|os.O_DIRECTORY,dir_fd=fd);os.close(fd);fd=nxt
                out=os.open(leaf,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600,dir_fd=fd);os.write(out,b'end');os.close(out)
            finally:os.close(fd)
        for depth,valid in ((128,True),(129,False)):
            root=self.base/('depth'+str(depth));tree(root,[b'n']*depth,b'end');path=self.base/('depth'+str(depth)+'.brk')
            if valid:
                ci.build(root,path,compress=False);self.assertEqual(self.cpp(path).returncode,0)
                image=ci.Image(path);image.verify();image.close()
            else:
                with self.assertRaises(ValueError):ci.build(root,path,compress=False)
        for size,valid in ((254,True),(255,False)):
            root=self.base/('path'+str(size));tree(root,[b'n'*255]*15,b'x'*size);path=self.base/('path'+str(size)+'.brk')
            if valid:
                ci.build(root,path,compress=False);self.assertEqual(self.cpp(path).returncode,0)
                image=ci.Image(path);image.verify();image.close()
            else:
                with self.assertRaises(ValueError):ci.build(root,path,compress=False)

if __name__=='__main__':unittest.main()
