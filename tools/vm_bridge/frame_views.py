"""Saved screenshot views: one native source, small previews and later native crops.

Only bridge-owned RGB PNGs and run-scoped metadata are accepted. Pillow improves
resampling when available; the bounded stdlib codec/box fallback needs no package.
"""
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import time
import uuid
import zlib
from common import write_json
from jobs import validate_id
from media import scoped_path

WIDTHS={'tiny':320,'medium':960}
MAX_FILE=64*1024*1024

# Small fixed glyphs keep diagnostic rulers available without host fonts/Pillow.
GLYPHS={c:rows.split('/') for c,rows in {
    '0':'111/101/101/101/111','1':'010/110/010/010/111',
    '2':'111/001/111/100/111','3':'111/001/111/001/111',
    '4':'101/101/111/001/001','5':'111/100/111/001/111',
    '6':'111/100/111/101/111','7':'111/001/010/010/010',
    '8':'111/101/111/101/111','9':'111/101/111/001/111',
    'x':'000/101/010/101/000','y':'101/101/111/001/111',
    '=':'000/111/000/111/000',' ':'000/000/000/000/000'}.items()}


def coordinate_grid(rgb,w,h,crop):
    """Annotate display pixels with native coordinates; never changes geometry."""
    x,y,cw,ch=crop;sx=w/cw;sy=h/ch;out=bytearray(rgb)
    desired=max(96/sx,64/sy);power=10**max(0,len(str(int(desired)))-1)
    step=next(n*power for n in (1,2,5,10) if n*power>=desired)
    def pixel(px,py,color):
        if 0<=px<w and 0<=py<h:
            offset=(py*w+px)*3;out[offset:offset+3]=color
    def label(px,py,text):
        # Dark backing makes coordinates readable over any captured content.
        width=len(text)*8
        px=max(0,min(px,w-width));py=max(0,min(py,h-12))
        for dy in range(12):
            for dx in range(width):pixel(px+dx,py+dy,b'\x10\x18\x20')
        for i,c in enumerate(text):
            for gy,row in enumerate(GLYPHS[c]):
                for gx,bit in enumerate(row):
                    if bit=='1':
                        for dy in range(2):
                            for dx in range(2):pixel(px+i*8+gx*2+dx,py+1+gy*2+dy,b'\xff\xff\xff')
    xs=list(range(((x+step-1)//step)*step,x+cw,step))
    ys=list(range(((y+step-1)//step)*step,y+ch,step))
    for native in xs:
        px=round((native-x)*sx)
        for py in range(h):
            if py%6<3:
                pixel(px-1,py,b'\x10\x18\x20');pixel(px,py,b'\x40\xe0\xff')
    for native in ys:
        py=round((native-y)*sy)
        for px in range(w):
            if px%6<3:
                pixel(px,py-1,b'\x10\x18\x20');pixel(px,py,b'\x40\xe0\xff')
    for native in xs:
        px=round((native-x)*sx)
        if px>=64:label(px+2,2,'x='+str(native))
    for native in ys:
        py=round((native-y)*sy)
        if py>=28:label(2,py+2,'y='+str(native))
    label(2,2,'x='+str(x));label(2,15,'y='+str(y))
    return bytes(out),{'kind':'coordinate_grid','coordinate_space':'native_frame',
        'native_step':step,'origin':[x,y],'x_ticks':xs,'y_ticks':ys,
        'note':'Diagnostic overlay; clean source pixels are retained'}


def decode_rgb(blob):
    if not blob.startswith(b'\x89PNG\r\n\x1a\n'):raise ValueError('Saved source is not PNG')
    at=8;payload=[];extent=None;ended=False
    while at+12<=len(blob):
        size=struct.unpack('>I',blob[at:at+4])[0];kind=blob[at+4:at+8];end=at+8+size
        if end+4>len(blob):raise ValueError('Incomplete PNG chunk')
        data=blob[at+8:end]
        if zlib.crc32(kind+data)&0xffffffff!=struct.unpack('>I',blob[end:end+4])[0]:raise ValueError('PNG checksum mismatch')
        if kind==b'IHDR':
            if extent is not None or len(data)!=13:raise ValueError('Invalid PNG header')
            w,h,depth,color,compression,filtering,interlace=struct.unpack('>IIBBBBB',data)
            if not 0<w<=4096 or not 0<h<=2160 or (depth,color,compression,filtering,interlace)!=(8,2,0,0,0):raise ValueError('Unsupported saved frame encoding')
            extent=(w,h)
        elif kind==b'IDAT':payload.append(data)
        elif kind==b'IEND':ended=True;break
        at=end+4
    if not extent or not ended:raise ValueError('Incomplete PNG')
    w,h=extent;expected=(w*3+1)*h;decoder=zlib.decompressobj()
    raw=decoder.decompress(b''.join(payload),expected+1)
    if len(raw)!=expected or not decoder.eof or decoder.unconsumed_tail or decoder.unused_data:raise ValueError('Invalid PNG payload size')
    stride=w*3+1;rgb=bytearray(w*h*3)
    for y in range(h):
        if raw[y*stride]!=0:raise ValueError('Saved frame requires an unsupported PNG filter')
        rgb[y*w*3:(y+1)*w*3]=raw[y*stride+1:(y+1)*stride]
    return w,h,bytes(rgb)


def render(rgb,w,h,crop,width):
    x,y,cw,ch=crop;iw=min(cw,width or cw);ih=max(1,round(ch*iw/cw))
    if (x,y,cw,ch,iw,ih)==(0,0,w,h,w,h):return bytes(rgb),iw,ih,'native'
    if (iw,ih)==(cw,ch):
        pixels=b''.join(rgb[((y+dy)*w+x)*3:((y+dy)*w+x+cw)*3] for dy in range(ch))
        return pixels,iw,ih,'native_crop'
    try:
        from PIL import Image
    except ImportError:Image=None
    if Image is not None:
        image=Image.frombytes('RGB',(w,h),bytes(rgb)).crop((x,y,x+cw,y+ch))
        if image.size!=(iw,ih):image=image.resize((iw,ih),getattr(Image,'Resampling',Image).LANCZOS)
        return image.tobytes(),iw,ih,'pillow_lanczos' if (iw,ih)!=(cw,ch) else 'native_crop'
    out=bytearray(iw*ih*3)
    # Bounded block averages preserve more small text than nearest-neighbour.
    for dy in range(ih):
        y0=y+dy*ch//ih;y1=y+max(dy*ch//ih+1,(dy+1)*ch//ih)
        for dx in range(iw):
            x0=x+dx*cw//iw;x1=x+max(dx*cw//iw+1,(dx+1)*cw//iw)
            count=(y1-y0)*(x1-x0);sums=[0,0,0]
            for sy in range(y0,y1):
                begin=(sy*w+x0)*3;end=(sy*w+x1)*3
                for c in range(3):sums[c]+=sum(rgb[begin+c:end:3])
            offset=(dy*iw+dx)*3
            for c in range(3):out[offset+c]=sums[c]//count
    return bytes(out),iw,ih,'stdlib_box' if (iw,ih)!=(cw,ch) else 'native_crop'


class FrameViews:
    def __init__(self,registry,session,encode,artifact_factory):
        self.registry=Path(registry);self.session=session;self.encode=encode;self.artifact_factory=artifact_factory
        run=self.registry/session;self.directory=run/'frames'
        if run.is_symlink() or self.directory.is_symlink():raise ValueError('Frame metadata escapes its run')
        self.directory.mkdir(mode=0o700,exist_ok=True)
        if self.directory.stat().st_uid!=os.getuid():raise ValueError('Frame metadata owner mismatch')
    def index(self,ident):
        validate_id(ident);return self.directory/(ident+'.json')
    def metadata(self,ident):
        path=self.index(ident)
        try:
            with os.fdopen(os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK),'rb') as f:
                info=os.fstat(f.fileno())
                if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid() or info.st_size>65536:raise ValueError('Invalid frame metadata file')
                data=json.load(f)
        except FileNotFoundError as error:raise ValueError('Saved frame metadata unavailable; capture a new frame') from error
        if data.get('frame_id')!=ident:raise ValueError('Frame metadata identity mismatch')
        validate_id(data['source_frame_id'])
        return data
    def image(self,data):
        path=Path(data['artifact']);expected=scoped_path(self.registry,self.session,path.name)
        if path.absolute()!=expected.absolute():raise ValueError('Saved frame artifact escapes its run')
        with os.fdopen(os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK),'rb') as f:
            info=os.fstat(f.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid() or info.st_size>MAX_FILE:raise ValueError('Invalid saved frame artifact')
            blob=f.read(MAX_FILE+1)
        if hashlib.sha256(blob).hexdigest()!=data['sha256']:raise ValueError('Saved frame artifact checksum mismatch')
        return blob
    @staticmethod
    def crop(root,crop,max_width):
        w,h=root['coordinate_width'],root['coordinate_height']
        if crop is None:crop={'x':0,'y':0,'width':w,'height':h}
        if not isinstance(crop,dict) or set(crop)!={'x','y','width','height'} or any(isinstance(v,bool) or not isinstance(v,int) for v in crop.values()):raise ValueError('Crop requires integer native x/y/width/height')
        x,y,cw,ch=(crop[k] for k in ['x','y','width','height'])
        if not 0<=x<w or not 0<=y<h or not 0<cw<=w-x or not 0<ch<=h-y:raise ValueError('Crop outside native frame')
        if max_width is not None and (isinstance(max_width,bool) or not isinstance(max_width,int) or not 32<=max_width<=4096):raise ValueError('max_width must be 32–4096')
        return x,y,cw,ch
    def write(self,data,image,publish_index=True):
        path=self.artifact_factory('.png');expected=scoped_path(self.registry,self.session,path.name)
        if path.absolute()!=expected.absolute():raise ValueError('Frame artifact escapes its run')
        temporary=path.with_suffix('.png.tmp')
        try:
            with temporary.open('xb') as f:os.chmod(temporary,0o600);f.write(image)
            temporary.replace(path)
        finally:temporary.unlink(missing_ok=True)
        data={**data,'artifact':str(path),'sha256':hashlib.sha256(image).hexdigest()}
        if publish_index:write_json(self.index(data['frame_id']),data)
        return data
    def derive(self,root,crop,max_width,rgb=None,overlay=False):
        x,y,cw,ch=self.crop(root,crop,max_width);iw=min(cw,max_width or cw);ih=max(1,round(ch*iw/cw))
        if not overlay and (x,y,cw,ch,iw,ih)==(0,0,root['width'],root['height'],root['width'],root['height']):return dict(root)
        spec=f"{root['frame_id']}:{x}:{y}:{cw}:{ch}:{iw}:{ih}"+(':grid2' if overlay else '')
        ident=hashlib.sha256(spec.encode()).hexdigest()[:32]
        if self.index(ident).exists():
            saved=self.metadata(ident)
            if saved.get('view_spec')!=spec:raise ValueError('Frame view identity collision')
            self.image(saved);return saved
        w,h=root['coordinate_width'],root['coordinate_height']
        if rgb is None:
            decoded_w,decoded_h,rgb=decode_rgb(self.image(root))
            if (w,h)!=(decoded_w,decoded_h):raise ValueError('Saved native extent mismatch')
        pixels,iw,ih,backend=render(rgb,w,h,(x,y,cw,ch),max_width)
        data={k:v for k,v in root.items() if k not in {'previews','artifact','sha256'}}
        data.update(frame_id=ident,view_spec=spec,width=iw,height=ih,crop_origin=[x,y],crop_width=cw,crop_height=ch,
            scale=iw/cw,scale_y=ih/ch,derived_host_monotonic=time.monotonic(),resampler=backend)
        if overlay:
            pixels,data['overlay']=coordinate_grid(pixels,iw,ih,(x,y,cw,ch))
        return self.write(data,self.encode(iw,ih,pixels))
    @staticmethod
    def reference(data):
        fields=('frame_id','source_frame_id','artifact','sha256','width','height',
            'coordinate_width','coordinate_height','crop_origin','crop_width','crop_height','scale','scale_y',
            'generation','guest_boot_id','frame_epoch','captured_monotonic')
        return {key:data[key] for key in fields if key in data}
    def publish(self,rgb,data,crop=None,max_width=None):
        w,h=data['coordinate_width'],data['coordinate_height'];self.crop(data,crop,max_width)
        ident=uuid.uuid4().hex
        root={**data,'frame_id':ident,'source_frame_id':ident,'view_api':1,'width':w,'height':h,
            'crop_origin':[0,0],'crop_width':w,'crop_height':h,'scale':1,'scale_y':1,'resampler':'native'}
        root=self.write(root,self.encode(w,h,rgb),publish_index=False)
        root['previews']={name:self.reference(self.derive(root,None,width,rgb)) for name,width in WIDTHS.items()}
        root['previews_scope']='native_frame'
        selected=self.derive(root,crop,max_width,rgb)
        selected['previews']=root['previews'];selected['previews_scope']='native_frame'
        write_json(self.index(ident),root)
        if selected['frame_id']!=ident:write_json(self.index(selected['frame_id']),selected)
        return {'data':selected,'image':self.image(selected)}
    def view(self,ident,crop=None,max_width=None,size='full',overlay=False):
        if size not in {'full',*WIDTHS}:raise ValueError('size must be tiny, medium or full')
        if not isinstance(overlay,bool):raise ValueError('overlay must be boolean')
        if max_width is not None and size!='full':raise ValueError('Use size or max_width, not both')
        reference=self.metadata(ident);root=self.metadata(reference['source_frame_id'])
        if root['frame_id']!=root['source_frame_id']:raise ValueError('Invalid native source identity')
        result=self.derive(root,crop,WIDTHS.get(size,max_width),overlay=overlay)
        result['previews']=root.get('previews',{});result['previews_scope']='native_frame'
        result['vm_capture_performed']=False
        return {'data':result,'image':self.image(result)}
