"""Recording transport and timeline invariants, independent of a live VM."""
import array
import contextlib
import io
import importlib.util
import json
import os
import socket
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch

from common import QMP, process_identity, process_is_alive
from launch import recording_display_args
from record import CaptureBuffer
from rfb import RfbDisplay


def update(rectangles):
    return bytes(2)+struct.pack('>H',len(rectangles))+b''.join(
        struct.pack('>HHHHi',x,y,w,h,encoding)+payload
        for x,y,w,h,encoding,payload in rectangles)


class Wire:
    def __init__(self,data):self.data=io.BytesIO(data);self.sent=bytearray()
    def recv_into(self,view):
        # Fragment headers and pixels to exercise exact-read handling.
        chunk=self.data.read(min(3,len(view)));view[:len(chunk)]=chunk;return len(chunk)
    def sendall(self,data):self.sent.extend(data)


class RfbTests(unittest.TestCase):
    def connection(self,payload,audio=False):
        header=b'RFB 003.008\n'+bytes([1,1])+bytes(4)+struct.pack('>HH',2,2)+bytes(16)+struct.pack('>I',4)+b'QEMU'
        wire=Wire(header+payload);display=RfbDisplay(wire,audio=audio);display.handshake()
        return display,wire

    def test_fragmented_complete_rectangle_and_partial_update(self):
        first=bytes(range(16));second=b'abcd'
        r,_=self.connection(update([(0,0,2,2,0,first)])+update([(1,1,1,1,0,second)]))
        self.assertEqual(r.next_update(),(2,2,first))
        self.assertEqual(r.next_update(),(2,2,first[:12]+second))

    def test_truncated_update_is_never_published(self):
        r,_=self.connection(update([(0,0,2,2,0,bytes(15))]))
        with self.assertRaises(EOFError):r.next_update()
        self.assertEqual(r.updates,0)

    def test_rectangle_bounds_checked_before_pixel_read(self):
        r,_=self.connection(update([(1,1,2,2,0,b'')]))
        with self.assertRaisesRegex(ValueError,'outside framebuffer'):r.next_update()

    def test_resize_only_update_waits_for_new_pixels(self):
        r,_=self.connection(update([(0,0,1,1,-223,b'')])+update([(0,0,1,1,0,b'abcd')]))
        self.assertEqual(r.next_update(),(1,1,b'abcd'))
        self.assertEqual(r.updates,2)

    def test_audio_negotiation_and_direction_specific_opcodes(self):
        ack=update([(0,0,0,0,-259,b'')])
        audio=struct.pack('>BBH',255,1,1)+struct.pack('>BBHI',255,1,2,8)+b'abcdefgh'+struct.pack('>BBH',255,1,0)
        r,wire=self.connection(ack+audio+update([(0,0,2,2,0,bytes(16))]),True)
        events=[];r.audio_callback=lambda kind,pcm,now:events.append((kind,pcm))
        r.next_update()
        self.assertTrue(r.audio_supported)
        self.assertEqual(events,[('begin',b''),('data',b'abcdefgh'),('end',b'')])
        self.assertIn(struct.pack('>BBHBBI',255,1,2,3,2,48000),wire.sent)
        self.assertIn(struct.pack('>BBH',255,1,0),wire.sent)

    def test_unnegotiated_audio_rejected(self):
        r,_=self.connection(struct.pack('>BBH',255,1,1))
        with self.assertRaisesRegex(ValueError,'before negotiation'):r.next_update()


class AudioTimelineTests(unittest.TestCase):
    def test_idle_gap_preserved_across_activity_segments(self):
        b=CaptureBuffer(10)
        b.audio_event('begin',b'',11)
        b.audio_event('data',bytes(480*4),11.01)
        b.audio_event('end',b'',11.02)
        b.audio_event('begin',b'',12)
        b.audio_event('data',bytes(480*4),12.01)
        _,chunks,_=b.take(200000)
        self.assertEqual([start for start,_ in chunks],[48000,96000])
        self.assertEqual(b.audio_received_samples,960)

    def test_encoder_queue_overflow_preserves_source_sample_positions(self):
        b=CaptureBuffer(0)
        for i in range(5):b.audio_event('data',bytes(48000*4),i+1)
        self.assertEqual(b.audio_dropped_samples,144000)
        _,chunks,_=b.take(300000)
        self.assertEqual([start for start,_ in chunks],[144000,192000])
        self.assertLessEqual(b.audio_bytes,48000*4*2)

    def test_long_gap_without_activity_transition_recovers_sample_clock(self):
        b=CaptureBuffer(0)
        b.audio_event('data',bytes(480*4),.01)
        b.audio_event('data',bytes(480*4),2.01)
        b.audio_event('data',bytes(480*4),2.02)
        _,chunks,_=b.take(200000)
        self.assertEqual([start for start,_ in chunks],[0,96000,96480])
        self.assertEqual(b.discontinuity_count,1)
        self.assertEqual(b.discontinuities[0]['estimated_gap_samples'],95520)

    def test_take_splits_audio_without_losing_sample_position(self):
        b=CaptureBuffer(0);b.audio_event('data',b'abcd'*100,.01)
        _,chunks,_=b.take(430)
        self.assertEqual(chunks,[(380,b'abcd'*50)])
        _,chunks,_=b.take(1000)
        self.assertEqual(chunks,[(430,b'abcd'*50)])


class PrivateHandoffTests(unittest.TestCase):
    def test_qmp_transfers_real_file_descriptor_with_json_request(self):
        left,right=socket.socketpair()
        try:
            q=object.__new__(QMP);q.sock=left;q.reader=left.makefile('rb');q.sequence=0
            right.sendall(b'{"return":{},"id":1}\n')
            with tempfile.TemporaryFile() as file:
                file.write(b'passed-fd');file.seek(0)
                self.assertEqual(q.call('getfd',{'fdname':'test'},fd=file.fileno()),{})
                payload,ancillary,_,_=right.recvmsg(4096,socket.CMSG_SPACE(array.array('i').itemsize))
                self.assertEqual(json.loads(payload)['execute'],'getfd')
                received=array.array('i');received.frombytes(ancillary[0][2])
                try:self.assertEqual(os.read(received[0],9),b'passed-fd')
                finally:os.close(received[0])
            q.reader.close()
        finally:left.close();right.close()

    def test_zombie_worker_is_not_considered_alive(self):
        import subprocess,time
        worker=subprocess.Popen(['true'])
        try:
            identity=process_identity(worker.pid)
            for _ in range(100):
                if not process_is_alive(worker.pid,identity):break
                time.sleep(.001)
            self.assertFalse(process_is_alive(worker.pid,identity))
        finally:worker.wait()

    def test_native_gl_display_does_not_add_incompatible_vnc(self):
        args=recording_display_args(['qemu','-display','sdl,gl=on','-audiodev','none,id=hda0'])
        self.assertNotIn('-vnc',args)
        self.assertIn('sdl,gl=on',args)

    def test_native_software_display_keeps_private_audio_recording(self):
        args=recording_display_args(['qemu','-display','sdl','-audiodev','none,id=hda0'])
        self.assertEqual(args[-2:],['-vnc','none,audiodev=hda0'])

    def test_egl_headless_keeps_private_audio_recording(self):
        args=recording_display_args(['qemu','-display','egl-headless,rendernode=/dev/dri/renderD128','-audiodev','none,id=hda0'])
        self.assertEqual(args[-2:],['-vnc','none,audiodev=hda0'])

    def test_explicit_native_gl_vnc_conflict_is_reported(self):
        with self.assertRaisesRegex(ValueError,'OpenGL.*VNC'):
            recording_display_args(['qemu','-display','gtk,gl=on','-vnc','none'])

    def test_native_gl_recording_is_unavailable_before_worker_dispatch(self):
        import server
        from pathlib import Path
        with tempfile.TemporaryDirectory() as directory:
            descriptor={'private_rfb_available':False,'recording_unavailable_reason':'Native OpenGL display is incompatible with QEMU VNC'}
            with patch.object(server,'REGISTRY',Path(directory)),patch.object(server,'load_session',return_value=descriptor):
                status=server.video_recording('a'*32,action='status')
                self.assertFalse(status['available'])
                with self.assertRaisesRegex(ValueError,'Native OpenGL'):
                    server.video_recording('a'*32,action='start')
            self.assertEqual(list(Path(directory).iterdir()),[])

    def test_preserves_existing_vnc_security_and_audio_selection(self):
        args=recording_display_args(['qemu','-vnc','127.0.0.1:1,password=on,audiodev=other','-audiodev','none,id=hda0'])
        self.assertEqual(args[2],'127.0.0.1:1,password=on,audiodev=other')
        self.assertEqual(args.count('-vnc'),1)


class VideoArtifactTests(unittest.TestCase):
    def test_large_video_returns_metadata_without_base64_payload(self):
        import server
        from pathlib import Path
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);session='a'*32;artifacts=root/session/'artifacts';artifacts.mkdir(parents=True)
            name='b'*32+'.mkv';path=artifacts/name
            with path.open('wb') as file:file.truncate(17*1024*1024)
            with patch.object(server,'REGISTRY',root),patch.object(server,'load_session'):
                result=server.get_artifact(session,name)
            self.assertEqual(result['size'],17*1024*1024)
            self.assertEqual(result['mime_type'],'video/x-matroska')
            self.assertNotIn('base64',result)
            self.assertEqual(result['artifact'],str(path))


class ClaimOwnershipTests(unittest.TestCase):
    def test_delayed_worker_cannot_overwrite_a_newer_claim(self):
        import record
        from pathlib import Path
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);session='a'*32;(root/session).mkdir()
            owner={'recording':'b'*32,'generation':'g','pid':os.getpid(),
                   'process_identity':process_identity(os.getpid()),'state':'starting'}
            path=root/session/'video-recording.json';path.write_text(json.dumps(owner))
            with patch.object(record,'REGISTRY',root), \
                 patch.object(record,'load_session',return_value={'generation':'g'}), \
                 patch.object(record,'recording_lock',lambda _:contextlib.nullcontext()), \
                 patch.object(record.signal,'signal'), \
                 patch.object(record,'private_display') as display:
                result=record.run(session,'c'*32,1,30,320,180,False)
            self.assertEqual(result['state'],'failed')
            self.assertEqual(json.loads(path.read_text()),owner)
            display.assert_not_called()


@unittest.skipUnless(importlib.util.find_spec('av'),'Run with the pinned recording runtime for codec tests')
class MediaTimelineTests(unittest.TestCase):
    def fake_run(self, audio, changed_generation=False):
        import record
        from pathlib import Path
        class Socket:
            def __init__(self):self.stop=threading.Event()
            def settimeout(self,_):pass
            def shutdown(self,_):self.stop.set()
            def close(self):self.stop.set()
        sock=Socket()
        class Display:
            def __init__(self,*args,**kwargs):
                self.audio_supported=False;self.bytes_received=0;self.updates=0
                self.geometry_changes=[(320,180)];self.geometry_change_count=1
            def handshake(self):pass
            def next_update(self):
                if not self.updates:
                    self.updates+=1;return 320,180,bytes(320*180*4)
                sock.stop.wait();raise EOFError('closed')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);session='a'*32;token='b'*32;(root/session).mkdir()
            owner={'recording':token,'generation':'g','pid':os.getpid(),
                   'process_identity':process_identity(os.getpid()),'state':'starting'}
            path=root/session/'video-recording.json';path.write_text(json.dumps(owner))
            with patch.object(record,'REGISTRY',root), \
                 patch.object(record,'load_session',return_value={'generation':'g'}), \
                 patch.object(record,'recording_lock',lambda _:contextlib.nullcontext()), \
                 patch.object(record,'session_lock',lambda _:contextlib.nullcontext({'generation':'changed' if changed_generation else 'g'})), \
                 patch.object(record.signal,'signal'), \
                 patch.object(record,'private_display',return_value=sock), \
                 patch.object(record,'RfbDisplay',Display):
                result=record.run(session,token,1,30,320,180,audio)
            promoted=(root/session/'artifacts'/f'{token}.mkv').exists()
            return result,promoted

    def test_short_capture_with_unsupported_audio_is_failed(self):
        result,promoted=self.fake_run(True)
        self.assertEqual(result['state'],'failed')
        self.assertIn('audio capability',result['error'])
        self.assertFalse(promoted)

    def test_generation_change_before_promotion_prevents_success(self):
        result,promoted=self.fake_run(False,True)
        self.assertEqual(result['state'],'failed')
        self.assertIn('generation changed before finalization',result['error'])
        self.assertFalse(promoted)

    def test_codec_retains_acquisition_timestamps_and_idle_tail(self):
        import av
        from pathlib import Path
        from record import MediaWriter
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'timeline.mkv'
            writer=MediaWriter(path,320,180,30,True)
            expected=[0,.1,.7,1.5,2.0]
            for t in expected:writer.write_video(bytes(320*180*4),320,180,t)
            writer.write_audio([],96000);writer.close()
            with av.open(str(path)) as container:
                self.assertAlmostEqual(container.duration/1000000,2.0,delta=.05)
                self.assertEqual([round(frame.time,3) for frame in container.decode(video=0)],expected)
            with av.open(str(path)) as container:
                audio=list(container.decode(audio=0))
                self.assertGreaterEqual(sum(frame.samples for frame in audio),96000)

    def test_final_packet_duration_does_not_add_a_low_fps_frame(self):
        import av
        from pathlib import Path
        from record import MediaWriter
        for fps in [1,2,30]:
            with self.subTest(fps=fps),tempfile.TemporaryDirectory() as directory:
                path=Path(directory)/'tail.mkv'
                writer=MediaWriter(path,320,180,fps,False)
                writer.write_video(bytes(320*180*4),320,180,0)
                writer.write_video(bytes(320*180*4),320,180,2,terminal=True);writer.close()
                with av.open(str(path)) as container:
                    self.assertAlmostEqual(container.duration/1000000,2.0,delta=.005)

    def test_silence_preserves_gap_between_audio_bursts(self):
        import av
        from pathlib import Path
        from record import MediaWriter
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'audio.mkv'
            writer=MediaWriter(path,320,180,30,True)
            writer.write_video(bytes(320*180*4),320,180,0)
            pulse=struct.pack('<hh',8000,-8000)*4800
            writer.write_audio([(9600,pulse),(57600,pulse)],76800)
            writer.write_video(bytes(320*180*4),320,180,1.6);writer.close()
            active=[]
            with av.open(str(path)) as container:
                for frame in container.decode(audio=0):
                    values=array.array('f');values.frombytes(bytes(frame.planes[0])[:frame.samples*4])
                    if sum(v*v for v in values)/len(values)>.001:active.append(frame.time)
            self.assertTrue(any(.18<t<.33 for t in active),active)
            self.assertTrue(any(1.18<t<1.33 for t in active),active)
            self.assertFalse(any(.5<t<1.0 for t in active),active)


if __name__=='__main__':unittest.main()
