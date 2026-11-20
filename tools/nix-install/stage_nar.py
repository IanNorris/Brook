"""Hash and import the exact Nix archive named in a trusted stage plan."""

import base64
import errno
import hashlib
import json
import os
import re
import stat
import struct
import subprocess
import tempfile
from pathlib import Path
from urllib.parse import quote, urlsplit

UPSTREAM = "https://cache.nixos.org"
STORE = Path("/nix/store")
STORE_NAME = re.compile(r"[0123456789abcdfghijklmnpqrsvwxyz]{32}-[a-zA-Z0-9+._?=-]+")
NIX32 = "0123456789abcdfghijklmnpqrsvwxyz"
CHUNK = 1024 * 1024


def store_name(value):
    path = Path(value)
    if str(path.parent) != str(STORE) or not STORE_NAME.fullmatch(path.name):
        raise ValueError(f"Invalid Nix store path {value!r}")
    return path.name


def profile_layout(profile, closure, *, store=STORE):
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


def hash_tree(path, expected_size):
    """Produce the digest of a canonical NAR without writing an archive."""
    digest = hashlib.sha256()
    size = 0

    def emit(data):
        nonlocal size
        size += len(data)
        if size > expected_size:
            raise ValueError(f"NAR size exceeds the plan for {path}")
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


def verify_tree(path, entry):
    if hash_tree(path, entry["narSize"]) != entry["narHash"]:
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
    fields = dict(line.split(": ", 1) for line in metadata.decode().splitlines() if ": " in line)
    if (
        fields.get("StorePath") != str(STORE / name)
        or nar_hash(fields["NarHash"]) != entry["narHash"]
        or int(fields["NarSize"]) != entry["narSize"]
        or sorted(fields["References"].split()) != entry["references"]
    ):
        raise ValueError(f"NAR metadata disagrees with the signed plan for {name}")
    url = fields["URL"]
    components = url.split("/")
    if urlsplit(url).scheme or any(
        not re.fullmatch(r"[a-zA-Z0-9_+.-]+", part) or part in (".", "..")
        for part in components
    ):
        raise ValueError(f"Invalid NAR location for {name}")
    return f"{cache}/" + "/".join(quote(part) for part in components), fields["Compression"]


def verified_archive(url, compression, archive, entry, tools):
    digest = hashlib.sha256()
    size = 0
    with subprocess.Popen(curl(tools["curl"], url), stdout=subprocess.PIPE) as download:
        decompress = None
        try:
            if compression == "none":
                source = download.stdout
            else:
                if compression not in ("xz", "zstd"):
                    raise ValueError(f"Unsupported NAR compression {compression!r}")
                decompress = subprocess.Popen([tools[compression], "-d", "-c"],
                                              stdin=download.stdout, stdout=subprocess.PIPE)
                download.stdout.close()
                source = decompress.stdout
            with source, archive.open("xb") as output:
                while chunk := source.read(CHUNK):
                    size += len(chunk)
                    if size > entry["narSize"]:
                        raise ValueError("Downloaded NAR exceeds the signed size")
                    digest.update(chunk)
                    output.write(chunk)
            status = decompress.wait() if decompress is not None else 0
            if download.wait() or status:
                raise OSError("NAR transfer or decompression failed")
        finally:
            for process in (decompress, download):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()
    actual = "sha256-" + base64.b64encode(digest.digest()).decode("ascii")
    if size != entry["narSize"] or actual != entry["narHash"]:
        raise ValueError("Downloaded NAR does not match the signed plan")


def rename_no_replace(source, target):
    import ctypes

    libc = ctypes.CDLL(None, use_errno=True)
    if libc.renameat2(-100, os.fsencode(source), -100, os.fsencode(target), 1):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(target))


def ensure_path(store, name, entry, cache, tools):
    target = store / name
    if os.path.lexists(target):
        verify_tree(target, entry)
        return "present"
    sources = [cache["brook"]]
    if entry["source"] == "upstream":
        sources = [UPSTREAM, cache["brook"]] if cache["mirrorUpstream"] else [UPSTREAM]
    with tempfile.TemporaryDirectory(prefix=".brook-nar-", dir=store) as directory:
        scratch = Path(directory)
        archive = scratch / "archive.nar"
        for index, source in enumerate(sources):
            try:
                url, compression = read_narinfo(tools["curl"], source, name, entry)
                verified_archive(url, compression, archive, entry, tools)
                break
            except OSError:
                if index + 1 == len(sources):
                    raise
                archive.unlink(missing_ok=True)
        unpacked = scratch / "path"
        with archive.open("rb") as stream:
            subprocess.run([tools["nar-unpack"], unpacked], check=True, stdin=stream)
        verify_tree(unpacked, entry)
        try:
            rename_no_replace(unpacked, target)
        except OSError as error:
            if error.errno != errno.EEXIST:
                raise
            verify_tree(target, entry)
            return "present"
        return "upstream" if source == UPSTREAM else "brook"
