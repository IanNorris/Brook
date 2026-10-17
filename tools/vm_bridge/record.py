#!/usr/bin/env python3
"""Bounded VM-only recording worker. Started by vm_record; never captures a desktop."""
import argparse
from collections import deque
from fractions import Fraction
import json
import math
import os
import resource
from pathlib import Path
import signal
import select
import sys
import socket
import threading
import time

from common import REGISTRY, load_session, process_identity, recording_lock, session_lock, write_json
from rfb import RfbDisplay, private_display

RATE=48000
MAX_AUDIO_BYTES=RATE*4*2


class CaptureBuffer:
    """One latest video frame and two seconds of timestamped audio; never blocks RX."""
    def __init__(self, began):
        self.began=began;self.lock=threading.Lock();self.latest=None;self.sequence=0
        self.audio=deque();self.audio_bytes=0;self.audio_cursor=0;self.segment=None
        self.audio_received_samples=self.audio_dropped_samples=0
        self.activity=[];self.activity_count=0;self.discontinuities=[];self.discontinuity_count=0;self.error=None

    def video(self,width,height,pixels,now):
        with self.lock:
            self.sequence+=1;self.latest=(self.sequence,now,width,height,pixels)

    def audio_event(self,kind,pcm,now):
        with self.lock:
            if kind!='data':
                self.activity_count+=1
                if len(self.activity)<4096:self.activity.append({'kind':kind,'at_s':now-self.began})
                self.segment=None;return
            samples=len(pcm)//4
            if self.segment is None:
                self.audio_cursor=max(self.audio_cursor,round((now-self.began)*RATE)-samples,0)
                self.segment=self.audio_cursor
            # Preserve the sample clock across ordinary packet jitter, but
            # recover after a long source/transport gap without BEGIN/END.
            # QEMU supplies no source timestamps: this is an estimated gap,
            # explicitly reported rather than silently losing all later audio.
            observed_end=max(0,round((now-self.began)*RATE))
            missing=observed_end-(self.audio_cursor+samples)
            if missing>RATE//4:
                self.discontinuity_count+=1
                if len(self.discontinuities)<256:
                    self.discontinuities.append({'at_s':now-self.began,'estimated_gap_samples':missing})
                self.audio_cursor+=missing
            start=self.audio_cursor;self.audio_cursor+=samples
            self.audio_received_samples+=samples
            while self.audio and self.audio_bytes+len(pcm)>MAX_AUDIO_BYTES:
                _,old=self.audio.popleft();self.audio_bytes-=len(old)
                self.audio_dropped_samples+=len(old)//4
            if len(pcm)>MAX_AUDIO_BYTES:
                self.audio_dropped_samples+=samples;return
            self.audio.append((start,pcm));self.audio_bytes+=len(pcm)

    def take(self,limit):
        with self.lock:
            chunks=[]
            while self.audio and self.audio[0][0]<limit:
                start,pcm=self.audio.popleft();self.audio_bytes-=len(pcm)
                size=min(len(pcm)//4,limit-start)
                chunks.append((start,pcm[:size*4]))
                if size*4<len(pcm):
                    rest=pcm[size*4:];self.audio.appendleft((start+size,rest));self.audio_bytes+=len(rest)
                    break
            return self.latest,chunks,self.error


class MediaWriter:
    def __init__(self,path,width,height,fps,audio):
        import av
        self.av=av;self.container=av.open(str(path),'w',format='matroska')
        self.width,self.height,self.fps=width,height,fps
        self.video=self.container.add_stream('libx264',rate=fps)
        self.video.width=width;self.video.height=height;self.video.pix_fmt='yuv420p'
        self.video.codec_context.time_base=Fraction(1,1000000)
        self.video.time_base=Fraction(1,1000000)
        self.video.codec_context.thread_count=2
        self.video.codec_context.max_b_frames=0
        self.video.options={'preset':'veryfast','crf':'16','tune':'zerolatency'}
        self.audio=None;self.resampler=None
        if audio:
            self.audio=self.container.add_stream('aac',rate=RATE)
            self.audio.layout='stereo';self.audio.bit_rate=192000
            self.audio.time_base=Fraction(1,RATE)
            self.resampler=av.AudioResampler(format='fltp',layout='stereo',rate=RATE)
        self.audio_cursor=0;self.silence_samples=self.late_audio_samples=0
        self.frames=0;self.last_pts=-1;self.pts_clamps=0

    def write_video(self,pixels,width,height,seconds,terminal=False):
        if width>self.width or height>self.height:
            raise ValueError('VM mode exceeds recording canvas; no downscaling permitted')
        if len(pixels)!=width*height*4:raise ValueError('Incomplete VM frame')
        frame=self.av.VideoFrame(self.width,self.height,'bgr0')
        stride=frame.planes[0].line_size
        canvas=bytearray(frame.planes[0].buffer_size)
        x=(self.width-width)//2;y=(self.height-height)//2
        for row in range(height):
            dest=(y+row)*stride+x*4
            canvas[dest:dest+width*4]=pixels[row*width*4:(row+1)*width*4]
        frame.planes[0].update(canvas)
        pts=max(0,round(seconds*1000000))
        if pts<=self.last_pts:pts=self.last_pts+1;self.pts_clamps+=1
        frame.pts=pts;frame.time_base=Fraction(1,1000000);self.last_pts=pts
        for packet in self.video.encode(frame):
            if terminal:
                # Matroska needs a nonzero duration representable in its 1ms
                # time base; otherwise it substitutes a whole nominal frame.
                packet.duration=max(1,math.ceil(Fraction(1,1000)/packet.time_base))
            self.container.mux(packet)
        self.frames+=1

    def pcm(self,data):
        if not self.audio or not data:return
        samples=len(data)//4
        frame=self.av.AudioFrame(format='s16',layout='stereo',samples=samples)
        frame.planes[0].update(data);frame.sample_rate=RATE
        frame.pts=self.audio_cursor;frame.time_base=Fraction(1,RATE)
        for converted in self.resampler.resample(frame):
            for packet in self.audio.encode(converted):self.container.mux(packet)
        self.audio_cursor+=samples

    def silence_until(self,limit):
        while self.audio and self.audio_cursor<limit:
            size=min(1024,limit-self.audio_cursor);self.pcm(bytes(size*4));self.silence_samples+=size

    def write_audio(self,chunks,limit):
        if not self.audio:return
        for start,pcm in chunks:
            self.silence_until(min(start,limit))
            late=max(0,self.audio_cursor-start)
            self.late_audio_samples+=min(late,len(pcm)//4)
            self.pcm(pcm[late*4:])
        self.silence_until(limit)

    def close(self):
        try:
            if self.audio:
                for converted in self.resampler.resample(None):
                    for packet in self.audio.encode(converted):self.container.mux(packet)
                for packet in self.audio.encode(None):self.container.mux(packet)
            for packet in self.video.encode(None):self.container.mux(packet)
        finally:self.container.close()


def run(session,recording,duration,fps,width,height,audio):
    data=load_session(session);directory=REGISTRY/session
    descriptor=directory/'video-recording.json';stop_path=directory/f'video-stop-{recording}'
    output=directory/'artifacts'/f'{recording}.mkv';output.parent.mkdir(exist_ok=True)
    partial=output.with_suffix('.partial.mkv');timing=output.with_suffix('.frames.jsonl')
    status={'recording':recording,'session':session,'generation':data['generation'],
            'pid':os.getpid(),'process_identity':process_identity(os.getpid()),
            'state':'starting','artifact':str(output),'partial_artifact':str(partial),
            'timing_artifact':str(timing),'duration_limit_s':duration,'target_fps':fps,
            'width':width,'height':height,'audio_requested':audio,
            'source':'qemu-private-rfb','timing':'host-monotonic transport observation',
            'video_codec':'h264','audio_codec':'aac' if audio else None,
            'canvas_placement':'centered_without_scaling','audio_sample_rate':RATE,
            'audio_channels':2,'max_audio_queue_bytes':MAX_AUDIO_BYTES,
            'source_timestamps_available':False,
            'upstream_frame_drops':None,'upstream_audio_drops':None,
            'speaker_playback_required':False}
    def publish():
        with recording_lock(session):
            owner=json.loads(descriptor.read_text())
            if any(owner.get(key)!=status.get(key) for key in
                   ('recording','generation','pid','process_identity')):
                raise RuntimeError('Recorder claim was superseded')
            write_json(descriptor,status)
    stopped=threading.Event();sock=None;thread=None;writer=None;buffer=None
    def request_stop(*_):stopped.set()
    signal.signal(signal.SIGTERM,request_stop);signal.signal(signal.SIGINT,request_stop)
    try:
        publish()
        sock=private_display(session);sock.settimeout(10)
        # Handshake creates no host input; callbacks are processed by RX only.
        display=RfbDisplay(sock,audio=audio)
        display.handshake();sock.settimeout(None)
        began=time.monotonic();buffer=CaptureBuffer(began);display.audio_callback=buffer.audio_event
        def receive():
            try:
                while not stopped.is_set():
                    w,h,pixels=display.next_update()
                    buffer.video(w,h,pixels,time.monotonic())
            except (OSError,EOFError,ValueError,RuntimeError) as error:
                if not stopped.is_set():
                    with buffer.lock:buffer.error=str(error)
        thread=threading.Thread(target=receive,daemon=True);thread.start()
        writer=MediaWriter(partial,width,height,fps,audio)
        status.update(state='recording',began_monotonic=began)
        last_sequence=0;last_frame_at=-1.;last_pixels=bytes(width*height*4)
        last_width,last_height=width,height;deadline=began;missed_slots=coalesced=0
        heartbeat=0;last_identity_check=0
        with timing.open('x') as log:
            while not stopped.is_set() and not stop_path.exists():
                now=time.monotonic()
                if now-began>=duration:break
                if now-last_identity_check>=1:
                    current=load_session(session)
                    if current['generation']!=data['generation']:raise RuntimeError('VM generation changed')
                    last_identity_check=now
                # Keep 250ms of transport jitter behind the audio writer.
                latest,chunks,error=buffer.take(max(0,round((now-began-.25)*RATE)))
                if error:raise RuntimeError(error)
                writer.write_audio(chunks,max(0,round((now-began-.25)*RATE)))
                if latest and (latest[0]!=last_sequence or now-last_frame_at>=.5):
                    sequence,acquired,w,h,pixels=latest
                    held=sequence==last_sequence
                    if not held:coalesced+=max(0,sequence-last_sequence-1)
                    pts_at=now if held else acquired
                    writer.write_video(pixels,w,h,pts_at-began)
                    last_pixels,last_width,last_height=pixels,w,h
                    last_sequence=sequence;last_frame_at=now
                    log.write(json.dumps({'pts_us':writer.last_pts,'source_sequence':sequence,
                                          'acquired_monotonic':acquired,'held_image':held})+'\n')
                if audio and now-began>5 and not display.audio_supported:
                    raise RuntimeError('QEMU VNC audio unavailable; configure audiodev=hda0')
                if now-heartbeat>=1:
                    status.update(elapsed_s=now-began,video_frames=writer.frames,
                                  audio_received_samples=buffer.audio_received_samples,
                                  audio_supported=display.audio_supported,
                                  heartbeat_monotonic=now)
                    publish();log.flush();heartbeat=now
                deadline+=1/fps;now=time.monotonic()
                if deadline<now:
                    skipped=math.floor((now-deadline)*fps)+1;missed_slots+=skipped;deadline+=skipped/fps
                stopped.wait(max(0,deadline-now))
            ended=time.monotonic();stopped.set()
            sock.shutdown(socket.SHUT_RDWR);thread.join(timeout=2)
            limit=round((ended-began)*RATE)
            latest,chunks,error=buffer.take(limit)
            if error:raise RuntimeError(error)
            writer.write_audio(chunks,limit)
            if latest is None:raise RuntimeError('No complete VM display update received')
            if audio and not display.audio_supported:
                raise RuntimeError('Requested VM audio capability was not negotiated')
            # Explicitly retain the final image through the idle tail. No
            # encoding backlog is collapsed into a shorter output timeline.
            if latest:
                _,_,last_width,last_height,last_pixels=latest
            writer.write_video(last_pixels,last_width,last_height,ended-began,terminal=True)
            log.write(json.dumps({'pts_us':writer.last_pts,'held_image':True,'final':True})+'\n')
            encoded_frames=writer.frames
            silence_samples=writer.silence_samples;late_audio_samples=writer.late_audio_samples
            pts_clamps=writer.pts_clamps
            writer.close();writer=None
            # Exclude a bridge reset between generation validation and artifact
            # promotion, including recordings shorter than the polling period.
            with session_lock(session) as current, recording_lock(session):
                if current['generation']!=data['generation']:
                    raise RuntimeError('VM generation changed before finalization')
                owner=json.loads(descriptor.read_text())
                if any(owner.get(key)!=status.get(key) for key in
                       ('recording','generation','pid','process_identity')):
                    raise RuntimeError('Recorder claim changed before finalization')
                partial.replace(output)
        status.update(state='complete',elapsed_s=ended-began,ended_monotonic=ended,
                      video_frames=encoded_frames,video_pts_clamps=pts_clamps,missed_encoder_slots=missed_slots,
                      audio_silence_filled_samples=silence_samples,audio_late_samples=late_audio_samples,
                      coalesced_source_updates=coalesced,bytes_received=display.bytes_received,
                      source_updates=display.updates,geometry_changes=display.geometry_changes,geometry_change_count=display.geometry_change_count,
                      audio_received_samples=buffer.audio_received_samples,
                      audio_queue_dropped_samples=buffer.audio_dropped_samples,
                      audio_activity=buffer.activity,audio_activity_count=buffer.activity_count,
                      audio_discontinuities=buffer.discontinuities,
                      audio_discontinuity_count=buffer.discontinuity_count,
                      audio_supported=display.audio_supported)
    except BaseException as error:
        status.update(state='failed',error=str(error),ended_monotonic=time.monotonic())
    finally:
        stopped.set()
        if sock:
            try:sock.shutdown(socket.SHUT_RDWR)
            except OSError:pass
            sock.close()
        if thread:thread.join(timeout=2)
        if writer:
            try:writer.close()
            except Exception as error:status['finalize_error']=str(error)
        stop_path.unlink(missing_ok=True)
        usage=resource.getrusage(resource.RUSAGE_SELF)
        status.update(host_cpu_user_s=usage.ru_utime,host_cpu_system_s=usage.ru_stime,
                      host_peak_rss_bytes=usage.ru_maxrss*1024)
        try:publish()
        except (OSError,ValueError,RuntimeError,TimeoutError) as error:
            status.update(state='failed',ownership_error=str(error))
        write_json(output.with_suffix('.json'),status)
    return status


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',required=True);parser.add_argument('--recording',required=True)
    parser.add_argument('--duration',type=float,default=30);parser.add_argument('--fps',type=int,default=30)
    parser.add_argument('--width',type=int,default=1920);parser.add_argument('--height',type=int,default=1080)
    parser.add_argument('--audio',action='store_true');parser.add_argument('--wait-for-parent',action='store_true');args=parser.parse_args()
    if len(args.recording)!=32 or any(c not in '0123456789abcdef' for c in args.recording):parser.error('Invalid recording ID')
    if not 1<=args.duration<=600 or not 1<=args.fps<=60:parser.error('Invalid recording duration/rate')
    if not 32<=args.width<=4096 or not 32<=args.height<=2160 or args.width%2 or args.height%2:parser.error('Invalid even canvas geometry')
    if args.wait_for_parent:
        ready,_,_=select.select([sys.stdin],[],[],10)
        if not ready or sys.stdin.readline(16)!='start\n':raise SystemExit('Recorder parent did not publish ownership')
    result=run(args.session,args.recording,args.duration,args.fps,args.width,args.height,args.audio)
    print(json.dumps(result));raise SystemExit(0 if result['state']=='complete' else 1)
