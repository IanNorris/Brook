"""Run the actual wrapper with a fake game to check argv and saved preferences."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Launchers(unittest.TestCase):
    def run_wrapper(self, saved=False, args=None, autoexec=False):
        if args is None:
            args = ['+map', 'base1']
        with tempfile.TemporaryDirectory() as name:
            directory=Path(name);home=directory/'home'
            config=home/'.yq2/baseq2/config.cfg'
            original='bind d "+back"\n'
            if saved:
                config.parent.mkdir(parents=True);config.write_text(original)
            if autoexec:
                config.parent.mkdir(parents=True, exist_ok=True)
                (config.parent/'autoexec.cfg').write_text(original)
            game=directory/'game'
            game.write_text('#!/usr/bin/env python3\nimport sys,json\nprint(json.dumps(sys.argv[1:]))\n')
            game.chmod(0o700)
            text=(ROOT/'data/nix-wrappers/yquake2-play.sh.in').read_text()
            text=text.replace('/data/yq2',str(home)).replace('@YQUAKE2_BIN@',str(game))
            wrapper=directory/'wrapper';wrapper.write_text(text)
            result=subprocess.run(['bash',str(wrapper),'gl3',*args],check=True,capture_output=True,text=True)
            args=json.loads(result.stdout)
            self.assertEqual(args[args.index('vid_renderer')+1],'gl3')
            if saved or autoexec:
                if saved:
                    self.assertEqual(config.read_text(),original)
                if autoexec:
                    self.assertEqual((config.parent/'autoexec.cfg').read_text(),original)
                self.assertNotIn('brook-controls.cfg',args)
                self.assertFalse((config.parent/'brook-controls.cfg').exists())
            else:
                self.assertIn('brook-controls.cfg',args)
                self.assertIn('bind d "+moveright"',(ROOT/'data/config/yquake2-controls.cfg').read_text())
            return args

    def test_fresh_controls_keep_gl3_and_map(self):
        args = self.run_wrapper()
        self.assertEqual(args[-2:], ['+map', 'base1'])
        self.assertNotIn('+d1', args)

    def test_saved_preferences_remain_authoritative(self):
        args = self.run_wrapper(saved=True)
        self.assertEqual(args[-2:], ['+map', 'base1'])

    def test_fresh_profile_restores_startup_demo(self):
        args = self.run_wrapper(args=[])
        self.assertEqual(args[-3:], ['+exec', 'brook-controls.cfg', '+d1'])

    def test_only_early_settings_still_start_demo(self):
        settings = ['+set', 'vid_mode', '-1', '+set', 'name', '+map']
        args = self.run_wrapper(args=settings)
        self.assertEqual(args[-len(settings)-1:], settings + ['+d1'])

    def test_explicit_late_commands_remain_authoritative(self):
        for command in [
            ['+map', 'base1'], ['+demomap', 'demo1.dm2'],
            ['+connect', '192.0.2.1'], ['+exec', 'custom.cfg'],
            ['+quit'], ['+d1'], ['+timedemo', '1', '+demomap', 'demo1.dm2'],
        ]:
            with self.subTest(command=command):
                caller = ['+set', 'vid_mode', '-1'] + command
                args = self.run_wrapper(args=caller)
                self.assertEqual(args[-len(caller):], caller)
                self.assertEqual(args.count('+d1'), command.count('+d1'))

    def test_saved_profiles_keep_engine_default_startup(self):
        for preferences in ({'saved': True}, {'autoexec': True}):
            with self.subTest(preferences=preferences):
                args = self.run_wrapper(args=[], **preferences)
                self.assertNotIn('+exec', args)
                self.assertNotIn('+d1', args)

    def test_incomplete_set_does_not_consume_injected_demo(self):
        args = self.run_wrapper(args=['+set', 'name'])
        self.assertEqual(args[-2:], ['+set', 'name'])
        self.assertNotIn('+d1', args)


if __name__=='__main__':unittest.main()
