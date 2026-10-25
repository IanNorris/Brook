#!/usr/bin/env python3
"""Compile the patched production axis decoder with controlled XI event classes.

This imports the real SDL struct, extraction loop, raw decoder and DeviceChanged
case. Only SDL event delivery/focus is stubbed; no display or host input is used.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', required=True, type=Path)
a = p.parse_args()
source = (a.source / 'src/video/x11/SDL_x11xinput2.c').read_text()
header = (a.source / 'src/video/x11/SDL_x11mouse.h').read_text()


def function(name):
    match = re.search(r'static void ' + re.escape(name) + r'\(', source)
    assert match, name
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


struct = re.search(r'typedef struct SDL_XInput2DeviceInfo\s*\{.*?\} SDL_XInput2DeviceInfo;', header, re.S)
case = re.search(r'    case XI_DeviceChanged:\n    \{.*?\n    \} break;', source, re.S)
assert struct and case
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <X11/extensions/XInput2.h>
typedef unsigned SDL_MouseID;
typedef struct { void *InputTransform; bool enable_relative_system_scale,relative_mode; void *focus; } SDL_Mouse;
static SDL_Mouse mouse={.relative_mode=true,.focus=(void*)1};
static float out_x,out_y;
static SDL_Mouse *SDL_GetMouse(void) { return &mouse; }
static void *SDL_GetKeyboardFocus(void) { return mouse.focus; }
static void SDL_SendMouseMotion(unsigned long time,void *focus,SDL_MouseID id,bool relative,float x,float y) {
    (void)time;(void)focus;assert(id==7 && relative);out_x=x;out_y=y;
}
static Atom xinput2_rel_x_atom=1,xinput2_rel_y_atom=2,xinput2_abs_x_atom=3,xinput2_abs_y_atom=4;
'''
code += struct.group(0) + '\n'
code += 'typedef struct { SDL_XInput2DeviceInfo *mouse_device_info; } SDL_VideoData;\n'
code += function('xinput2_update_relative_axes') + '\n' + function('parse_relative_valuators') + '\n'
code += 'static void changed(SDL_VideoData *videodata,XGenericEventCookie *cookie) { switch(cookie->evtype) {\n'
code += case.group(0) + '\ndefault: assert(0); } }\n'
code += r'''
static void metadata(SDL_VideoData *video,int mode,int device) {
    XIValuatorClassInfo x={.type=XIValuatorClass,.number=0,.mode=mode,.label=mode==XIModeRelative?1:3};
    XIValuatorClassInfo y={.type=XIValuatorClass,.number=1,.mode=mode,.label=mode==XIModeRelative?2:4};
    XIAnyClassInfo *classes[]={(XIAnyClassInfo*)&x,(XIAnyClassInfo*)&y};
    XIDeviceChangedEvent event={.deviceid=device,.classes=classes,.num_classes=2};
    XGenericEventCookie cookie={.evtype=XI_DeviceChanged,.data=&event};
    changed(video,&cookie);
    // Event memory need not survive until RawMotion is processed.
    memset(&x,0,sizeof(x));memset(&y,0,sizeof(y));
}
static void raw(SDL_XInput2DeviceInfo *device,double x,double y,float expected_x,float expected_y) {
    unsigned char mask=3;double values[]={x,y};
    XIRawEvent event={.sourceid=7,.deviceid=2,.valuators={.mask=&mask,.mask_len=1,.values=values},.raw_values=values};
    parse_relative_valuators(device,&event);assert(out_x==expected_x && out_y==expected_y);
}
int main(void) {
    SDL_XInput2DeviceInfo device={.device_id=2};SDL_VideoData video={.mouse_device_info=&device};
    metadata(&video,XIModeAbsolute,2);
    raw(&device,918,462,0,0);raw(&device,923,466,5,4);
    // Even if the server is already absolute again, queued relative motion
    // must use the classes in its preceding DeviceChanged event, not re-query.
    metadata(&video,XIModeRelative,2);
    float total_x=0,total_y=0;
    for(unsigned i=0;i<20;++i) {
        raw(&device,127,-100,127,-100);total_x+=out_x;total_y+=out_y;
        raw(&device,73,0,73,0);total_x+=out_x;total_y+=out_y;
    }
    assert(total_x==4000 && total_y==-2000);
    metadata(&video,XIModeAbsolute,2);raw(&device,10000,-10000,0,0);raw(&device,10003,-9998,3,2);
    metadata(&video,XIModeRelative,2);raw(&device,8,-9,8,-9);
    metadata(&video,XIModeAbsolute,99);raw(&device,8,-9,8,-9); // Other device cannot reset this cache.
    puts("SDL_XINPUT_AXES_PASS production event classes, raw decoder, queued relative/absolute transitions, baseline reset and event lifetimes");
}
'''
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'test.c').write_text(code)
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'xi'], text=True))
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c99', '-Wall', '-Wextra', '-Werror', *flags, str(root / 'test.c'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
