#!/usr/bin/env python3
"""Build/read immutable BRKPACK1 images. Extraction never follows image symlinks."""
import argparse
import ctypes
import ctypes.util
from dataclasses import dataclass
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import tarfile
import tempfile

HEADER = struct.Struct('<8sIIQQII9Q16sQQ16s')
INODE = struct.Struct('<IIIIQQQQIIq')
DIRENT = struct.Struct('<IIIHH')
BLOCK = struct.Struct('<QIIIIQ')
MAGIC, CHUNK, INDEX_LIMIT = b'BRKPACK1', 65536, 16 * 1024 * 1024
DIR, FILE, LINK = 1, 2, 3

def checksum(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value

def align(value):
    return (value + 4095) & ~4095

def require(condition, message):
    if not condition:
        raise ValueError(message)

class Codec:
    def __init__(self, path=None):
        self.lib = ctypes.CDLL(path or ctypes.util.find_library('lz4') or 'liblz4.so.1')
        self.lib.LZ4_versionNumber.restype = ctypes.c_int
        require(self.lib.LZ4_versionNumber() == 11000, 'LZ4 1.10.0 required for this builder/reader')
        self.lib.LZ4_compressBound.argtypes = [ctypes.c_int]
        self.lib.LZ4_compressBound.restype = ctypes.c_int
        self.lib.LZ4_compress_default.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        self.lib.LZ4_compress_default.restype = ctypes.c_int
        self.lib.LZ4_decompress_safe.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        self.lib.LZ4_decompress_safe.restype = ctypes.c_int

    def encode(self, data):
        out = ctypes.create_string_buffer(self.lib.LZ4_compressBound(len(data)))
        size = self.lib.LZ4_compress_default(data, out, len(data), len(out))
        require(size > 0, 'LZ4 encode failed')
        return out.raw[:size]

    def decode(self, data, size):
        out = ctypes.create_string_buffer(size)
        result = self.lib.LZ4_decompress_safe(data, out, len(data), size)
        require(result == size, 'invalid LZ4 block/decoded size')
        return out.raw

@dataclass(frozen=True)
class Node:
    kind: int
    mode: int
    uid: int
    gid: int
    size: int
    first: int
    count: int
    target: int
    target_size: int
    reserved: int
    mtime: int

class Image:
    def __init__(self, path, library=None):
        self.file = Path(path).open('rb')
        self.library, self.codec = library, None
        try:
            data = self.file.read(HEADER.size)
            require(len(data) == HEADER.size, 'short header')
            h = HEADER.unpack(data)
            (magic, version, header_size, features, image_size, block_size, root,
             inode_at, inode_count, entry_at, entry_count, block_at, block_count,
             strings_at, strings_size, payload_at, identity, meta_hash, header_hash, reserved) = h
            require(magic == MAGIC and version == 1 and header_size == HEADER.size and not features,
                    'unsupported header/features')
            require(block_size == CHUNK and root == 0 and 0 < inode_count <= 0xffffffff and entry_count <= 0xffffffff,
                    'invalid root/count/block size')
            require(4096 <= payload_at <= INDEX_LIMIT and payload_at % 4096 == 0 and
                    payload_at <= image_size <= min(os.fstat(self.file.fileno()).st_size, (1 << 63)-1) and image_size % 4096 == 0,
                    'invalid image/index bounds')
            require(inode_at == HEADER.size and entry_at == inode_at + inode_count * INODE.size and
                    block_at == entry_at + entry_count * DIRENT.size and strings_at == block_at + block_count * BLOCK.size and
                    strings_at + strings_size <= payload_at == align(strings_at + strings_size), 'noncanonical table layout')
            require(any(identity) and not any(reserved) and header_hash == checksum(data[:136]), 'header checksum/identity/reserved')
            index = self.file.read(payload_at - HEADER.size)
            require(len(index) == payload_at-HEADER.size and checksum(index) == meta_hash, 'metadata checksum/short index')
            require(not any(index[strings_at + strings_size - HEADER.size:]), 'nonzero index padding')
            self.nodes = [Node(*INODE.unpack_from(index, inode_at-HEADER.size+i*INODE.size)) for i in range(inode_count)]
            self.entries = [DIRENT.unpack_from(index, entry_at-HEADER.size+i*DIRENT.size) for i in range(entry_count)]
            self.blocks = [BLOCK.unpack_from(index, block_at-HEADER.size+i*BLOCK.size) for i in range(block_count)]
            self.strings = index[strings_at-HEADER.size:strings_at-HEADER.size+strings_size]
            self.image_size, self.payload_at, self.identity = image_size, payload_at, identity
            self.links = [0] * inode_count
            self.parents = [None] * inode_count
            next_block = 0
            for node in self.nodes:
                require(not node.reserved and node.mode & ~0o177777 == 0 and node.kind in (DIR, FILE, LINK), 'invalid inode')
                require(stat.S_IFMT(node.mode) == {DIR:stat.S_IFDIR, FILE:stat.S_IFREG, LINK:stat.S_IFLNK}[node.kind], 'inode type/mode mismatch')
                if node.kind == FILE:
                    require(node.first == next_block and node.count == (node.size // CHUNK + bool(node.size % CHUNK)) and
                            node.count <= block_count-next_block and not node.target and not node.target_size, 'invalid file extent')
                    for i in range(node.count):
                        expected = node.size % CHUNK if i+1 == node.count and node.size % CHUNK else CHUNK
                        require(self.blocks[next_block+i][2] == expected, 'invalid final block size')
                    next_block += node.count
                elif node.kind == LINK:
                    require(not node.first and not node.count and node.size == node.target_size, 'invalid symlink inode')
                    self.string(node.target, node.target_size, False)
                else:
                    require(not (node.size or node.first or node.count or node.target or node.target_size), 'directory data')
            require(self.nodes[0].kind == DIR and next_block == block_count, 'invalid root/unowned blocks')
            next_data = payload_at
            for at, stored, plain, codec, flags, digest in self.blocks:
                require(at == next_data and 0 < stored <= CHUNK and 0 < plain <= CHUNK and codec in (0, 1) and not flags and
                        ((codec == 0 and stored == plain) or (codec == 1 and stored < plain)) and stored <= image_size-next_data,
                        'invalid block bounds/codec')
                next_data += stored
            require(image_size == align(next_data), 'invalid final image size')
            previous = None
            for parent, child, at, length, flags in self.entries:
                require(parent < inode_count and 0 < child < inode_count and not flags and self.nodes[parent].kind == DIR,
                        'invalid directory reference')
                name = self.string(at, length, True)
                key = (parent, name)
                require(previous is None or key > previous, 'unsorted/duplicate directory name')
                previous = key
                self.links[child] += 1
                if self.nodes[child].kind == DIR:
                    require(self.links[child] == 1, 'directory hardlink')
                    self.parents[child] = (parent, name)
            paths = [0] * inode_count
            for i in range(1, inode_count):
                require(self.links[i] > 0, 'unreachable inode')
                if self.nodes[i].kind != DIR:
                    continue
                at, depth, length = i, 0, 0
                while at:
                    require(depth < 128 and self.parents[at] is not None, 'directory cycle/depth')
                    at, name = self.parents[at]
                    depth += 1
                    length += len(name)+1
                    require(length <= 4095, 'directory path too long')
                paths[i] = length
            for parent, child, at, length, flags in self.entries:
                require(paths[parent] + length + 1 <= 4095, 'path too long')
            self.children = [[] for _ in self.nodes]
            for parent, child, at, length, flags in self.entries:
                self.children[parent].append((self.string(at, length, True), child))
            self.cached = None
        except BaseException:
            self.file.close()
            raise

    def close(self):
        self.file.close()

    def string(self, at, length, name):
        require(0 < length <= (255 if name else 4095) and 0 <= at <= len(self.strings) and length <= len(self.strings)-at,
                'string bounds')
        value = self.strings[at:at+length]
        require(b'\0' not in value and (not name or (b'/' not in value and value not in (b'.', b'..'))), 'invalid name/target')
        return value

    def paths(self):
        stack = [(b'', 0)]
        while stack:
            path, inode = stack.pop()
            yield path, inode
            if self.nodes[inode].kind == DIR:
                stack.extend((path+b'/'+name if path else name, child) for name, child in reversed(self.children[inode]))

    def block(self, index):
        if self.cached is not None and self.cached[0] == index:
            return self.cached[1]
        self.cached = None
        at, stored, plain, codec, flags, digest = self.blocks[index]
        self.file.seek(at)
        data = self.file.read(stored)
        require(len(data) == stored, 'short payload read')
        if codec:
            if self.codec is None:
                self.codec = Codec(self.library)
            data = self.codec.decode(data, plain)
        require(len(data) == plain and checksum(data) == digest, 'payload checksum/length')
        self.cached = (index, data)
        return data

    def read(self, inode, offset, length):
        node = self.nodes[inode]
        require(node.kind == FILE and offset >= 0 and length >= 0, 'invalid file read')
        length = min(length, max(0, node.size-offset))
        result = bytearray()
        while length:
            block, within = divmod(offset, CHUNK)
            data = self.block(node.first+block)
            count = min(length, len(data)-within)
            result.extend(data[within:within+count])
            length -= count
            offset += count
        return bytes(result)

    def verify(self):
        self.cached = None
        for i in range(len(self.blocks)):
            self.block(i)
        return self.inspect()

    def inspect(self):
        return dict(format='BRKPACK1', identity=self.identity.hex(), image_bytes=self.image_size,
                    index_bytes=self.payload_at, inodes=len(self.nodes), entries=len(self.entries),
                    blocks=len(self.blocks), compressed_blocks=sum(b[3] == 1 for b in self.blocks),
                    plain_bytes=sum(n.size for n in self.nodes if n.kind == FILE),
                    stored_bytes=sum(b[1] for b in self.blocks), integrity='FNV-1a64 corruption checks; not authentication')

def build(source, destination, library=None, compress=True):
    destination = Path(destination)
    require(not destination.exists(), 'refusing to replace existing image')
    source = os.fsencode(source)
    codec = Codec(library) if compress else None
    root = os.open(source, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    nodes, entries, blocks, strings, hardlinks = [], [], [], bytearray(), {}
    def add_string(value):
        at = len(strings)
        strings.extend(value)
        return at
    def record(st, kind, size=0, first=0, count=0, target=0, target_size=0):
        require(0 <= st.st_uid <= 0xffffffff and 0 <= st.st_gid <= 0xffffffff, 'owner out of range')
        # Normalize times and omit privileged mode bits. Content and metadata are
        # deterministic for an unchanged tree and the pinned codec version.
        return Node(kind, stat.S_IFMT(st.st_mode) | (st.st_mode & 0o777), st.st_uid, st.st_gid,
                    size, first, count, target, target_size, 0, 0)
    with tempfile.TemporaryFile() as payload:
        def directory(fd, parent, path, depth):
            require(depth <= 128, 'tree too deep')
            for name in sorted(os.fsencode(name) for name in os.listdir(fd)):
                require(len(name) <= 255 and name not in (b'.', b'..') and b'/' not in name and b'\0' not in name, 'invalid source name')
                full = path+b'/'+name if path else name
                require(len(full)+1 <= 4095, 'source path too long')
                st = os.stat(name, dir_fd=fd, follow_symlinks=False)
                kind = DIR if stat.S_ISDIR(st.st_mode) else FILE if stat.S_ISREG(st.st_mode) else LINK if stat.S_ISLNK(st.st_mode) else 0
                require(kind, 'special files unsupported: '+repr(full))
                known = hardlinks.get((st.st_dev, st.st_ino)) if kind == FILE else None
                child = known if known is not None else len(nodes)
                at = add_string(name)
                entries.append((parent, child, at, len(name), 0))
                if known is not None:
                    continue
                nodes.append(None)
                if kind == DIR:
                    nodes[child] = record(st, kind)
                    sub = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                    try:
                        current = os.fstat(sub)
                        require((current.st_dev, current.st_ino) == (st.st_dev, st.st_ino), 'source directory changed')
                        directory(sub, child, full, depth+1)
                    finally:
                        os.close(sub)
                elif kind == LINK:
                    target = os.readlink(name, dir_fd=fd)
                    require(0 < len(target) <= 4095 and b'\0' not in target, 'invalid symlink target')
                    nodes[child] = record(st, kind, len(target), target=add_string(target), target_size=len(target))
                else:
                    opened = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=fd)
                    with os.fdopen(opened, 'rb') as stream:
                        before = os.fstat(stream.fileno())
                        require((before.st_dev, before.st_ino, before.st_size) == (st.st_dev, st.st_ino, st.st_size), 'source file changed')
                        first, remaining = len(blocks), st.st_size
                        while remaining:
                            plain = stream.read(min(CHUNK, remaining))
                            require(len(plain) == min(CHUNK, remaining), 'source file truncated')
                            encoded = codec.encode(plain) if codec else plain
                            use = compress and len(encoded) < len(plain)
                            data = encoded if use else plain
                            blocks.append((payload.tell(), len(data), len(plain), int(use), 0, checksum(plain)))
                            payload.write(data)
                            remaining -= len(plain)
                        after = os.fstat(stream.fileno())
                        require(not stream.read(1) and (before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                                (after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'source file changed during packing')
                    nodes[child] = record(st, kind, st.st_size, first, len(blocks)-first)
                    hardlinks[(st.st_dev, st.st_ino)] = child
        try:
            nodes.append(record(os.fstat(root), DIR))
            directory(root, 0, b'', 0)
        finally:
            os.close(root)
        entries.sort(key=lambda e:(e[0], bytes(strings[e[2]:e[2]+e[3]])))
        inode_at = HEADER.size
        entry_at = inode_at+len(nodes)*INODE.size
        block_at = entry_at+len(entries)*DIRENT.size
        strings_at = block_at+len(blocks)*BLOCK.size
        payload_at = align(strings_at+len(strings))
        require(payload_at <= INDEX_LIMIT, 'resident index exceeds 16 MiB')
        index = bytearray().join(INODE.pack(*vars(n).values()) for n in nodes)
        index += b''.join(DIRENT.pack(*e) for e in entries)
        index += b''.join(BLOCK.pack(at+payload_at, stored, plain, kind, flags, digest) for at, stored, plain, kind, flags, digest in blocks)
        index += strings
        index += bytes(payload_at-HEADER.size-len(index))
        identity = hashlib.sha256(index).digest()[:16]
        h = [MAGIC, 1, HEADER.size, 0, align(payload_at+payload.tell()), CHUNK, 0,
             inode_at, len(nodes), entry_at, len(entries), block_at, len(blocks), strings_at, len(strings), payload_at,
             identity, checksum(index), 0, bytes(16)]
        h[-2] = checksum(HEADER.pack(*h)[:136])
        # xb prevents replacement, including symlink destinations. Any failure
        # leaves an explicit incomplete new file; existing data is never touched.
        with destination.open('xb') as out:
            out.write(HEADER.pack(*h));out.write(index);payload.seek(0);shutil.copyfileobj(payload, out)
            out.write(bytes(h[4]-out.tell()));out.flush();os.fsync(out.fileno())
    image = Image(destination, library)
    try:
        return image.inspect()
    finally:
        image.close()

def parent_fd(root, path):
    parts = path.split(b'/')
    fd = os.dup(root)
    try:
        for part in parts[:-1]:
            next_fd = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd);fd = next_fd
        return fd, parts[-1]
    except BaseException:
        os.close(fd);raise

def extract(image, destination):
    os.mkdir(destination, 0o700)  # refuse every existing destination
    root = os.open(destination, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    paths = sorted(image.paths(), key=lambda item:(item[0].count(b'/'), item[0]))
    created = {}
    try:
        for path, inode in paths:
            if not path:
                continue
            node = image.nodes[inode]
            parent, leaf = parent_fd(root, path)
            try:
                if node.kind == DIR:
                    os.mkdir(leaf, 0o700, dir_fd=parent)
                elif node.kind == LINK:
                    os.symlink(image.string(node.target, node.target_size, False), leaf, dir_fd=parent)
                elif inode in created:
                    source, name = parent_fd(root, created[inode])
                    try:
                        os.link(name, leaf, src_dir_fd=source, dst_dir_fd=parent, follow_symlinks=False)
                    finally:
                        os.close(source)
                else:
                    fd = os.open(leaf, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=parent)
                    with os.fdopen(fd, 'wb') as out:
                        for offset in range(0, node.size, CHUNK):
                            out.write(image.read(inode, offset, CHUNK))
                        os.fchmod(out.fileno(), node.mode & 0o777)
                        os.utime(out.fileno(), ns=(node.mtime*10**9, node.mtime*10**9))
                    created[inode] = path
            finally:
                os.close(parent)
        for path, inode in reversed(paths):
            node = image.nodes[inode]
            if node.kind != DIR:
                continue
            if path:
                parent, leaf = parent_fd(root, path)
                try:
                    fd = os.open(leaf, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
                finally:
                    os.close(parent)
            else:
                fd = os.dup(root)
            try:
                os.fchmod(fd, node.mode & 0o777)
                os.utime(fd, ns=(node.mtime*10**9, node.mtime*10**9))
            finally:
                os.close(fd)
    finally:
        os.close(root)

class FileStream(io.RawIOBase):
    def __init__(self, image, inode):
        self.image, self.inode, self.offset = image, inode, 0
    def read(self, size=-1):
        if size < 0:
            size = self.image.nodes[self.inode].size-self.offset
        result = self.image.read(self.inode, self.offset, size)
        self.offset += len(result)
        return result

def export_tar(image, destination):
    seen = {}
    with Path(destination).open('xb') as output, tarfile.open(fileobj=output, mode='w', format=tarfile.PAX_FORMAT) as archive:
        for path, inode in image.paths():
            node = image.nodes[inode]
            entry = tarfile.TarInfo(os.fsdecode(path) if path else '.')
            entry.mode, entry.mtime = node.mode & 0o777, node.mtime
            if node.kind == DIR:
                entry.type = tarfile.DIRTYPE
            elif node.kind == LINK:
                entry.type, entry.linkname = tarfile.SYMTYPE, os.fsdecode(image.string(node.target, node.target_size, False))
            elif inode in seen:
                entry.type, entry.linkname = tarfile.LNKTYPE, seen[inode]
            else:
                entry.size = node.size;seen[inode] = entry.name
            archive.addfile(entry, FileStream(image, inode) if entry.isreg() else None)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest='action', required=True)
    for action in ('build','list','inspect','verify','extract','export'):
        sub = subs.add_parser(action)
        if action == 'build':
            sub.add_argument('source');sub.add_argument('image', type=Path)
            sub.add_argument('--codec', choices=('raw','lz4'), default='lz4')
        else:
            sub.add_argument('image', type=Path)
        if action in ('extract','export'):
            sub.add_argument('destination', type=Path)
        sub.add_argument('--lz4-library')
    args = parser.parse_args()
    if args.action == 'build':
        print(json.dumps(build(args.source, args.image, args.lz4_library, args.codec == 'lz4'), indent=2));return
    image = Image(args.image, args.lz4_library)
    try:
        if args.action == 'list':
            print(json.dumps([dict(path=os.fsdecode(path), inode=inode, **vars(image.nodes[inode])) for path,inode in image.paths()], indent=2))
        elif args.action == 'verify':
            print(json.dumps(image.verify(), indent=2))
        elif args.action == 'inspect':
            print(json.dumps(image.inspect(), indent=2))
        elif args.action == 'extract':
            extract(image, args.destination)
        else:
            export_tar(image, args.destination)
    finally:
        image.close()
if __name__ == '__main__':
    main()
