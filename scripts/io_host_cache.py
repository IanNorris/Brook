"""Host file-page-cache controls for stopped, privately owned test images."""
import ctypes
import mmap
import os
from pathlib import Path
import stat
import time


def residency(fd):
    """Observe residency without faulting file pages in; this is a snapshot."""
    size = os.fstat(fd).st_size
    page = mmap.PAGESIZE
    if not size or size % page:
        raise ValueError("Test images must be nonempty and page aligned")
    count = size // page
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                            ctypes.POINTER(ctypes.c_ubyte)]
    libc.mincore.restype = ctypes.c_int
    with mmap.mmap(fd, size, flags=mmap.MAP_PRIVATE,
                   prot=mmap.PROT_READ | mmap.PROT_WRITE) as mapping:
        address = ctypes.addressof(ctypes.c_char.from_buffer(mapping))
        vector = (ctypes.c_ubyte * count)()
        if libc.mincore(address, size, vector):
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
        resident = sum(value & 1 for value in vector)
    return dict(pages=count, resident_pages=resident, page_bytes=page)


def evict_images(directory, names=("boot.img", "data.img", "nix.img")):
    """Call only after the original VM owner has waited for QEMU's exit.

    Flush each private inode before requesting DONTNEED. No system-wide cache
    control is used. Device caches and filesystem metadata are outside scope.
    Failure preserves the images and makes a cache-cold claim fail closed.
    """
    directory = Path(directory).resolve(strict=True)
    results = {}
    for name in names:
        if Path(name).name != name:
            raise ValueError("Image names must be single path components")
        path = directory / name
        fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid()
                    or info.st_nlink != 1):
                raise ValueError("Need a regular, owned image with one link")
            began = time.monotonic()
            os.fsync(fd)
            before = residency(fd)
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            after = residency(fd)
            results[name] = dict(before=before, after=after,
                                 elapsed_host_s=time.monotonic()-began)
            if after["resident_pages"]:
                raise RuntimeError(f"Image cache eviction incomplete: {name}")
        finally:
            os.close(fd)
    return dict(images=results, scope="host file pages only; residency snapshots; "
                "not device-cache or filesystem-metadata eviction")
