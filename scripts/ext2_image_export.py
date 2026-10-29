#!/usr/bin/env python3
"""Read-only ext2 namespace preflight for immutable image packing; no mounts.

Uses libext2fs to read a preserved, clean regular-file image. No library write
flags or filesystem mutation API is used. Source ownership and inode identity
are retained in the inventory, independent of the host user's permissions.
"""
import argparse
import ctypes as C
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import stat
import struct


CALLBACK = C.CFUNCTYPE(C.c_int, C.c_uint32, C.c_int, C.c_void_p,
                      C.c_int, C.c_int, C.c_void_p, C.c_void_p)


def require(condition, message):
    if not condition:
        raise ValueError(message)


class Source:
    def __init__(self, image, library=None):
        self.fd = os.open(image, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        self.fs = C.c_void_p()
        try:
            self.initial = os.fstat(self.fd)
            require(stat.S_ISREG(self.initial.st_mode), 'source must be a regular file')
            header = os.pread(self.fd, 1024, 1024)
            require(len(header) == 1024 and struct.unpack_from('<H', header, 56)[0] == 0xef53,
                    'not an ext2 superblock')
            require(struct.unpack_from('<H', header, 58)[0] & 1, 'source is not clean; use a checked stopped copy')
            require(struct.unpack_from('<I', header, 72)[0] == 0,
                    'only Linux-created ext2 inode ownership layouts are supported')
            compat, incompat, rocompat = struct.unpack_from('<III', header, 92)
            require(not compat & ~0x38 and not incompat & ~2 and not rocompat & ~3,
                    'unsupported ext2 features (journal/extents/64-bit/inline data not supported by this exporter)')
            log_block = struct.unpack_from('<I', header, 24)[0]
            require(log_block <= 6, 'invalid block size')
            block_size = 1024 << log_block
            blocks = struct.unpack_from('<I', header, 4)[0]
            require(blocks * block_size <= self.initial.st_size, 'truncated source filesystem')
            self.inode_bytes = struct.unpack_from('<H',header,88)[0] if struct.unpack_from('<I',header,76)[0] else 128
            require(128 <= self.inode_bytes <= block_size and not self.inode_bytes & (self.inode_bytes-1), 'invalid inode size')
            self.features = dict(compat=compat, incompat=incompat, rocompat=rocompat,
                                 block_bytes=block_size, filesystem_bytes=blocks*block_size)
            self.lib = C.CDLL(library or os.environ.get('BROOK_EXT2_LIBRARY') or
                              ctypes.util.find_library('ext2fs') or 'libext2fs.so.2')
            self.signature('ext2fs_open', [C.c_char_p,C.c_int,C.c_int,C.c_uint,C.c_void_p,C.POINTER(C.c_void_p)])
            self.signature('ext2fs_namei', [C.c_void_p,C.c_uint32,C.c_uint32,C.c_char_p,C.POINTER(C.c_uint32)])
            self.signature('ext2fs_read_inode_full', [C.c_void_p,C.c_uint32,C.c_void_p,C.c_int])
            self.signature('ext2fs_dir_iterate2', [C.c_void_p,C.c_uint32,C.c_int,C.c_void_p,CALLBACK,C.c_void_p])
            self.signature('ext2fs_file_open', [C.c_void_p,C.c_uint32,C.c_uint,C.POINTER(C.c_void_p)])
            self.signature('ext2fs_file_read', [C.c_void_p,C.c_void_p,C.c_uint,C.POINTER(C.c_uint)])
            self.signature('ext2fs_file_close', [C.c_void_p])
            self.lib.ext2fs_free.argtypes = [C.c_void_p]
            self.lib.ext2fs_free.restype = None
            manager = C.c_void_p.in_dll(self.lib, 'unix_io_manager')
            # Open the already pinned inode through procfs, eliminating pathname
            # replacement between our no-follow open and libext2fs's open.
            self.check(self.lib.ext2fs_open(os.fsencode('/proc/self/fd/'+str(self.fd)),
                       0,0,0,manager,C.byref(self.fs)), 'open ext2 read-only')
        except BaseException:
            self.close()
            raise

    def signature(self, name, args):
        fn = getattr(self.lib, name)
        fn.argtypes, fn.restype = args, C.c_long

    @staticmethod
    def check(code, operation):
        if code:
            raise OSError(f'libext2fs {operation} failed: {code}')

    def close(self):
        if self.fs.value:
            self.lib.ext2fs_free(self.fs)
            self.fs = C.c_void_p()
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def unchanged(self):
        now = os.fstat(self.fd)
        require((now.st_dev,now.st_ino,now.st_size,now.st_mtime_ns,now.st_ctime_ns) ==
                (self.initial.st_dev,self.initial.st_ino,self.initial.st_size,
                 self.initial.st_mtime_ns,self.initial.st_ctime_ns), 'source changed during traversal')

    def resolve(self, path):
        require(path.startswith('/') and '\0' not in path, 'source directory must be absolute and NUL-free')
        ino = C.c_uint32()
        self.check(self.lib.ext2fs_namei(self.fs,2,2,os.fsencode(path),C.byref(ino)), 'resolve directory')
        return ino.value

    def inode(self, ino):
        raw = C.create_string_buffer(self.inode_bytes)
        self.check(self.lib.ext2fs_read_inode_full(self.fs,ino,raw,self.inode_bytes), 'read inode '+str(ino))
        data = raw.raw
        mode, uid = struct.unpack_from('<HH',data)
        size, atime, ctime, mtime = struct.unpack_from('<IIII',data,4)
        gid, links = struct.unpack_from('<HH',data,24)
        acl = struct.unpack_from('<I',data,104)[0]
        require(not acl and not struct.unpack_from('<H',data,118)[0], 'extended inode attributes cannot be represented in BRKPACK1')
        flags = struct.unpack_from('<I',data,32)[0]
        require(not flags & ~0x1030, 'unsupported inode flags')
        if self.inode_bytes > 128:
            extra = struct.unpack_from('<H',data,128)[0]
            require(extra <= self.inode_bytes-128 and extra % 4 == 0, 'invalid inode extra size')
            if extra >= 12:
                require(not struct.unpack_from('<I',data,136)[0],
                        'extended mtime epoch/nanoseconds cannot be represented in BRKPACK1')
            at = 128+extra
            if at+4 <= self.inode_bytes:
                require(not struct.unpack_from('<I',data,at)[0], 'inline inode attributes cannot be represented in BRKPACK1')
        if stat.S_ISREG(mode):
            size |= struct.unpack_from('<I',data,108)[0] << 32
        uid |= struct.unpack_from('<H',data,120)[0] << 16
        gid |= struct.unpack_from('<H',data,122)[0] << 16
        require(links and (stat.S_ISREG(mode) or stat.S_ISDIR(mode) or stat.S_ISLNK(mode)),
                'unsupported or unlinked inode '+str(ino))
        return dict(ino=ino,mode=mode,uid=uid,gid=gid,size=size,atime=atime,
                    ctime=ctime,mtime=mtime,links=links), data

    def chunks(self, ino, size):
        handle = C.c_void_p()
        self.check(self.lib.ext2fs_file_open(self.fs,ino,0,C.byref(handle)), 'open inode content')
        try:
            buffer = C.create_string_buffer(65536)
            remaining = size
            while remaining:
                wanted = min(remaining,len(buffer));got = C.c_uint()
                self.check(self.lib.ext2fs_file_read(handle,buffer,wanted,C.byref(got)), 'read inode content')
                require(got.value == wanted, 'short inode read')
                yield buffer.raw[:wanted]
                remaining -= wanted
        finally:
            self.check(self.lib.ext2fs_file_close(handle), 'close inode content')

    def symlink(self, meta, raw):
        size = meta['size']
        require(0 < size <= 4095, 'invalid symlink size')
        if size <= 60 and struct.unpack_from('<I',raw,28)[0] == 0:
            target = raw[40:40+size]
        else:
            target = b''.join(self.chunks(meta['ino'],size))
        require(b'\0' not in target, 'invalid symlink text')
        return target

    def children(self, ino):
        result, errors, names = [], [], set()
        @CALLBACK
        def visit(_dir,_entry,entry,_offset,_block_size,_buffer,_private):
            try:
                header = C.string_at(entry,8)
                child, _length, length = struct.unpack('<IHH',header)
                length &= 255
                if not child:
                    return 0
                name = C.string_at(entry+8,length)
                if name in (b'.',b'..'):
                    return 0
                require(name and b'/' not in name and b'\0' not in name and name not in names,
                        'unsafe or duplicate directory entry')
                names.add(name);result.append((name,child))
                return 0
            except BaseException as error:
                errors.append(error)
                return 2  # DIRENT_ABORT; never unwind a Python exception through C
        self.check(self.lib.ext2fs_dir_iterate2(self.fs,ino,0,None,visit,None), 'iterate directory')
        if errors:
            raise errors[0]
        return sorted(result)

    def inventory(self, directory):
        nodes, directories, files = {}, set(), set()
        entries = strings = blocks = unique_bytes = builder_nodes = 0
        def walk(ino, path, depth):
            nonlocal entries, strings, blocks, unique_bytes, builder_nodes
            require(depth <= 128 and len(path)+1 <= 4095, 'namespace depth/path exceeds image limits')
            meta, raw = self.inode(ino)
            nodes[os.fsdecode(path)] = meta
            mode = meta['mode']
            if stat.S_ISREG(mode) and ino in files:
                return
            builder_nodes += 1
            if stat.S_ISDIR(mode):
                require(ino not in directories, 'directory cycle or directory hardlink')
                directories.add(ino)
                for name, child in self.children(ino):
                    entries += 1;strings += len(name)
                    walk(child,path+b'/'+name if path else name,depth+1)
            elif stat.S_ISREG(mode):
                files.add(ino);blocks += (meta['size']+65535)//65536;unique_bytes += meta['size']
            else:
                target = self.symlink(meta,raw);strings += len(target)
                meta['target'] = os.fsdecode(target)
        root = self.resolve(directory)
        require(stat.S_ISDIR(self.inode(root)[0]['mode']), 'source root is not a directory')
        walk(root,b'',0)
        index = (160+builder_nodes*64+entries*16+blocks*32+strings+4095)&~4095
        self.unchanged()
        return dict(version=1,source_directory=directory,source_features=self.features,
                    source_stat=dict(device=self.initial.st_dev,inode=self.initial.st_ino,
                                     bytes=self.initial.st_size,mtime_ns=self.initial.st_mtime_ns),
                    summary=dict(paths=len(nodes),builder_inodes=builder_nodes,directory_entries=entries,
                                 block_count=blocks,string_bytes=strings,index_bytes=index,
                                 unique_regular_files=len(files),unique_regular_bytes=unique_bytes),nodes=nodes)


def parent_fd(root, path):
    parts = path.split(b'/')
    require(parts and all(p and p not in (b'.',b'..') and b'\0' not in p for p in parts), 'unsafe export path')
    fd = os.dup(root)
    try:
        for part in parts[:-1]:
            following = os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW,dir_fd=fd)
            os.close(fd);fd = following
        return fd,parts[-1]
    except BaseException:
        os.close(fd)
        raise


def export(source, report, destination, manifest):
    """Extract once per source inode; source owners/modes remain in a manifest.

    Host files stay private and readable without chown. The packer must consume
    this manifest to restore source metadata and verify every regular-file hash.
    """
    destination, manifest = Path(destination), Path(manifest)
    require(not destination.exists() and not destination.is_symlink(), 'export directory already exists')
    require(not manifest.exists() and not manifest.is_symlink(), 'manifest already exists')
    require(not manifest.resolve().is_relative_to(destination.resolve()), 'manifest must be outside the exported namespace')
    require(report['source_stat']['device']==source.initial.st_dev and
            report['source_stat']['inode']==source.initial.st_ino and
            report['source_stat']['bytes']==source.initial.st_size and
            report['source_stat']['mtime_ns']==source.initial.st_mtime_ns,
            'inventory does not describe this source image')
    source.unchanged()
    nodes = report['nodes']
    require('' in nodes and stat.S_ISDIR(nodes['']['mode']), 'missing root metadata')
    os.mkdir(destination,0o700)
    root = os.open(destination,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    created = {}
    try:
        for text, meta in sorted(nodes.items(),key=lambda item:(item[0].count('/'),item[0])):
            if not text:
                continue
            path = os.fsencode(text)
            parent,leaf = parent_fd(root,path)
            try:
                mode,ino = meta['mode'],meta['ino']
                if stat.S_ISDIR(mode):
                    os.mkdir(leaf,0o700,dir_fd=parent)
                elif stat.S_ISLNK(mode):
                    target = os.fsencode(meta['target'])
                    require(b'\0' not in target, 'invalid symlink target')
                    os.symlink(target,leaf,dir_fd=parent)
                elif stat.S_ISREG(mode):
                    if ino in created:
                        old,digest = created[ino]
                        source_parent,source_leaf = parent_fd(root,old)
                        try:
                            os.link(source_leaf,leaf,src_dir_fd=source_parent,dst_dir_fd=parent,follow_symlinks=False)
                        finally:
                            os.close(source_parent)
                        meta['sha256'] = digest
                    else:
                        fd = os.open(leaf,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600,dir_fd=parent)
                        digest = hashlib.sha256()
                        with os.fdopen(fd,'wb') as out:
                            for chunk in source.chunks(ino,meta['size']):
                                digest.update(chunk)
                                if not any(chunk):
                                    out.seek(len(chunk),1)
                                else:
                                    require(out.write(chunk)==len(chunk), 'short export write')
                            out.truncate(meta['size'])
                        meta['sha256'] = digest.hexdigest()
                        created[ino] = (path,meta['sha256'])
                else:
                    raise ValueError('unsupported export inode')
            finally:
                os.close(parent)
        source.unchanged()
        report['export_format'] = 'brook-ext2-source-metadata-v1'
        with manifest.open('x') as out:
            json.dump(report,out,indent=2);out.flush();os.fsync(out.fileno())
    finally:
        os.close(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path)
    parser.add_argument('--directory',default='/store')
    parser.add_argument('--library')
    parser.add_argument('--output',type=Path,required=True,help='New inventory or exported metadata manifest')
    parser.add_argument('--extract',type=Path,help='New private export directory; preserves hardlinks without privileged chown')
    args = parser.parse_args()
    require(not args.output.exists(), 'refusing to replace inventory')
    source = Source(args.image,args.library)
    try:
        report = source.inventory(args.directory)
        if args.extract:
            export(source,report,args.extract,args.output)
        else:
            with args.output.open('x') as out:
                json.dump(report,out,indent=2)
        print(json.dumps(report['summary'],indent=2))
    finally:
        source.close()


if __name__ == '__main__':
    main()
