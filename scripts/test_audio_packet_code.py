#!/usr/bin/env python3
"""Generated-signal regressions; run with NumPy and FFmpeg in the environment."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import wave

import numpy as np
from workloads.analyze_audio_packet_code import decode


class PacketCodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='brook-packet-code-')
        cls.root=Path(cls.temp.name);cls.rate=44100
        # Independent waveform fixture for the documented v1 frequency protocol.
        cls.packets=[(1200*np.sin(2*np.pi*(400+40*((i*137+29)%463))*np.arange(4410)/44100)).astype('<i2') for i in range(100)]
        cls.ffmpeg=shutil.which(os.environ.get('BROOK_TEST_FFMPEG','ffmpeg'))
        if not cls.ffmpeg:raise RuntimeError('Set BROOK_TEST_FFMPEG or provide ffmpeg in PATH')
        cls.normal=cls.write('normal',np.concatenate(cls.packets))
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    @classmethod
    def write(cls,name,values):
        path=cls.root/(name+'.wav')
        with wave.open(str(path),'wb') as f:
            f.setnchannels(1);f.setsampwidth(2);f.setframerate(cls.rate);f.writeframes(values.tobytes())
        return path
    def transform(self,name,filter):
        path=self.root/(name+'.wav')
        subprocess.run([self.ffmpeg,'-v','error','-threads','1','-filter_threads','1','-i',str(self.normal),'-af',filter,str(path)],check=True)
        with wave.open(str(path)) as f:values=np.frombuffer(f.readframes(f.getnframes()),dtype='<i2')
        return decode(values,self.rate,100)
    def test_normal_signal_has_all_symbols_at_unit_rate(self):
        x=decode(np.concatenate(self.packets),self.rate,100)
        self.assertEqual(len(x['symbol_runs']),100);self.assertFalse(x['backwards_or_repeat_transitions']);self.assertEqual(x['unobserved_ids_between_runs'],0)
        self.assertAlmostEqual(x['approximate_source_seconds_per_pcm_second'],1,delta=.005)
    def test_known_pitch_preserving_tempo_controls(self):
        for tempo in (1.3,.5):
            x=self.transform('tempo-'+str(tempo),'atempo='+str(tempo))
            self.assertAlmostEqual(x['approximate_source_seconds_per_pcm_second'],tempo,delta=.02)
            self.assertAlmostEqual(x['pitch_scale'],1,delta=.001)
    def test_skip_and_replay_are_distinct(self):
        skip=decode(np.concatenate(self.packets[:30]+self.packets[35:]),self.rate,100)
        self.assertEqual(skip['unobserved_ids_between_runs'],5);self.assertFalse(skip['backwards_or_repeat_transitions'])
        replay=decode(np.concatenate(self.packets[:35]+self.packets[30:35]+self.packets[35:]),self.rate,100)
        self.assertEqual(len(replay['backwards_or_repeat_transitions']),1)
        self.assertEqual(replay['backwards_or_repeat_transitions'][0]['previous'],34)
        self.assertEqual(replay['backwards_or_repeat_transitions'][0]['next'],30)
    def test_small_pitch_shift_does_not_force_unit_rate(self):
        x=self.transform('pitch','asetrate=44320.5,aresample=44100')
        self.assertAlmostEqual(x['pitch_scale'],1.005,delta=.001)
        self.assertAlmostEqual(x['approximate_source_seconds_per_pcm_second'],1.005,delta=.01)
    def test_plain_tone_cannot_be_accepted_as_a_sequence(self):
        values=(1200*np.sin(2*np.pi*440*np.arange(self.rate*2)/self.rate)).astype('<i2')
        self.assertNotEqual(decode(values,self.rate,100)['status'],'ok')


if __name__=='__main__':unittest.main()
