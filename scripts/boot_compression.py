"""Build and verify the loader's signed, size-bounded LZ4 payloads."""
import ctypes
import hashlib
import os
import stat
import struct

from nacl.signing import SigningKey, VerifyKey

MAGIC = b'BRKCMP01'
SIGN_DOMAIN = b'BROOK-BOOT-COMPRESSION-V1\0'
KERNEL = 1
INITRD = 2
HEADER = struct.Struct('<8sIIQQ32s')
PREFIX_SIZE = HEADER.size + 64
MIN_SIZE = {KERNEL: 64, INITRD: 512}


def read_signer(path, other_public_keys=()):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        mode = os.fstat(stream.fileno()).st_mode
        if not stat.S_ISREG(mode) or mode & 0o077:
            raise ValueError('boot compression seed must be a private regular file')
        seed = stream.read(33)
    if len(seed) != 32:
        raise ValueError('boot compression seed must contain exactly 32 raw bytes')
    signer = SigningKey(seed)
    if signer.verify_key.encode() in other_public_keys:
        raise ValueError('boot compression requires a dedicated signing key')
    return signer


def lz4():
    lib = ctypes.CDLL(os.environ['BROOK_IMAGE_LZ4_LIBRARY'])
    lib.LZ4_compressBound.argtypes = [ctypes.c_int]
    lib.LZ4_compress_default.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.LZ4_decompress_safe.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    return lib


def compress(data, kind, signer, maximum):
    if not MIN_SIZE[kind] <= len(data) <= maximum:
        raise ValueError('boot payload size is outside the allowed range')
    lib = lz4()
    target = ctypes.create_string_buffer(lib.LZ4_compressBound(len(data)))
    size = lib.LZ4_compress_default(data, target, len(data), len(target))
    if size <= 0:
        raise ValueError('LZ4 compression failed')
    if not MIN_SIZE[kind] <= size + PREFIX_SIZE < len(data):
        return data
    body = target.raw[:size]
    header = HEADER.pack(MAGIC, kind, 1, len(data), size, hashlib.sha256(body).digest())
    return header + signer.sign(SIGN_DOMAIN + header).signature + body


def expand(data, kind, public_key, maximum):
    if not data.startswith(MAGIC):
        return data
    if not PREFIX_SIZE < len(data) <= maximum:
        raise ValueError('invalid compressed boot payload size')
    magic, actual_kind, codec, size, stored, digest = HEADER.unpack_from(data)
    if actual_kind != kind or codec != 1 or not MIN_SIZE[kind] <= size <= maximum or stored != len(data) - PREFIX_SIZE:
        raise ValueError('invalid compressed boot payload header')
    VerifyKey(public_key).verify(SIGN_DOMAIN + data[:HEADER.size], data[HEADER.size:PREFIX_SIZE])
    body = data[PREFIX_SIZE:]
    if hashlib.sha256(body).digest() != digest:
        raise ValueError('compressed boot payload digest mismatch')
    target = ctypes.create_string_buffer(size)
    if lz4().LZ4_decompress_safe(body, target, stored, size) != size:
        raise ValueError('invalid LZ4 boot payload')
    return target.raw
