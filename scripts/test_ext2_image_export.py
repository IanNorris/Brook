import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import compressed_image as ci
import ext2_image_export as ex

LIBRARY = os.environ.get('BROOK_EXT2_LIBRARY')


class Ext2Export(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory()
        cls.base = Path(cls.fixture.name)
        source = cls.base/'input';(source/'d').mkdir(parents=True)
        cls.content = bytes(range(256))*400 + b'end'
        (source/'d/data').write_bytes(cls.content)
        os.link(source/'d/data',source/'alias')
        (source/'empty').touch()
        (source/'short').symlink_to('d/data')
        (source/'long').symlink_to('/nix/store/'+ 'a'*120 + '/bin/application')
        # BRKPACK1 stores whole seconds; supported fixtures must not silently
        # import host nanosecond timestamps through mke2fs -d.
        for path in [source,*source.rglob('*')]:
            os.utime(path,ns=(1000000000,1000000000),follow_symlinks=False)
        cls.image = cls.base/'source.img'
        with cls.image.open('wb') as stream:stream.truncate(16*1024*1024)
        subprocess.run(['mke2fs','-q','-t','ext2','-F','-b','4096','-d',str(source),str(cls.image)],check=True,capture_output=True)
        for field,value in [('uid',12345),('gid',45678),('mode','0100555'),('mtime','20000101000000')]:
            subprocess.run(['debugfs','-w','-R',f'set_inode_field /d/data {field} {value}',str(cls.image)],check=True,capture_output=True)
        cls.before = hashlib.sha256(cls.image.read_bytes()).hexdigest()

    @classmethod
    def tearDownClass(cls):
        assert hashlib.sha256(cls.image.read_bytes()).hexdigest() == cls.before
        cls.fixture.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory();self.base = Path(self.tmp.name)
        self.source = ex.Source(self.image,LIBRARY)
        self.report = self.source.inventory('/')

    def tearDown(self):
        self.source.close();self.tmp.cleanup()

    def export(self):
        tree,manifest = self.base/'tree',self.base/'manifest.json'
        ex.export(self.source,self.report,tree,manifest)
        return tree,json.loads(manifest.read_text())

    def test_roundtrip_metadata_links_content_and_exact_preflight(self):
        tree,manifest = self.export()
        self.assertEqual((tree/'alias').stat().st_ino,(tree/'d/data').stat().st_ino)
        self.assertEqual((tree/'d/data').read_bytes(),self.content)
        self.assertEqual(os.readlink(tree/'short'),'d/data')
        self.assertEqual(os.readlink(tree/'long'),self.report['nodes']['long']['target'])
        packed = self.base/'raw.brk'
        ci.build(tree,packed,compress=False,metadata=manifest)
        image = ci.Image(packed)
        try:
            image.verify();paths=dict(image.paths());node=image.nodes[paths[b'd/data']]
            self.assertEqual(paths[b'alias'],paths[b'd/data'])
            self.assertEqual((node.mode,node.uid,node.gid,node.mtime),(0o100555,12345,45678,self.report['nodes']['d/data']['mtime']))
            self.assertEqual(image.read(paths[b'alias'],0,len(self.content)),self.content)
            self.assertEqual(image.payload_at,self.report['summary']['index_bytes'])
            self.assertEqual(len(image.nodes),self.report['summary']['builder_inodes'])
        finally:image.close()
        with self.assertRaises(ValueError):ex.export(self.source,self.report,tree,self.base/'second.json')

    def test_modified_bytes_and_symlinks_fail_closed(self):
        tree,manifest = self.export()
        with (tree/'d/data').open('r+b') as stream:stream.write(b'BAD')
        with self.assertRaisesRegex(ValueError,'digest'):ci.build(tree,self.base/'bad.brk',compress=False,metadata=manifest)
        (tree/'d/data').write_bytes(self.content)
        (tree/'short').unlink();(tree/'short').symlink_to('d/xxxx')
        with self.assertRaisesRegex(ValueError,'symlink'):ci.build(tree,self.base/'bad-link.brk',compress=False,metadata=manifest)
        self.assertFalse((self.base/'bad.brk').exists())

    def test_hardlink_topology_and_inconsistent_owner_rejected(self):
        tree,manifest = self.export()
        wrong=copy.deepcopy(manifest);wrong['nodes']['alias']['uid']=23
        with self.assertRaisesRegex(ValueError,'hardlink metadata'):ci.build(tree,self.base/'owner.brk',compress=False,metadata=wrong)
        (tree/'alias').unlink();(tree/'alias').write_bytes(self.content)
        with self.assertRaisesRegex(ValueError,'hardlink topology'):ci.build(tree,self.base/'topology.brk',compress=False,metadata=manifest)

    def test_manifest_requires_complete_namespace_and_valid_fields(self):
        tree,manifest = self.export()
        for mutation in ['missing','extra','negative']:
            wrong=copy.deepcopy(manifest)
            if mutation=='missing':del wrong['nodes']['empty']
            elif mutation=='extra':wrong['nodes']['not-present']=wrong['nodes']['empty']
            else:wrong['nodes']['']['mode']=-1
            with self.assertRaises(ValueError):ci.build(tree,self.base/(mutation+'.brk'),compress=False,metadata=wrong)

    def test_source_mutation_dirty_features_and_symlink_source_rejected(self):
        altered=self.base/'altered.img';shutil.copyfile(self.image,altered)
        source=ex.Source(altered,LIBRARY)
        try:
            report=source.inventory('/')
            with altered.open('r+b') as stream:stream.seek(2048);stream.write(b'changed')
            with self.assertRaisesRegex(ValueError,'source changed'):ex.export(source,report,self.base/'bad-export',self.base/'bad.json')
            self.assertFalse((self.base/'bad-export').exists())
        finally:source.close()
        for name,offset,value in [('dirty',58,struct.pack('<H',0)),('errors',58,struct.pack('<H',3)),('features',96,struct.pack('<I',0x40)),('creator',72,struct.pack('<I',1))]:
            image=self.base/(name+'.img');shutil.copyfile(self.image,image)
            with image.open('r+b') as stream:stream.seek(1024+offset);stream.write(value)
            with self.assertRaises(ValueError):ex.Source(image,LIBRARY)
        link=self.base/'link.img';link.symlink_to(self.image)
        with self.assertRaises(OSError):ex.Source(link,LIBRARY)

    def test_extended_attributes_are_not_silently_omitted(self):
        image=self.base/'attributes.img';shutil.copyfile(self.image,image)
        subprocess.run(['debugfs','-w','-R','ea_set /d/data user.test value',str(image)],check=True,capture_output=True)
        source=ex.Source(image,LIBRARY)
        try:
            with self.assertRaisesRegex(ValueError,'attributes'):source.inventory('/')
        finally:source.close()

    def test_extended_mtime_is_not_silently_truncated(self):
        image=self.base/'timestamp.img';shutil.copyfile(self.image,image)
        subprocess.run(['debugfs','-w','-R','set_inode_field /d/data mtime_extra 4',str(image)],check=True,capture_output=True)
        source=ex.Source(image,LIBRARY)
        try:
            with self.assertRaisesRegex(ValueError,'extended mtime'):source.inventory('/')
        finally:source.close()

    def test_export_path_and_manifest_scope_rejected(self):
        for path in [b'../escape',b'/escape',b'd/../../escape',b'd//escape']:
            root=os.open(self.base,os.O_RDONLY|os.O_DIRECTORY)
            try:
                with self.assertRaises(ValueError):ex.parent_fd(root,path)
            finally:os.close(root)
        with self.assertRaises(ValueError):ex.export(self.source,self.report,self.base/'tree',self.base/'tree/manifest.json')


if __name__=='__main__':unittest.main()
