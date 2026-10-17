"""Read-only RFB client for a private QMP-passed VM display socket.

No keyboard, pointer or clipboard messages are sent. Frames are published only
when every rectangle in a server update has been read and applied.
"""
import socket
import struct
import time
import uuid

from common import QMP, load_session

MAX_WIDTH, MAX_HEIGHT = 4096, 2160
MAX_FRAME_BYTES = MAX_WIDTH * MAX_HEIGHT * 4


def private_display(session):
    """Pass a socketpair over the validated VM QMP endpoint; no VNC listener."""
    load_session(session)
    client, peer = socket.socketpair()
    name = 'brook-record-' + uuid.uuid4().hex
    try:
        with QMP(session) as q:
            q.call('getfd', {'fdname': name}, fd=peer.fileno())
            try:
                q.call('add_client', {'protocol': 'vnc', 'fdname': name, 'skipauth': True})
            except BaseException:
                try: q.call('closefd', {'fdname': name})
                except (OSError, RuntimeError): pass
                raise
        return client
    except BaseException:
        client.close()
        raise
    finally:
        peer.close()


class RfbDisplay:
    def __init__(self, sock, audio=False, audio_callback=None):
        self.sock=sock
        self.audio_requested=audio;self.audio_supported=False
        self.audio_callback=audio_callback
        self.width=self.height=0
        self.pixels=bytearray()
        self.bytes_received=self.updates=self.rectangles=0
        self.geometry_changes=[];self.geometry_change_count=0

    def read(self, count):
        if not 0 <= count <= MAX_FRAME_BYTES:
            raise ValueError('RFB payload exceeds frame limit')
        data=bytearray(count); view=memoryview(data)
        while view:
            got=self.sock.recv_into(view)
            if not got: raise EOFError('VM display stream disconnected')
            self.bytes_received+=got;view=view[got:]
        return data

    def resize(self, width, height):
        if not 0 < width <= MAX_WIDTH or not 0 < height <= MAX_HEIGHT:
            raise ValueError('VM display exceeds supported native geometry')
        self.width,self.height=width,height
        self.pixels=bytearray(width*height*4)
        self.geometry_change_count+=1
        if len(self.geometry_changes)<128:self.geometry_changes.append((width,height))

    def handshake(self):
        if bytes(self.read(12)) != b'RFB 003.008\n':
            raise ValueError('QEMU RFB 3.8 required')
        self.sock.sendall(b'RFB 003.008\n')
        count=self.read(1)[0]
        if not count or 1 not in self.read(count):
            raise RuntimeError('Private QMP client lacks None authentication')
        self.sock.sendall(b'\x01')
        if struct.unpack('>I', self.read(4))[0]:
            raise RuntimeError('Private VM display authentication failed')
        self.sock.sendall(b'\x01')  # Shared: preserve other VM viewers.
        width,height=struct.unpack('>HH',self.read(4))
        self.read(16)  # We request a fixed little-endian BGRX pixel format.
        length=struct.unpack('>I',self.read(4))[0]
        if length > 4096: raise ValueError('Oversized VM display name')
        self.name=bytes(self.read(length)).decode(errors='replace')
        self.resize(width,height)
        pixel_format=struct.pack('>BBBBHHHBBBxxx',32,24,0,1,255,255,255,16,8,0)
        self.sock.sendall(bytes(4)+pixel_format)
        encodings=[0,-223]+([-259] if self.audio_requested else [])
        self.sock.sendall(struct.pack('>BBH',2,0,len(encodings))+
                          b''.join(struct.pack('>i',value) for value in encodings))
        self.request(False)

    def request(self, incremental=True):
        self.sock.sendall(struct.pack('>BBHHHH',3,int(incremental),0,0,self.width,self.height))

    def next_update(self):
        while True:
            kind=self.read(1)[0]
            if kind == 255:
                subtype=self.read(1)[0];opcode=struct.unpack('>H',self.read(2))[0]
                if subtype!=1 or opcode not in (0,1,2):
                    raise ValueError('Unsupported QEMU audio message')
                pcm=b''
                if opcode==2:
                    length=struct.unpack('>I',self.read(4))[0]
                    if length>1024*1024 or length%4:
                        raise ValueError('Invalid QEMU S16 stereo audio extent')
                    pcm=bytes(self.read(length))
                if not self.audio_supported:
                    raise ValueError('QEMU audio arrived before negotiation')
                if self.audio_callback:
                    self.audio_callback(('end','begin','data')[opcode],pcm,time.monotonic())
                continue
            if kind == 2: continue  # Bell.
            if kind == 3:  # Ignore clipboard; never forward it to the host.
                self.read(3);length=struct.unpack('>I',self.read(4))[0]
                if length>65536:raise ValueError('Oversized VM clipboard message')
                self.read(length);continue
            if kind != 0: raise ValueError(f'Unsupported VM display message {kind}')
            self.read(1);count=struct.unpack('>H',self.read(2))[0]
            if count>4096:raise ValueError('Too many VM update rectangles')
            resized=False; changed=False
            for _ in range(count):
                x,y,w,h,encoding=struct.unpack('>HHHHi',self.read(12))
                if encoding == -259:
                    if not self.audio_requested:raise ValueError('Unrequested QEMU audio capability')
                    if not self.audio_supported:
                        self.audio_supported=True
                        # QEMU extension: S16 little-endian, stereo, 48 kHz.
                        self.sock.sendall(struct.pack('>BBHBBI',255,1,2,3,2,48000))
                        self.sock.sendall(struct.pack('>BBH',255,1,0))
                    continue
                if encoding == -223:
                    if x or y:raise ValueError('Invalid VM resize origin')
                    self.resize(w,h);resized=True;continue
                if encoding != 0:raise ValueError(f'Unexpected VM display encoding {encoding}')
                if not w or not h or x+w>self.width or y+h>self.height:
                    raise ValueError('VM display rectangle outside framebuffer')
                raw=self.read(w*h*4)
                for row in range(h):
                    start=((y+row)*self.width+x)*4
                    self.pixels[start:start+w*4]=raw[row*w*4:(row+1)*w*4]
                self.rectangles+=1;changed=True
            self.updates+=1;self.request(not resized)
            # A resize-only update has no complete new image yet. Request one.
            if changed:return self.width,self.height,bytes(self.pixels)
