"""Hash and import the exact Nix archive named in a trusted stage plan."""

import base64
from contextlib import ExitStack
import errno
import hashlib
import json
import os
import re
import stat
import struct
import shutil
import subprocess
import tempfile
from pathlib import Path
from urllib.parse import quote, urlsplit
from zipfile import ZIP_STORED, ZipFile, BadZipFile

UPSTREAM = "https://cache.nixos.org"
STORE = Path("/nix/store")
STORE_NAME = re.compile(r"[0123456789abcdfghijklmnpqrsvwxyz]{32}-[a-zA-Z0-9+._?=-]+")
NIX32 = "0123456789abcdfghijklmnpqrsvwxyz"
CHUNK = 1024 * 1024
CACHE_BUNDLE_LIMIT = 960 * CHUNK
CACHE_BUNDLE_COUNT = 16
CACHE_METADATA_LIMIT = 64 * 1024
CACHE_MEMBER_NAME = re.compile(r"(?:[0123456789abcdfghijklmnpqrsvwxyz]{32}[.]narinfo|nar/[a-zA-Z0-9_+.-]+)")


def store_name(value):
    path = Path(value)
    if str(path.parent) != str(STORE) or not STORE_NAME.fullmatch(path.name):
        raise ValueError(f"Invalid Nix store path {value!r}")
    return path.name


def profile_layout(profile, closure, *, store=STORE, legacy_paths=()):
    """Expand a verified profile root without changing its store references."""
    links = {}
    directories = []

    def walk(source, relative, ancestors):
        resolved = source.resolve(strict=True)
        if resolved.is_relative_to(store):
            suffix = resolved.relative_to(store)
        else:
            suffix = resolved.relative_to(STORE)
        if not suffix.parts or suffix.parts[0] not in closure or resolved in ancestors or len(ancestors) >= 128:
            raise ValueError(f"Invalid profile entry {source} -> {resolved}")
        if resolved.is_dir():
            if relative.parts:
                directories.append(relative.as_posix())
            for child in sorted(resolved.iterdir()):
                if child not in legacy_paths:
                    walk(child, relative / child.name, (*ancestors, resolved))
        elif resolved.is_file() and relative.parts:
            links[relative.as_posix()] = str(STORE / suffix)
        else:
            raise ValueError(f"Unsupported profile entry {source}")

    walk(profile, Path(), ())
    return {"directories": directories, "links": links}


def layout_digest(layout):
    canonical = json.dumps(layout, sort_keys=True, separators=(",", ":")).encode("ascii")
    return hashlib.sha256(canonical).hexdigest()


def nar_hash(value):
    if value.startswith("sha256-"):
        raw = base64.b64decode(value[7:], validate=True)
    elif value.startswith("sha256:") and len(value) == 59:
        packed = 0
        for digit in value[7:]:
            packed = packed * 32 + NIX32.index(digit)
        raw = packed.to_bytes(32, "little")
    else:
        raise ValueError(f"Invalid NAR hash {value!r}")
    if len(raw) != 32:
        raise ValueError(f"Invalid NAR digest length for {value!r}")
    return "sha256-" + base64.b64encode(raw).decode("ascii")


def hash_tree(path, expected_size, *, legacy_paths=()):
    """Produce the digest of a canonical NAR without writing an archive."""
    digest = hashlib.sha256()
    size = 0

    def emit(data):
        nonlocal size
        size += len(data)
        if size > expected_size:
            raise ValueError(f"canonical NAR exceeds the signed {expected_size}-byte size for {path}")
        digest.update(data)

    def length(n):
        emit(struct.pack("<Q", n))

    def pad(n):
        emit(b"\0" * (-n % 8))

    def string(data):
        length(len(data))
        emit(data)
        pad(len(data))

    def node(entry):
        mode = entry.lstat().st_mode
        string(b"(")
        string(b"type")
        if stat.S_ISLNK(mode):
            string(b"symlink")
            string(b"target")
            string(os.fsencode(os.readlink(entry)))
        elif stat.S_ISDIR(mode):
            string(b"directory")
            for child in sorted(entry.iterdir(), key=lambda item: os.fsencode(item.name)):
                if child in legacy_paths:
                    continue
                string(b"entry")
                string(b"(")
                string(b"name")
                string(os.fsencode(child.name))
                string(b"node")
                node(child)
                string(b")")
        elif stat.S_ISREG(mode):
            string(b"regular")
            if mode & stat.S_IXUSR:
                string(b"executable")
                string(b"")
            string(b"contents")
            flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
            with os.fdopen(os.open(entry, flags), "rb") as stream:
                st = os.fstat(stream.fileno())
                if not stat.S_ISREG(st.st_mode):
                    raise ValueError(f"Store file changed type: {entry}")
                length(st.st_size)
                remaining = st.st_size
                while remaining:
                    chunk = stream.read(min(CHUNK, remaining))
                    if not chunk:
                        raise ValueError(f"Store file shrank: {entry}")
                    emit(chunk)
                    remaining -= len(chunk)
                if stream.read(1):
                    raise ValueError(f"Store file grew: {entry}")
                pad(st.st_size)
        else:
            raise ValueError(f"Unexpected store node type: {entry}")
        string(b")")

    string(b"nix-archive-1")
    node(path)
    if size != expected_size:
        raise ValueError(f"NAR has {size} bytes, expected {expected_size} for {path}")
    return "sha256-" + base64.b64encode(digest.digest()).decode("ascii")


def verify_tree(path, entry, *, legacy_paths=()):
    if hash_tree(path, entry["narSize"], legacy_paths=legacy_paths) != entry["narHash"]:
        raise ValueError(f"NAR hash mismatch for {path}")


def curl(tool, url):
    return [tool, "--fail", "--silent", "--show-error", "--connect-timeout", "10",
            "--max-time", "1200", "--proto", "=http,https", url]


def read_narinfo(tool, cache, name, entry):
    with subprocess.Popen(curl(tool, f"{cache}/{name[:32]}.narinfo"),
                          stdout=subprocess.PIPE) as proc:
        metadata = proc.stdout.read(65537)
        if len(metadata) > 65536:
            proc.kill()
            raise ValueError("NAR metadata exceeded 64 KiB")
        if proc.wait():
            raise OSError(f"NAR metadata unavailable at {cache} for {name}")
    location, compression = parse_narinfo(metadata, name, entry)
    return f"{cache}/" + "/".join(quote(part) for part in location.split("/")), compression


def parse_narinfo(metadata, name, entry):
    if len(metadata) > CACHE_METADATA_LIMIT:
        raise ValueError("NAR metadata exceeded 64 KiB")
    fields = dict(line.split(": ", 1) for line in metadata.decode().splitlines() if ": " in line)
    if (
        fields.get("StorePath") != str(STORE / name)
        or nar_hash(fields["NarHash"]) != entry["narHash"]
        or int(fields["NarSize"]) != entry["narSize"]
        or sorted(fields.get("References", "").split()) != entry["references"]
    ):
        raise ValueError(f"NAR metadata disagrees with the signed plan for {name}")
    url = fields["URL"]
    components = url.split("/")
    if urlsplit(url).scheme or any(
        not re.fullmatch(r"[a-zA-Z0-9_+.-]+", part) or part in (".", "..")
        for part in components
    ):
        raise ValueError(f"Invalid NAR location for {name}")
    return url, fields["Compression"]


def archive_digest(incoming, compression, archive, entry, tools):
    if compression not in ("none", "xz", "zstd"):
        raise ValueError(f"Unsupported NAR compression {compression!r}")
    decompress = None
    try:
        source = incoming
        if compression != "none":
            decompress = subprocess.Popen([tools[compression], "-d", "-c"],
                                          stdin=incoming, stdout=subprocess.PIPE)
            source = decompress.stdout
        digest = hashlib.sha256()
        size = 0
        with archive.open("xb") as output:
            while chunk := source.read(CHUNK):
                size += len(chunk)
                if size > entry["narSize"]:
                    raise ValueError("Downloaded NAR exceeds the signed size")
                digest.update(chunk)
                output.write(chunk)
        if decompress is not None and decompress.wait():
            raise OSError("NAR transfer or decompression failed")
        return size, "sha256-" + base64.b64encode(digest.digest()).decode("ascii")
    finally:
        if decompress is not None:
            decompress.stdout.close()
            if decompress.poll() is None:
                decompress.kill()
                decompress.wait()


def check_archive(result, entry):
    if result != (entry["narSize"], entry["narHash"]):
        raise ValueError("Downloaded NAR does not match the signed plan")


def verified_archive(url, compression, archive, entry, tools):
    with subprocess.Popen(curl(tools["curl"], url), stdout=subprocess.PIPE) as download:
        try:
            result = archive_digest(download.stdout, compression, archive, entry, tools)
            if download.wait():
                raise OSError("NAR transfer or decompression failed")
            check_archive(result, entry)
        finally:
            if download.poll() is None:
                download.kill()
                download.wait()


class PackedCache(ExitStack):
    """A bridge-delivered cache. Read named members without extracting paths."""

    def __init__(self, paths):
        super().__init__()
        self.paths = paths
        self.members = {}

    def __enter__(self):
        if not 1 <= len(self.paths) <= CACHE_BUNDLE_COUNT:
            raise ValueError("Invalid number of cache bundles")
        try:
            for path in self.paths:
                if not path.is_file() or path.stat().st_size > CACHE_BUNDLE_LIMIT:
                    raise ValueError(f"Invalid cache bundle size or type: {path}")
                bundle = self.enter_context(ZipFile(path))
                for member in bundle.infolist():
                    name = member.filename
                    if (member.compress_type != ZIP_STORED or member.flag_bits & 1
                            or not CACHE_MEMBER_NAME.fullmatch(name)
                            or member.file_size > CACHE_BUNDLE_LIMIT
                            or name in self.members or name.rsplit("/", 1)[-1] in (".", "..")
                            or len(self.members) >= 32768):
                        raise ValueError(f"Unexpected cache bundle member {name!r}")
                    self.members[name] = (bundle, member)
            return self
        except (BadZipFile, OSError, ValueError) as error:
            self.close()
            raise ValueError(f"Invalid bridge cache: {error}") from error

    def contains(self, name):
        return name[:32] + ".narinfo" in self.members

    def archive(self, name, output, entry, tools):
        metadata, member = self.members[name[:32] + ".narinfo"]
        try:
            with metadata.open(member) as stream:
                location, compression = parse_narinfo(stream.read(CACHE_METADATA_LIMIT + 1), name, entry)
            if location not in self.members:
                raise ValueError(f"Missing packed NAR {location} for {name}")
            bundle, member = self.members[location]
            with bundle.open(member) as compressed:
                if compression == "none":
                    result = archive_digest(compressed, compression, output, entry, tools)
                else:
                    # The subprocess needs an actual input descriptor. Only
                    # one compressed member is copied, under the same scratch
                    # directory as the uncompressed NAR.
                    with tempfile.TemporaryFile(dir=output.parent) as staged:
                        shutil.copyfileobj(compressed, staged, CHUNK)
                        staged.seek(0)
                        result = archive_digest(staged, compression, output, entry, tools)
            check_archive(result, entry)
        except BadZipFile as error:
            raise ValueError(f"Corrupt packed archive for {name}") from error


def rename_no_replace(source, target):
    import ctypes

    libc = ctypes.CDLL(None, use_errno=True)
    if libc.renameat2(-100, os.fsencode(source), -100, os.fsencode(target), 1):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(target))


def legacy_lib_links(path, references):
    """Identify direct runtime aliases made by nix-install's legacy linker."""
    directory = path / "lib"
    if path.is_symlink() or directory.is_symlink() or not directory.is_dir():
        return set()
    dependencies = set(references) - {path.name}
    return {
        child for child in directory.iterdir()
        if child.is_symlink() and not child.name.startswith(".")
        and (parts := os.readlink(child).split("/"))[:2] == ["..", ".."]
        and len(parts) == 5 and parts[2] in dependencies
        and parts[3:] == ["lib", child.name]
    }


def verify_installed(path, entry, legacy_paths):
    try:
        verify_tree(path, entry)
        return
    except (OSError, ValueError) as strict_error:
        if legacy_paths is None or entry["source"] != "upstream":
            raise ValueError(f"Existing store path failed verification: {strict_error}") from strict_error
        try:
            links = legacy_lib_links(path, entry["references"])
            if not links:
                raise strict_error
            try:
                verify_tree(path, entry, legacy_paths=links)
            except ValueError:
                # The legacy linker also creates lib/ when a package lacked it.
                directory = path / "lib"
                if any(child not in links for child in directory.iterdir()):
                    raise
                links.add(directory)
                verify_tree(path, entry, legacy_paths=links)
        except (OSError, ValueError) as error:
            raise ValueError(f"Existing store path failed verification: {error}") from error
        legacy_paths.update(links)


def check_legacy_links(store, closure, legacy_paths):
    for link in sorted(legacy_paths):
        if not link.is_symlink():
            continue  # The optional legacy lib/ directory itself is not a link.
        resolved = link.resolve(strict=True)
        if (not resolved.is_file() or not resolved.is_relative_to(store)
                or resolved.relative_to(store).parts[0] not in closure
                or resolved in legacy_paths):
            raise ValueError(f"Legacy runtime library link does not resolve to a verified file: {link}")


def ensure_path(store, name, entry, cache, tools, *, legacy_paths=None, packed_cache=None):
    target = store / name
    if os.path.lexists(target):
        verify_installed(target, entry, legacy_paths)
        return "present"
    with tempfile.TemporaryDirectory(prefix=".brook-nar-", dir=store) as directory:
        scratch = Path(directory)
        archive = scratch / "archive.nar"
        upstream = entry["source"] == "upstream"
        if packed_cache is not None and packed_cache.contains(name):
            packed_cache.archive(name, archive, entry, tools)
            origin = "bridge"
        else:
            if packed_cache is not None or cache["brook"] is None:
                if not upstream:
                    raise ValueError(f"Missing bridge cache entry for {name}")
                sources = [UPSTREAM]
            else:
                sources = [cache["brook"]]
                if upstream:
                    sources = [UPSTREAM, cache["brook"]] if cache["mirrorUpstream"] else [UPSTREAM]
            for index, source in enumerate(sources):
                try:
                    url, compression = read_narinfo(tools["curl"], source, name, entry)
                    verified_archive(url, compression, archive, entry, tools)
                    break
                except OSError:
                    if index + 1 == len(sources):
                        raise
                    archive.unlink(missing_ok=True)
            origin = "upstream" if source == UPSTREAM else "brook"
        unpacked = scratch / "path"
        with archive.open("rb") as stream:
            subprocess.run([tools["nar-unpack"], unpacked], check=True, stdin=stream)
        try:
            verify_tree(unpacked, entry)
        except (OSError, ValueError) as error:
            raise ValueError(f"Extracted store path failed verification for {name}: {error}") from error
        try:
            rename_no_replace(unpacked, target)
        except OSError as error:
            if error.errno != errno.EEXIST:
                raise
            verify_installed(target, entry, legacy_paths)
            return "present"
        return origin
