#!/usr/bin/env python3
"""Fault-inject the fixed-sector panic journal on a disposable memory disk."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='brook-panic-journal-') as temp:
    binary = Path(temp) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', str(root / 'src/tests/test_panic_journal/main.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('panic journal: PASS')
