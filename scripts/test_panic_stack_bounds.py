#!/usr/bin/env python3
"""Check overflow and boundary handling for panic stack reads."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#include "panic_stack.h"
#include <cassert>
int main() {
    brook::PanicStackBounds b{0xffffc00000001000ULL,0xffffc00000002000ULL};
    assert(b.Contains(b.base,16) && b.Contains(b.top-16,16));
    assert(!b.Contains(b.top-15,16) && !b.Contains(b.top,1));
    assert(!b.Contains(b.base-1,1) && !b.Contains(b.base,0));
    assert(b.Available(b.top-5,256)==5 && b.Available(b.base,256)==256);
    assert(!b.Contains(~uint64_t(0)-3,16));
    brook::PanicStackBounds empty{};
    assert(!empty.Contains(0xffffc00000001000ULL,16));
}
'''
with tempfile.TemporaryDirectory(prefix='brook-panic-stack-') as tmp:
    root=Path(tmp);(root/'test.cpp').write_text(SOURCE)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT/'src/kernel/src'),
                    str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('panic stack bounds: PASS')
