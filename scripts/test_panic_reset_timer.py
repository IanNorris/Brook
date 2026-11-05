#!/usr/bin/env python3
"""Check the panic delay's bounded failures and single reset attempt."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='brook-panic-timer-') as temp:
    binary = Path(temp) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall',
                    '-Wextra', '-Werror', str(root / 'src/tests/test_panic_reset_timer/main.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('panic reset timer: PASS')
