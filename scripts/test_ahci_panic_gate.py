#!/usr/bin/env python3
"""Race panic admission against ordinary AHCI DMA ownership."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='brook-ahci-panic-gate-') as temp:
    binary=Path(temp)/'test'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O2','-Wall','-Wextra',
                    '-Werror','-pthread',str(root/'src/tests/test_ahci_panic_gate/main.cpp'),
                    '-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=30)
print('AHCI panic gate: PASS')
