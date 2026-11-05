#!/usr/bin/env python3
"""Host regression: a headless panic emits a decodable structured record."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix='brook-panic-headless-') as tmp:
    build = Path(tmp)
    qrcode = ROOT / 'src/third_party/qrcodegen'
    lz4 = ROOT / 'src/third_party/lz4'
    for source, output in ((qrcode / 'qrcodegen.c', 'qrcode.o'),
                           (lz4 / 'lz4.c', 'lz4.o')):
        subprocess.run([os.environ.get('CC', 'cc'), '-c', str(source),
                        '-o', str(build / output)], check=True)
    subprocess.run([
        os.environ.get('CXX', 'c++'), '-std=c++17',
        '-iquote', str(ROOT / 'src/kernel/src'),
        '-I', str(ROOT / 'src/shared/inc_km'), '-I', str(qrcode), '-I', str(lz4),
        str(ROOT / 'src/tests/test_panic_headless/main.cpp'),
        str(ROOT / 'src/kernel/src/panic_qr.cpp'),
        str(ROOT / 'src/kernel/src/base45.cpp'),
        str(build / 'qrcode.o'), str(build / 'lz4.o'),
        '-o', str(build / 'test'),
    ], check=True)
    subprocess.run([str(build / 'test')], check=True)
