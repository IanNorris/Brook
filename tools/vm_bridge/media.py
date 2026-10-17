"""Reference-first MCP delivery for bridge-owned media (not guest text)."""
import base64
import hashlib
import json
import mimetypes
import os
from pathlib import Path
import re
import stat
import struct
import time
import uuid


MAX_PREVIEW_BYTES = 8 * 1024 * 1024
PATH_FIELDS = {"artifact", "waveform", "pcm", "profile", "speedscope", "timing_artifact"}


def scoped_path(registry, session, name):
    """Only completed, regular, user-owned files directly within this run."""
    if not re.fullmatch(r"[0-9a-f]{32}\.[A-Za-z0-9_.]+", name):
        raise ValueError("Invalid artifact basename")
    run = Path(registry) / session
    directory = run / "artifacts"
    if run.is_symlink() or directory.is_symlink():
        raise ValueError("Artifact directory escapes its run")
    path = directory / name
    if path.is_symlink() or path.resolve().parent != directory.resolve():
        raise ValueError("Artifact escapes its run")
    if ".partial." in name or name.endswith(".tmp"):
        raise ValueError("Artifact is not finalized")
    # Capture workers expose names while still writing. Those paths are useful
    # status, but cannot be fetched as completed artifacts.
    for filename in ("recording.json", "video-recording.json"):
        state_path = run / filename
        if state_path.exists():
            state = json.loads(state_path.read_text())
            pending = filename == "recording.json" or state.get("state") != "complete"
            if pending and str(path) in [state.get(key) for key in
                                        ("artifact", "timing_artifact", "partial_artifact")]:
                raise ValueError("Artifact is not finalized")
    return path


def describe(registry, session, name):
    path = scoped_path(registry, session, name)
    # O_NOFOLLOW also closes the final-component check/open symlink race.
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), "rb") as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid():
            raise ValueError("Artifact must be a regular file owned by this user")
        header = source.read(24)
        source.seek(0)
        checksum = hashlib.file_digest(source, "sha256").hexdigest()
        after = os.fstat(source.fileno())
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError("Artifact changed during delivery")
    mime = {".memory": "application/octet-stream", ".profile": "text/plain",
            ".wav": "audio/wav", ".mkv": "video/x-matroska"}.get(path.suffix)
    result = {"artifact_id": name, "path": str(path.absolute()), "mime_type": mime or
              mimetypes.guess_type(path.name)[0] or "application/octet-stream",
              "byte_length": before.st_size, "sha256": checksum,
              # Old artifacts have no trustworthy monotonic creation timestamp.
              "created_host_monotonic": None, "described_host_monotonic": time.monotonic()}
    if header.startswith(b"\x89PNG\r\n\x1a\n") and header[12:16] == b"IHDR":
        result.update(mime_type="image/png", width=struct.unpack(">I", header[16:20])[0],
                      height=struct.unpack(">I", header[20:24])[0])
    return result


class PreviewLimitError(ValueError):
    pass


class Delivery:
    """Recursively extract typed bytes and register known artifact path fields.

    Callers validate session access first. Strings, including stdout containing
    base64 or data URIs, are never interpreted as media.
    """
    def __init__(self, registry, session, include_image=False):
        self.registry, self.session = registry, session
        self.include_image = include_image
        self.images = []
        self.preview_bytes = 0
        self.descriptors = {}

    def reference(self, path):
        path = Path(path)
        expected = scoped_path(self.registry, self.session, path.name)
        if path.absolute() != expected.absolute():
            raise ValueError("Artifact path is outside its run")
        if path.name not in self.descriptors:
            self.descriptors[path.name] = describe(self.registry, self.session, path.name)
        return self.descriptors[path.name]

    def binary(self, blob, image=False, existing=None):
        blob = bytes(blob)
        if existing is not None:
            descriptor = self.reference(existing)
            if descriptor["sha256"] != hashlib.sha256(blob).hexdigest():
                raise ValueError("Image bytes do not match their artifact")
        else:
            directory = Path(self.registry) / self.session / "artifacts"
            directory.mkdir(mode=0o700, exist_ok=True)
            name = uuid.uuid4().hex + (".png" if image else ".bin")
            path = scoped_path(self.registry, self.session, name)
            temp = path.with_suffix(path.suffix + ".tmp")
            try:
                with temp.open("xb") as target:
                    os.chmod(temp, 0o600)
                    target.write(blob)
                temp.replace(path)
            finally:
                temp.unlink(missing_ok=True)
            descriptor = self.reference(path)
            descriptor["created_host_monotonic"] = time.monotonic()
        if image and self.include_image:
            if descriptor["mime_type"] != "image/png":
                raise ValueError("Image preview requires PNG")
            if self.preview_bytes + len(blob) > MAX_PREVIEW_BYTES or len(self.images) >= 4:
                raise PreviewLimitError("Image preview exceeds delivery limit; use artifact references")
            self.preview_bytes += len(blob)
            self.images.append({"type": "image", "mimeType": "image/png",
                                "data": base64.b64encode(blob).decode("ascii")})
        return descriptor

    def adapt(self, value):
        if isinstance(value, (bytes, bytearray, memoryview)):
            return self.binary(value)
        if isinstance(value, (list, tuple)):
            return [self.adapt(item) for item in value]
        if not isinstance(value, dict):
            return value
        if isinstance(value.get("image"), (bytes, bytearray, memoryview)):
            metadata = value.get("data", {})
            if not isinstance(metadata, dict):
                raise ValueError("Image metadata must be an object")
            existing = metadata.get("artifact") or metadata.get("waveform")
            descriptor = self.binary(value["image"], image=True, existing=existing)
            # Preserve the established image/data envelope's flattened metadata.
            rest = {k: v for k, v in value.items() if k not in {"image", "data"}}
            result = self.adapt({**metadata, **rest})
            result["image_artifact"] = descriptor
            return result
        result = {}
        for key, item in value.items():
            if key in PATH_FIELDS and isinstance(item, str):
                # Active recordings advertise future paths; only finalized
                # recordings have completed artifact descriptors.
                if value.get("state") not in {"starting", "recording", "failed"}:
                    self.reference(item)
            result[key] = self.adapt(item)
        return result
