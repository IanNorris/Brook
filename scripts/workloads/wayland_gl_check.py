#!/usr/bin/env python3
"""Guest visible hardware GL regression through SDL3 and Wayland DMA-BUFs."""
import argparse
import ctypes as c
import json
import os
import struct
import time


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest',action='store_true',required=True)
    parser.add_argument('--sdl',required=True)
    args=parser.parse_args();sdl=c.CDLL(args.sdl)
    def api(name,restype,*argtypes):
        fn=getattr(sdl,name);fn.restype=restype;fn.argtypes=argtypes;return fn
    error=api('SDL_GetError',c.c_char_p)
    init=api('SDL_Init',c.c_bool,c.c_uint32)
    attribute=api('SDL_GL_SetAttribute',c.c_bool,c.c_int,c.c_int)
    create=api('SDL_CreateWindow',c.c_void_p,c.c_char_p,c.c_int,c.c_int,c.c_uint64)
    context=api('SDL_GL_CreateContext',c.c_void_p,c.c_void_p)
    swap=api('SDL_GL_SwapWindow',c.c_bool,c.c_void_p)
    poll=api('SDL_PollEvent',c.c_bool,c.c_void_p)
    getproc=api('SDL_GL_GetProcAddress',c.c_void_p,c.c_char_p)
    window=ctx=None
    try:
        assert init(0x20),error()
        assert attribute(17,2) and attribute(18,0) and attribute(21,4),error() # GLES2 profile
        window=create(b'Brook hardware GL presentation check',320,240,2);assert window,error()
        ctx=context(window);assert ctx,error()
        def gl(name,restype,*argtypes):
            address=getproc(name.encode());assert address,name
            return c.CFUNCTYPE(restype,*argtypes)(address)
        renderer=gl('glGetString',c.c_char_p,c.c_uint)(0x1f01).decode()
        assert 'virgl' in renderer.lower() and 'llvmpipe' not in renderer.lower(),renderer
        clear_color=gl('glClearColor',None,c.c_float,c.c_float,c.c_float,c.c_float)
        clear=gl('glClear',None,c.c_uint);finish=gl('glFinish',None)
        read=gl('glReadPixels',None,c.c_int,c.c_int,c.c_int,c.c_int,c.c_uint,c.c_uint,c.c_void_p)
        get_error=gl('glGetError',c.c_uint)
        pixel=(c.c_ubyte*4)();event=c.create_string_buffer(128)
        samples=[]
        for frame in range(60):
            while poll(event):
                assert struct.unpack_from('<I',event.raw)[0]!=0x100,'SDL quit during presentation'
            color=tuple(int(i==(frame//20)%3) for i in range(3))
            clear_color(*color,1);clear(0x4000);finish()
            read(0,0,1,1,0x1908,0x1401,pixel)
            assert get_error()==0 and list(pixel[:3])==[value*255 for value in color],list(pixel)
            assert swap(window),error()
            if frame%20==0:samples.append(list(pixel))
            time.sleep(.05)
    finally:
        if ctx:api('SDL_GL_DestroyContext',c.c_bool,c.c_void_p)(ctx)
        if window:api('SDL_DestroyWindow',None,c.c_void_p)(window)
        api('SDL_Quit',None)()
    # An empty owned scene exposes global scene/job counters through the
    # existing stats ABI. It must be the only scene after the SDL app closes.
    libc=c.CDLL(None,use_errno=True);libc.syscall.restype=c.c_long
    def call(number,*values):
        result=libc.syscall(c.c_long(number),*[c.c_long(v) for v in values])
        assert result>=0,(number,c.get_errno());return result
    title=c.create_string_buffer(b'Scene retirement check');output=c.create_string_buffer(16)
    call(506,320,240,c.addressof(title),c.addressof(output),0)
    native_window=struct.unpack('<IIQ',output.raw)[0];scene=-1
    try:
        scene=call(0xB06,native_window,1,0,0,0,0);end=time.monotonic()+15
        while True:
            buffer=c.create_string_buffer(88);call(0xB07,scene,0,c.addressof(buffer),88,0,0)
            stats=struct.unpack('<4I5Q8I',buffer.raw)
            if stats[15]==1 and not stats[13] and not stats[7]:break
            assert time.monotonic()<end,stats;time.sleep(.05)
        assert not any(stats[index] for index in (4,6,9,10,11,12)),stats
    finally:
        if scene>=0:os.close(scene)
        call(507,native_window,0,0,0,0,0)
    print(json.dumps(dict(passed=True,renderer=renderer,presented_frames=60,samples=samples,
                         other_scenes=stats[15]-1,queued_jobs=stats[13],snapshot_bytes=stats[7])))


if __name__=='__main__':main()
