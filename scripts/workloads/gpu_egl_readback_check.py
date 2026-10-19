#!/usr/bin/env python3
"""Guest-only, hardware VirGL surfaceless EGL clear/readback regression."""
import argparse
import ctypes as c
import json
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest', action='store_true', required=True)
    parser.add_argument('--mesa', type=Path, required=True)
    parser.add_argument('--glvnd', type=Path, required=True)
    args = parser.parse_args()
    os.environ.update(EGL_PLATFORM='surfaceless', MESA_LOADER_DRIVER_OVERRIDE='virtio_gpu',
                      LIBGL_DRIVERS_PATH=str(args.mesa / 'lib/dri'),
                      __EGL_VENDOR_LIBRARY_FILENAMES=str(args.mesa / 'share/glvnd/egl_vendor.d/50_mesa.json'),
                      LIBGL_ALWAYS_SOFTWARE='0', MESA_SHADER_CACHE_DISABLE='true')
    egl = c.CDLL(str(args.glvnd / 'lib/libEGL.so.1'))
    gl = c.CDLL(str(args.glvnd / 'lib/libGLESv2.so.2'))
    def bind(lib, name, result, *arguments):
        fn = getattr(lib, name);fn.restype=result;fn.argtypes=list(arguments);return fn
    pointer = c.c_void_p
    integer = c.c_int
    uint = c.c_uint
    ints = c.POINTER(integer)
    display = bind(egl, 'eglGetPlatformDisplay', pointer, uint, pointer, pointer)(0x31dd, None, None)
    assert display
    initialize = bind(egl, 'eglInitialize', uint, pointer, ints, ints)
    major, minor = integer(), integer()
    assert initialize(display, c.byref(major), c.byref(minor))
    assert bind(egl, 'eglBindAPI', uint, uint)(0x30a0)
    attributes=(integer*13)(0x3033,1,0x3040,4,0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3038)
    config, count=pointer(), integer()
    assert bind(egl, 'eglChooseConfig', uint, pointer, ints, c.POINTER(pointer), integer, ints)(display,attributes,c.byref(config),1,c.byref(count)) and count.value
    context=bind(egl, 'eglCreateContext', pointer, pointer, pointer, pointer, ints)(display,config,None,(integer*3)(0x3098,2,0x3038))
    assert context
    surface=bind(egl, 'eglCreatePbufferSurface', pointer, pointer, pointer, ints)(display,config,(integer*5)(0x3057,64,0x3056,64,0x3038))
    assert surface
    current=bind(egl, 'eglMakeCurrent', uint, pointer, pointer, pointer, pointer)
    assert current(display,surface,surface,context)
    renderer=bind(gl,'glGetString',c.c_char_p,uint)(0x1f01).decode()
    assert 'virgl' in renderer.lower() and 'llvmpipe' not in renderer.lower(),renderer
    clear_color=bind(gl,'glClearColor',None,c.c_float,c.c_float,c.c_float,c.c_float)
    clear=bind(gl,'glClear',None,uint)
    finish=bind(gl,'glFinish',None)
    read=bind(gl,'glReadPixels',None,integer,integer,integer,integer,uint,uint,pointer)
    get_error=bind(gl,'glGetError',uint)
    pixels=[]
    for rgba in ((1,0,0,1),(0,1,0,1),(0,0,1,1)):
        clear_color(*rgba);clear(0x4000);finish()
        pixel=(c.c_ubyte*4)();read(31,31,1,1,0x1908,0x1401,pixel)
        assert get_error()==0
        assert list(pixel)==[channel*255 for channel in rgba],list(pixel)
        pixels.append(list(pixel))
    assert current(display,None,None,None)
    assert bind(egl,'eglDestroySurface',uint,pointer,pointer)(display,surface)
    assert bind(egl,'eglDestroyContext',uint,pointer,pointer)(display,context)
    assert bind(egl,'eglTerminate',uint,pointer)(display)
    print(json.dumps(dict(passed=True,renderer=renderer,pixels=pixels)),flush=True)


if __name__=='__main__':
    main()
