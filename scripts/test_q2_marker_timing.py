"""Exercise host marker receipts and the real benchmark with a synthetic demo."""
import http.server
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).parent / 'workloads'))
from q2_demo_timing import notify_marker


class MarkerTimingTests(unittest.TestCase):
    def setUp(self):
        self.receipts = []
        self.reply = None
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                name = self.path.rsplit('/', 1)[-1]
                stamp = time.monotonic_ns()
                owner.receipts.append((name, stamp))
                value = owner.reply if owner.reply is not None else dict(marker=name, host_ns=stamp)
                body = json.dumps(value).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.worker = threading.Thread(target=self.server.serve_forever)
        self.worker.start()
        self.url = 'http://127.0.0.1:%d/test' % self.server.server_port

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.worker.join()

    def test_receipt_uses_host_timestamp(self):
        value = notify_marker(self.url, 'START')
        self.assertEqual(value['host_ns'], self.receipts[0][1])
        self.assertLessEqual(value['before_guest_ns'], value['host_ns'])
        self.assertLessEqual(value['host_ns'], value['after_guest_ns'])

    def test_rejects_invalid_or_wrong_marker_receipts(self):
        for value in ({}, {'marker': 'END', 'host_ns': 123},
                      {'marker': 'START', 'host_ns': True},
                      {'marker': 'START', 'host_ns': 0},
                      {'marker': 'START', 'host_ns': '123'}):
            with self.subTest(value=value):
                self.reply = value
                with self.assertRaises(ValueError):
                    notify_marker(self.url, 'START')

    def test_complete_benchmark_with_synthetic_pak_and_engine(self):
        # No Quake data or game binary is required. Exercise main(), its actual
        # process/log loop, JSON output and host acknowledgements together.
        with tempfile.TemporaryDirectory(prefix='brook-q2-marker-') as folder:
            root = Path(folder)
            pak_dir = root / 'baseq2'
            pak_dir.mkdir()
            demo = b''.join(struct.pack('<iBi', 5, 0x14, n) for n in range(200))
            demo += struct.pack('<i', -1)
            directory = struct.pack('<56sii', b'demos/demo1.dm2', 12, len(demo))
            (pak_dir / 'pak0.pak').write_bytes(
                struct.pack('<4sii', b'PACK', 12 + len(demo), len(directory)) + demo + directory)
            engine = root / 'engine'
            engine.write_text('#!' + sys.executable + '\n'
                              'import os,time\nfrom pathlib import Path\n'
                              "p=Path(os.environ['HOME'])/'.yq2/baseq2/qconsole.log'\n"
                              "with p.open('w') as f:\n"
                              " f.write('BROOK_DEMO_TIMING_START\\n');f.flush()\n"
                              ' time.sleep(5.2)\n'
                              " f.write('BROOK_DEMO_TIMING_END\\n');f.flush()\n")
            engine.chmod(0o755)
            home = root / 'home'
            command = [sys.executable, str(Path(__file__).parent / 'workloads/q2_demo_timing.py'),
                       '--engine', str(engine), '--home', str(home),
                       '--pak-dir', str(pak_dir), '--marker-url', self.url]
            result = subprocess.run(command, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((home / 'timing-result.json').read_text())
            self.assertEqual([name for name, _ in self.receipts], ['START', 'END'])
            expected = (self.receipts[1][1] - self.receipts[0][1]) / 1e9
            self.assertEqual(report['host_marker_interval_s'], expected)
            self.assertEqual(report['measured_demo_records'], 150)
            self.assertGreater(report['observed_demo_interval_s'], 5)

    def test_unpaced_render_preserves_normal_demo_time(self):
        with tempfile.TemporaryDirectory(prefix='brook-q2-unpaced-') as folder:
            root=Path(folder);pak=root/'baseq2';pak.mkdir()
            demo=b''.join(struct.pack('<iBi',5,0x14,n) for n in range(80))
            demo+=struct.pack('<i',-1)
            directory=struct.pack('<56sii',b'demos/demo1.dm2',12,len(demo))
            (pak/'pak0.pak').write_bytes(struct.pack('<4sii',b'PACK',12+len(demo),64)+demo+directory)
            engine=root/'engine'
            engine.write_text('#!'+sys.executable+'\n'
                'import os,sys,time,json\nfrom pathlib import Path\n'
                "h=Path(os.environ['HOME']);(h/'argv.json').write_text(json.dumps(sys.argv))\n"
                "with (h/'.yq2/baseq2/qconsole.log').open('w') as f:\n"
                " f.write('BROOK_DEMO_TIMING_START\\n');f.flush();time.sleep(.25)\n"
                " f.write('BROOK_DEMO_TIMING_END\\n');f.flush()\n")
            engine.chmod(0o755);home=root/'home'
            result=subprocess.run([sys.executable,str(Path(__file__).parent/'workloads/q2_demo_timing.py'),
                '--engine',str(engine),'--pak-dir',str(pak),'--home',str(home),
                '--unpaced-render','--measured-frames','1'],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            argv=json.loads((home/'argv.json').read_text())
            self.assertEqual(argv[argv.index('r_vsync')+1],'0')
            self.assertEqual(argv[argv.index('timedemo')+1],'0')
            self.assertEqual(argv[argv.index('cl_maxfps')+1],'250')
            query=(home/'.yq2/baseq2/brook-benchmark-query.cfg').read_text()
            self.assertIn('set vid_maxfps 999\n',query)
            report=json.loads((home/'timing-result.json').read_text())
            self.assertFalse(report['uncapped'])
            self.assertTrue(report['unpaced_render'])
            self.assertEqual(report['recorded_frame_delta'],1)
            self.assertEqual(report['expected_server_interval_s'],.2)
            self.assertGreater(report['engine_pid'],0)


if __name__ == '__main__':
    unittest.main()
