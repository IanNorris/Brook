#!/usr/bin/env python3
"""Reproduce the 1 GiB Wasm import-limit patch and <25 MB hosting chunks."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = ROOT / 'vendor/linux-lab-runtime/qemu-system-x86_64.wasm'
assert hashlib.sha256(source.read_bytes()).hexdigest() == 'e4426f3c9bfeb91459b4dc22598b3d51100077224c589199101924f1c2dacb5e'
data = bytearray(source.read_bytes())
pos = 8

def uleb():
    global pos
    result = shift = 0
    while True:
        value = data[pos]
        pos += 1
        result |= (value & 127) << shift
        if value < 128:
            return result
        shift += 7

def name():
    global pos
    length = uleb()
    value = bytes(data[pos:pos + length]).decode()
    pos += length
    return value

def encode(value):
    result = bytearray()
    while True:
        part = value & 127
        value >>= 7
        result.append(part | (128 if value else 0))
        if not value:
            return result

assert data[:8] == b'\0asm\1\0\0\0'
patched = False
while pos < len(data):
    section = data[pos]
    pos += 1
    size = uleb()
    end = pos + size
    if section == 2:
        for _ in range(uleb()):
            module, field, kind = name(), name(), data[pos]
            pos += 1
            if kind == 0:
                uleb()
            elif kind == 1:
                pos += 1
                flags = uleb()
                uleb()
                if flags & 1:
                    uleb()
            elif kind == 2:
                flags = uleb()
                start = pos
                minimum, maximum = uleb(), uleb()
                assert (module, field, flags, minimum, maximum) == ('a', 'a', 3, 36800, 36800), (module, field, flags, minimum, maximum)
                replacement = encode(16384) + encode(16384)
                assert len(replacement) == pos - start
                data[start:pos] = replacement
                patched = True
            elif kind == 3:
                pos += 2
            elif kind == 4:
                uleb()
                uleb()
            else:
                raise ValueError(f'Unknown import kind {kind}')
    pos = end
assert patched
out = ROOT / 'dist/runtime'
out.mkdir(parents=True, exist_ok=True)
chunk_size = 16 * 1024 * 1024
parts = []
for index, offset in enumerate(range(0, len(data), chunk_size)):
    target = out / f'qemu-system-x86_64.wasm.part{index}'
    target.write_bytes(data[offset:offset + chunk_size])
    parts.append({'file': target.name, 'bytes': target.stat().st_size, 'sha256': hashlib.sha256(target.read_bytes()).hexdigest()})
metadata = {'qemuVersion': '8.2.0', 'runtimeSource': 'https://github.com/dbontr/linux-lab', 'buildRecipeCommit': '3a2233b01d65743886413c1f3c3dd76eee931e31', 'qemuSourceCommit': '0ef7b4e2814b231705d8371dd7997f5b72e70baf', 'sdlSourceCommit': '55b03c7493a7abed33cf803d1380a40fa8af903f', 'binaryOrigin': 'https://dbontr.github.io/linux-lab/qemu/', 'originalBytes': len(data), 'originalSha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'patchedSha256': hashlib.sha256(data).hexdigest(), 'memoryMiB': 1024, 'patch': 'Only shared a.a memory import minimum and maximum changed from 36800 to 16384 pages; byte count unchanged.', 'parts': parts}
(out / 'runtime.json').write_text(json.dumps(metadata, indent=2) + '\n')
(out / 'qemu-system-x86_64.js').write_bytes((ROOT / 'vendor/linux-lab-runtime/out.js').read_bytes())
for filename in ('qemu-system-x86_64.worker.js', 'COPYING.qemu', 'LICENSE.sdl'):
    (out / filename).write_bytes((ROOT / 'vendor/linux-lab-runtime' / filename).read_bytes())
print(json.dumps(metadata, indent=2))
