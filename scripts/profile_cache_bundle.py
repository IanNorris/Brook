"""Pack a local Brook binary cache for the bounded hardware bulk transport."""

from pathlib import Path
import sys
from zipfile import ZIP_STORED, ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/nix-install"))
from stage_nar import (CACHE_BUNDLE_COUNT, CACHE_BUNDLE_LIMIT, CACHE_MEMBER_NAME,
                       CACHE_METADATA_LIMIT, parse_narinfo, store_name)


def cache_inventory(cache, plan, *, missing=None):
    if cache.is_symlink() or not cache.is_dir() or (cache / "nar").is_symlink():
        raise ValueError("Expected a local generated binary cache directory")
    paths = {}
    for name, entry in sorted(plan["closure"].items()):
        store_name("/nix/store/" + name)
        if missing is not None and name not in missing:
            continue
        if entry["source"] == "upstream" and not plan["cache"]["mirrorUpstream"]:
            continue
        info_name = name[:32] + ".narinfo"
        info = cache / info_name
        if info.is_symlink() or not info.is_file():
            raise ValueError(f"Missing local metadata for {name}")
        with info.open("rb") as stream:
            metadata = stream.read(CACHE_METADATA_LIMIT + 1)
        member, _ = parse_narinfo(metadata, name, entry)
        if not member.startswith("nar/") or not CACHE_MEMBER_NAME.fullmatch(member):
            raise ValueError(f"Invalid local archive name for {name}")
        source = cache / member
        if source.is_symlink() or not source.is_file():
            raise ValueError(f"Missing local archive for {name}")
        paths[info_name] = info
        paths[member] = source
    return sorted(paths.items())


def pack_cache(cache, plan, out, *, limit=CACHE_BUNDLE_LIMIT, missing=None):
    if not 4096 <= limit <= CACHE_BUNDLE_LIMIT:
        raise ValueError("Invalid cache bundle size cap")
    groups = [[]]
    # ZIP_STORED does not recompress NAR bytes. Allow room per member for
    # the local header, central directory, timestamps and ZIP64 metadata.
    used = 1024
    for name, source in cache_inventory(cache, plan, missing=missing):
        size = source.stat().st_size + 2 * len(name) + 256
        if 1024 + size > limit:
            raise ValueError(f"Cache member exceeds bridge bundle size: {name}")
        if used + size > limit:
            groups.append([])
            used = 1024
        groups[-1].append((name, source))
        used += size
    if len(groups) > CACHE_BUNDLE_COUNT:
        raise ValueError("Binary cache needs more than 16 bridge bundles")
    for index, group in enumerate(groups):
        path = out / f"cache-{index:02d}.zip"
        with ZipFile(path, "x", compression=ZIP_STORED, strict_timestamps=False) as bundle:
            for name, source in group:
                bundle.write(source, name)
        if path.stat().st_size > limit:
            raise ValueError(f"Cache bundle exceeds its transfer bound: {path}")
        yield path



def validate_push_capacity(paths, capabilities):
    block = capabilities.get("block_bytes")
    free = capabilities.get("free_bytes")
    if (capabilities.get("bridge_cache_api") != 1 or type(block) is not int or block <= 0
            or type(free) is not int or free < 0):
        raise ValueError("Invalid bridge cache capacity response")
    sizes = (path.stat().st_size for path in paths)
    required = sum(((size + block - 1) // block) * block for size in sizes)
    if required > free:
        raise ValueError(f"Bridge cache needs {required} guest bytes; {free} available on /data")
    return required
