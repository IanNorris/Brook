#!/usr/bin/env python3
"""Exercise bounded emergency UART output with ready and stuck devices."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#include "panic_serial.h"
#include <cassert>
#include <string>
struct Port {
    unsigned calls=0, readyAfter=0;std::string data;
    bool ready() {return calls++>=readyAfter;}
    void write(uint8_t c) {data+=char(c);}
    void pause() {}
};
int main() {
    brook::PanicSerialState state{};Port p;p.readyAfter=2;
    brook::PanicSerialWrite(state,p,'A',4);
    brook::PanicSerialWrite(state,p,'\n',4);
    assert(!state.failed && p.data=="A\r\n");
    brook::PanicSerialState failed{};Port stuck;stuck.readyAfter=100;
    brook::PanicSerialWrite(failed,stuck,'X',5);
    assert(failed.failed && stuck.calls==5 && stuck.data.empty());
    brook::PanicSerialWrite(failed,stuck,'Y',5);
    assert(stuck.calls==5 && stuck.data.empty());
    brook::PanicSerialState partial{};Port newline;newline.readyAfter=0;
    brook::PanicSerialWrite(partial,newline,'\n',0);
    assert(partial.failed && newline.calls==0 && newline.data.empty());
}
'''
with tempfile.TemporaryDirectory(prefix='brook-panic-serial-') as tmp:
    path=Path(tmp);(path/'test.cpp').write_text(SOURCE)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT/'src/shared/inc_km'),
                    str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('bounded panic serial: PASS')
