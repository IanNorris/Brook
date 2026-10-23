#!/usr/bin/env python3
"""Guest-only bounded GLX visual, window-context and pixel-readback check on :0."""
import ctypes as c,json,os,signal
signal.alarm(30)
from pathlib import Path
os.environ['DISPLAY']=':0'
os.environ['__GLX_VENDOR_LIBRARY_NAME']='mesa'
X=c.CDLL(str(Path('/nix/profile/lib/libX11.so.6').resolve(strict=True)),mode=c.RTLD_GLOBAL)
M=c.CDLL(str(Path('/nix/profile/lib/libGLX_mesa.so.0').resolve(strict=True)),mode=c.RTLD_GLOBAL)
G=c.CDLL(str(Path('/nix/profile/lib/libGL.so.1').resolve(strict=True)),mode=c.RTLD_GLOBAL)
def api(lib,name,result,args):
 f=getattr(lib,name);f.restype=result;f.argtypes=args
 if name not in ['XCreateWindow','XMapWindow','XSync','glXCreateContext','glXMakeCurrent','glClear','glFinish','glReadPixels']:return f
 def logged(*values):
  print('CALL',name,flush=True);value=f(*values);print('RETURN',name,flush=True);return value
 return logged
P=c.c_void_p;I=c.c_int;U=c.c_ulong
openD=api(X,'XOpenDisplay',P,[c.c_char_p]);closeD=api(X,'XCloseDisplay',I,[P]);free=api(X,'XFree',I,[P]);sync=api(X,'XSync',I,[P,I]);screen=api(X,'XDefaultScreen',I,[P])
class Err(c.Structure):_fields_=[('type',I),('display',P),('resource',U),('serial',U),('error',c.c_ubyte),('request',c.c_ubyte),('minor',c.c_ubyte)]
errors=[]
@c.CFUNCTYPE(I,P,c.POINTER(Err))
def onerror(d,e):
 errors.append(dict(error=e.contents.error,request=e.contents.request,minor=e.contents.minor));return 0
api(X,'XSetErrorHandler',P,[P])(c.cast(onerror,P))
d=openD(b':0');assert d,'XOpenDisplay';result=dict(display=':0',errors=errors)
try:
 e=I();v=I();result['extension']=bool(api(G,'glXQueryExtension',I,[P,c.POINTER(I),c.POINTER(I)])(d,c.byref(e),c.byref(v)))
 major=I();minor=I();result['version_ok']=bool(api(G,'glXQueryVersion',I,[P,c.POINTER(I),c.POINTER(I)])(d,c.byref(major),c.byref(minor)));result['version']=[major.value,minor.value]
 attrs=(I*11)(4,8,8,9,8,10,8,12,24,0,0)
 vi=api(G,'glXChooseVisual',P,[P,I,c.POINTER(I)])(d,screen(d),attrs);result['rgba_depth24_visual']=bool(vi)
 if vi:
  class Visual(c.Structure):_fields_=[('visual',P),('id',U),('screen',I),('depth',I),('klass',I),('red',U),('green',U),('blue',U),('size',I),('bits',I)]
  class Attr(c.Structure):_fields_=[('bgmap',U),('bg',U),('bordermap',U),('border',U),('bitgravity',I),('wingravity',I),('backing',I),('planes',U),('pixel',U),('save',I),('events',c.c_long),('propagate',c.c_long),('override',I),('colormap',U),('cursor',U)]
  visual=c.cast(vi,c.POINTER(Visual)).contents
  root=api(X,'XRootWindow',U,[P,I])(d,screen(d))
  cmap=api(X,'XCreateColormap',U,[P,U,P,I])(d,root,visual.visual,0)
  attr=Attr();attr.colormap=cmap
  win=api(X,'XCreateWindow',U,[P,U,I,I,c.c_uint,c.c_uint,c.c_uint,I,c.c_uint,P,U,P])(d,root,20,20,320,240,0,visual.depth,1,visual.visual,(1<<3)|(1<<13),c.byref(attr))
  api(X,'XStoreName',I,[P,U,c.c_char_p])(d,win,b'Brook GLX render check')
  api(X,'XMapWindow',I,[P,U])(d,win);sync(d,0)
  ctx=api(G,'glXCreateContext',P,[P,P,P,I])(d,vi,None,1);result['context']=bool(ctx)
  result['current']=bool(ctx and api(G,'glXMakeCurrent',I,[P,U,P])(d,win,ctx))
  if result['current']:
   get=api(G,'glGetString',c.c_char_p,[c.c_uint]);result['renderer']=get(0x1F01).decode();result['gl_version']=get(0x1F02).decode()
   error=api(G,'glGetError',c.c_uint,[]);result['initial_gl_error']=error()
   api(G,'glDrawBuffer',None,[c.c_uint])(0x404);api(G,'glReadBuffer',None,[c.c_uint])(0x404)
   api(G,'glClearColor',None,[c.c_float]*4)(1,0,0,1);api(G,'glClear',None,[c.c_uint])(0x4000);result['clear_error']=error()
   api(G,'glFinish',None,[])()
   pixel=(c.c_ubyte*4)();api(G,'glReadPixels',None,[I,I,I,I,c.c_uint,c.c_uint,P])(0,0,1,1,0x1908,0x1401,pixel);result['pixel']=list(pixel);result['read_error']=error()
   api(G,'glXMakeCurrent',I,[P,U,P])(d,0,None)
  if ctx:api(G,'glXDestroyContext',None,[P,P])(d,ctx)
  api(X,'XDestroyWindow',I,[P,U])(d,win);api(X,'XFreeColormap',I,[P,U])(d,cmap);free(vi)
 sync(d,0)
finally:closeD(d)
print(json.dumps(result),flush=True)
assert result.get('current') and result.get('pixel') == [255, 0, 0, 255], result
assert not errors and all(result.get(k) == 0 for k in ['initial_gl_error','clear_error','read_error']), result
