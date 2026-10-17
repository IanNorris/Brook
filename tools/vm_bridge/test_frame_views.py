"""Saved-frame preview/crop identity, pixel integrity and VM input provenance."""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import common
import server
from frame_views import FrameViews,decode_rgb,render,coordinate_grid
from media import Delivery


class FrameViewTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.s='a'*32;self.boot='b'*32
        self.art=self.root/self.s/'artifacts';self.art.mkdir(parents=True)
        self.counter=0
        def artifact(suffix):
            self.counter+=1;return self.art/(f'{self.counter:032x}'+suffix)
        self.views=FrameViews(self.root,self.s,server.png_rgb,artifact)
        self.w=1280;self.h=720;self.rgb=bytes([20,90,180])*(self.w*self.h)
        self.meta={'generation':'g','guest_boot_id':self.boot,'coordinate_width':self.w,'coordinate_height':self.h,
            'captured_monotonic':42.5,'frame_epoch':123,'frame_tick_ms':456,'panic':False}
    def publish(self,**kw):return self.views.publish(self.rgb,self.meta,**kw)
    def test_previews_share_source_pixels_capture_time_and_geometry(self):
        r=self.publish();f=r['data'];self.assertEqual(decode_rgb(r['image']),(self.w,self.h,self.rgb))
        self.assertEqual(self.counter,3)
        for name,w,h in [('tiny',320,180),('medium',960,540)]:
            p=f['previews'][name];self.assertEqual((p['width'],p['height']),(w,h))
            self.assertEqual(p['source_frame_id'],f['frame_id']);self.assertEqual(p['frame_epoch'],123)
            self.assertEqual(p['captured_monotonic'],42.5);self.assertEqual(p['guest_boot_id'],self.boot)
            self.assertEqual(p['crop_origin'],[0,0]);self.assertEqual(p['coordinate_width'],1280)
            self.assertEqual(decode_rgb(Path(p['artifact']).read_bytes())[2],bytes([20,90,180])*(w*h))
        d=Delivery(self.root,self.s);text=json.dumps(d.adapt(r));self.assertEqual(len(d.descriptors),3)
        self.assertEqual(d.images,[]);self.assertNotIn('data:image',text)
    def test_exact_crop_uses_saved_native_source_and_is_cached(self):
        self.w=80;self.h=40;self.rgb=bytes(v for y in range(40) for x in range(80) for v in (x,y,(x+y)%256))
        self.meta.update(coordinate_width=80,coordinate_height=40)
        source=self.publish()['data'];crop={'x':10,'y':5,'width':40,'height':20}
        first=self.views.view(source['frame_id'],crop);before=self.counter
        second=self.views.view(source['frame_id'],crop)
        self.assertEqual(self.counter,before);self.assertEqual(first['data']['frame_id'],second['data']['frame_id'])
        w,h,pixels=decode_rgb(first['image']);self.assertEqual((w,h),(40,20));self.assertEqual(pixels[:3],bytes([10,5,15]))
        self.assertEqual(pixels[-3:],bytes([49,24,73]));self.assertFalse(first['data']['vm_capture_performed'])
        self.assertEqual(first['data']['frame_epoch'],source['frame_epoch'])
    def test_crop_request_keeps_full_native_frame_for_later_expansion(self):
        first=self.publish(crop={'x':100,'y':80,'width':400,'height':300},max_width=200)
        self.assertEqual((first['data']['width'],first['data']['height']),(200,150))
        full=self.views.view(first['data']['frame_id'])
        self.assertEqual(decode_rgb(full['image']),(1280,720,self.rgb))
        self.assertEqual(full['data']['frame_id'],first['data']['source_frame_id'])
    def test_coordinate_grid_is_cached_separate_and_preserves_clean_native_pixels(self):
        source=self.publish()['data'];crop={'x':123,'y':77,'width':800,'height':600}
        clean=self.views.view(source['frame_id'],crop,size='tiny')
        grid=self.views.view(source['frame_id'],crop,size='tiny',overlay=True)
        count=self.counter;again=self.views.view(grid['data']['frame_id'],crop,size='tiny',overlay=True)
        self.assertEqual(self.counter,count);self.assertEqual(grid['image'],again['image'])
        self.assertNotEqual(clean['image'],grid['image'])
        self.assertNotEqual(clean['data']['frame_id'],grid['data']['frame_id'])
        for key in ['source_frame_id','crop_origin','crop_width','crop_height','scale','scale_y','width','height','frame_epoch']:
            self.assertEqual(clean['data'][key],grid['data'][key])
        overlay=grid['data']['overlay'];self.assertEqual(overlay['origin'],[123,77])
        self.assertEqual(overlay['coordinate_space'],'native_frame')
        self.assertTrue(all(123<=tick<923 for tick in overlay['x_ticks']))
        self.assertTrue(all(77<=tick<677 for tick in overlay['y_ticks']))
        full=self.views.view(grid['data']['frame_id'])
        self.assertNotIn('overlay',full['data']);self.assertEqual(decode_rgb(full['image'])[2],self.rgb)
        with self.assertRaises(ValueError):self.views.view(source['frame_id'],overlay='yes')
    def test_unscaled_crop_fallback_copies_rows_without_resampling(self):
        with patch.dict('sys.modules',{'PIL':None}):
            pixels,w,h,backend=render(bytes(range(48)),4,4,(1,1,2,2),None)
        self.assertEqual((w,h,backend),(2,2,'native_crop'))
        self.assertEqual(pixels,bytes(range(15,21))+bytes(range(27,33)))
    def test_ruler_glyphs_are_drawn_as_readable_two_pixel_blocks(self):
        rgb,_=coordinate_grid(bytes(100*60*3),100,60,(123,77,800,480))
        # The left diagonal of the origin label's x occupies two horizontal
        # and two vertical pixels, not thin one-pixel stripes.
        for y in (5,6):
            for x in (2,3):self.assertEqual(rgb[(y*100+x)*3:(y*100+x)*3+3],b'\xff'*3)
    def test_stdlib_fallback_and_decoder_do_not_need_pillow(self):
        with patch.dict('sys.modules',{'PIL':None}):
            pixels,w,h,backend=render(bytes([50,70,90])*16,4,4,(0,0,4,4),2)
        self.assertEqual((w,h,backend),(2,2,'stdlib_box'));self.assertEqual(pixels,bytes([50,70,90])*4)
        blob=server.png_rgb(w,h,pixels);self.assertEqual(decode_rgb(blob),(w,h,pixels))
        with self.assertRaises(ValueError):decode_rgb(blob[:-5])
        bad=bytearray(blob);bad[-13]^=1
        with self.assertRaises(ValueError):decode_rgb(bytes(bad))
    def test_invalid_crop_size_and_cache_integrity_are_rejected(self):
        f=self.publish()['data']
        for opts in [{'crop':{'x':-1,'y':0,'width':10,'height':10}},
                     {'crop':{'x':1270,'y':0,'width':20,'height':10}},
                     {'size':'bad'},{'size':'tiny','max_width':100},{'max_width':True}]:
            with self.assertRaises(ValueError):self.views.view(f['frame_id'],**opts)
        p=Path(f['artifact']);p.write_bytes(p.read_bytes()+b'bad')
        with self.assertRaisesRegex(ValueError,'checksum mismatch'):self.views.view(f['frame_id'])
    def test_other_session_and_symlink_metadata_do_not_escape(self):
        f=self.publish()['data'];other=self.root/('c'*32);other.mkdir()
        v=FrameViews(self.root,'c'*32,server.png_rgb,lambda suffix:None)
        with self.assertRaises(ValueError):v.view(f['frame_id'])
        p=self.views.index(f['frame_id']);p.unlink();p.symlink_to(self.root/'foreign')
        with self.assertRaises(OSError):self.views.metadata(f['frame_id'])
    def test_stopped_vm_views_need_no_guest_or_qmp_and_keep_old_generation(self):
        f=self.publish()['data']
        with patch.object(server,'load_session',return_value={'generation':'after','state':'stopped'}), \
             patch.object(server,'frame_views',return_value=self.views),patch.object(server,'guest') as guest,patch.object(server,'QMP') as q:
            r=server.frame_view(self.s,f['frame_id'],size='tiny')
        self.assertTrue(r['data']['historical_generation']);self.assertEqual(r['data']['generation'],'g')
        guest.assert_not_called();q.assert_not_called()
    def test_preview_pointer_maps_to_native_and_old_capture_cannot_be_reenabled(self):
        original=self.publish()['data'];tiny=original['previews']['tiny'];events=[]
        class Q:
            def __enter__(self):return self
            def __exit__(self,*a):pass
            def call(self,cmd,args=None):
                if cmd=='query-qmp-schema':return [{'meta-type':'enum','members':[{'name':x} for x in ['ret','ctrl','shift']]}]
                events.extend(args['events']);return {}
        @contextlib.contextmanager
        def lock(*a,**k):yield {'generation':'g'}
        path=self.root/self.s/'last-frame.json';path.write_text(json.dumps(original))
        with patch.object(server,'REGISTRY',self.root),patch.object(server,'session_lock',lock), \
             patch.object(server,'frame_views',return_value=self.views),patch.object(server,'panic_state',return_value=False), \
             patch.object(server,'guest',return_value={'boot_id':self.boot}),patch.object(server,'QMP',lambda s:Q()):
            server.input_actions(self.s,[{'type':'move','x':160,'y':90}],frame_id=tiny['frame_id'])
            self.assertEqual([x['data']['value'] for x in events],[round(640*32767/1279),round(360*32767/719)])
            newer=self.publish()['data'];path.write_text(json.dumps(newer))
            self.views.view(original['frame_id'],size='tiny')
            events.clear()
            with self.assertRaisesRegex(ValueError,'latest capture'):
                server.input_actions(self.s,[{'type':'move','x':1,'y':1}],frame_id=tiny['frame_id'])
            self.assertEqual(events,[])
    def test_coordinate_input_rejects_a_guest_reboot_before_qmp_injection(self):
        f=self.publish()['data'];(self.root/self.s/'last-frame.json').write_text(json.dumps(f))
        @contextlib.contextmanager
        def lock(*a,**k):yield {'generation':'g'}
        with patch.object(server,'REGISTRY',self.root),patch.object(server,'session_lock',lock),patch.object(server,'panic_state',return_value=False), \
             patch.object(server,'guest',return_value={'boot_id':'c'*32}),patch.object(server,'QMP') as q:
            with self.assertRaisesRegex(ValueError,'Stale frame guest boot'):
                server.input_actions(self.s,[{'type':'move','x':1,'y':1}],frame_id=f['frame_id'])
        q.assert_not_called()
