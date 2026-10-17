"""MCP media contract: no bridge-owned binary payloads in JSON text."""
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import wave
from unittest.mock import patch

import common
import media
import server


class MediaDeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.session = 'a' * 32
        self.run = self.root / self.session
        self.directory = self.run / 'artifacts'
        self.directory.mkdir(parents=True)
        (self.run / 'session.json').write_text(json.dumps({
            'session': self.session, 'owner_uid': os.getuid(), 'state': 'stopped'}))
        for module in (common, server):
            patcher = patch.object(module, 'REGISTRY', self.root)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.png = server.png_rgb(2, 3, bytes(range(18)))

    def save(self, blob, suffix='.png'):
        path = self.directory / ('b' * 32 + suffix)
        path.write_bytes(blob)
        return path

    def call(self, name, **args):
        result = server.call_tool(name, {'session': self.session, **args})
        text = next(block['text'] for block in result['content'] if block['type'] == 'text')
        return result, json.loads(text)

    def test_default_image_is_reference_after_vm_exit(self):
        path = self.save(self.png)
        envelope, result = self.call('vm_artifacts', artifact_name=path.name)
        self.assertEqual([item['type'] for item in envelope['content']], ['text'])
        self.assertNotIn(base64.b64encode(self.png).decode(), json.dumps(envelope))
        item = result['artifacts'][0]
        self.assertEqual(item['path'], str(path))
        self.assertEqual(item['artifact_id'], path.name)
        self.assertEqual((item['width'], item['height']), (2, 3))
        self.assertEqual(item['sha256'], hashlib.sha256(self.png).hexdigest())
        self.assertEqual(item['byte_length'], len(self.png))
        self.assertIsNone(item['created_host_monotonic'])

    def test_explicit_preview_uses_native_image_block_only(self):
        path = self.save(self.png)
        envelope, result = self.call('vm_artifacts', artifact_name=path.name, include_image=True)
        self.assertEqual([item['type'] for item in envelope['content']], ['image', 'text'])
        self.assertEqual(base64.b64decode(envelope['content'][0]['data']), self.png)
        self.assertNotIn(base64.b64encode(self.png).decode(), envelope['content'][1]['text'])
        self.assertEqual(result['image_artifact']['path'], str(path))

    def test_binary_export_has_no_encoded_payload_or_old_size_limit(self):
        path = self.save(b'', '.memory')
        with path.open('wb') as target:
            target.truncate(17 * 1024 * 1024)
        envelope, result = self.call('vm_artifacts', artifact_name=path.name)
        self.assertEqual(result['byte_length'], 17 * 1024 * 1024)
        self.assertLess(len(json.dumps(envelope)), 3000)
        self.assertNotIn('base64', result)
        self.assertNotIn('encoding', result)

    def test_recursive_adapter_preserves_coordinates_and_guest_text(self):
        path = self.save(self.png)
        metadata = {'artifact': str(path), 'frame_id': 'frame', 'generation': 'boot',
                    'frame_epoch': 42, 'crop_origin': [10, 20], 'scale': .5,
                    'coordinate_width': 1920, 'coordinate_height': 1080}
        legitimate = 'data:image/png;base64,' + base64.b64encode(self.png).decode()
        adapter = media.Delivery(self.root, self.session)
        result = adapter.adapt({'observations': [{'image': self.png, 'data': metadata}],
                                'stdout': legitimate, 'nested': {'bytes': b'raw-pcm'}})
        frame = result['observations'][0]
        for key, value in metadata.items():
            self.assertEqual(frame[key], value)
        self.assertEqual(result['stdout'], legitimate)
        self.assertEqual(Path(result['nested']['bytes']['path']).read_bytes(), b'raw-pcm')
        self.assertEqual(len(adapter.descriptors), 2)
        self.assertEqual(adapter.images, [])
        self.assertEqual(list(self.directory.glob('*.png')), [path])
        json.dumps(result)  # All nested bytes removed before serialization.

    def test_waveform_delivers_both_source_pcm_and_plot_references(self):
        path = self.directory / ('b' * 32 + '.wav')
        with wave.open(str(path), 'wb') as audio:
            audio.setnchannels(2)
            audio.setsampwidth(2)
            audio.setframerate(44100)
            audio.writeframes(b'\0\0\0\0' * 441)
        envelope, result = self.call('vm_audio_analyze', artifact_name=path.name)
        self.assertEqual([block['type'] for block in envelope['content']], ['text'])
        self.assertEqual(result['duration_s'], .01)
        self.assertEqual(result['rms'], 0)
        self.assertEqual({item['mime_type'] for item in result['artifacts']}, {'audio/wav', 'image/png'})
        self.assertEqual(result['image_artifact']['width'], 800)
        self.assertEqual(result['image_artifact']['height'], 240)

    def test_preview_budget_and_type_are_enforced(self):
        path = self.save(self.png)
        with patch.object(media, 'MAX_PREVIEW_BYTES', len(self.png) - 1):
            with self.assertRaisesRegex(ValueError, 'preview exceeds'):
                self.call('vm_artifacts', artifact_name=path.name, include_image=True)
        for bad in ('true', 1, None, []):
            with self.subTest(value=bad), patch.object(server, 'get_artifact') as get:
                with self.assertRaisesRegex(ValueError, 'Invalid type'):
                    self.call('vm_artifacts', artifact_name=path.name, include_image=bad)
                get.assert_not_called()

    def test_escaping_paths_and_nonregular_files_are_rejected(self):
        outside = self.root / 'outside.png'
        outside.write_bytes(self.png)
        path = self.directory / ('c' * 32 + '.png')
        path.symlink_to(outside)
        for name in (path.name, '../outside.png', str(outside)):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.call('vm_artifacts', artifact_name=name)
        path.unlink()
        os.mkfifo(path)
        with self.assertRaisesRegex(ValueError, 'regular file'):
            self.call('vm_artifacts', artifact_name=path.name)
        path.unlink()
        self.directory.rmdir()
        self.directory.symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'directory escapes'):
            self.call('vm_artifacts', artifact_name='b' * 32 + '.png')

    def test_missing_session_and_mismatched_image_do_not_publish(self):
        path = self.save(self.png)
        with self.assertRaisesRegex(ValueError, 'do not match'):
            media.Delivery(self.root, self.session).adapt({
                'image': b'wrong', 'data': {'artifact': str(path)}})
        (self.run / 'session.json').unlink()
        with self.assertRaises(FileNotFoundError):
            self.call('vm_artifacts', artifact_name=path.name)

    def test_live_capture_and_partial_artifacts_are_not_delivered(self):
        path = self.save(b'incomplete', '.wav')
        capture = self.run / 'recording.json'
        capture.write_text(json.dumps({'artifact': str(path)}))
        with self.assertRaisesRegex(ValueError, 'not finalized'):
            self.call('vm_artifacts', artifact_name=path.name)
        capture.unlink()
        video = self.run / 'video-recording.json'
        video.write_text(json.dumps({'state': 'recording', 'timing_artifact': str(path)}))
        with self.assertRaisesRegex(ValueError, 'not finalized'):
            self.call('vm_artifacts', artifact_name=path.name)
        video.write_text(json.dumps({'state': 'complete', 'timing_artifact': str(path)}))
        self.call('vm_artifacts', artifact_name=path.name)
        for suffix in ('.partial.mkv', '.png.tmp'):
            partial = self.save(b'incomplete', suffix)
            with self.assertRaisesRegex(ValueError, 'not finalized'):
                self.call('vm_artifacts', artifact_name=partial.name)

    def test_stdio_default_is_safe_without_response_unbundling(self):
        path = self.save(self.png)
        request = {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                   'params': {'name': 'vm_artifacts', 'arguments': {
                       'session': self.session, 'artifact_name': path.name}}}
        result = subprocess.run([sys.executable, str(Path(server.__file__))],
            input=json.dumps(request)+'\n', text=True, capture_output=True, timeout=10,
            env={**os.environ, 'BROOK_VM_REGISTRY': str(self.root)}, check=True)
        reply = json.loads(result.stdout)['result']
        self.assertFalse(reply['isError'])
        self.assertEqual([block['type'] for block in reply['content']], ['text'])
        self.assertEqual(json.loads(reply['content'][0]['text'])['path'], str(path))
        self.assertNotIn(base64.b64encode(self.png).decode(), result.stdout)


if __name__ == '__main__':
    unittest.main()
