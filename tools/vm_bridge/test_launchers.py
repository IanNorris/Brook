"""Run the actual wrapper with a fake game to check argv and saved preferences."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Launchers(unittest.TestCase):
    def run_wrapper(self, saved=False):
        with tempfile.TemporaryDirectory() as name:
            directory=Path(name);home=directory/'home'
            config=home/'.yq2/baseq2/config.cfg'
            original='bind d "+back"\n'
            if saved:
                config.parent.mkdir(parents=True);config.write_text(original)
            game=directory/'game'
            game.write_text('#!/usr/bin/env python3\nimport sys,json\nprint(json.dumps(sys.argv[1:]))\n')
            game.chmod(0o700)
            text=(ROOT/'data/nix-wrappers/yquake2-play.sh.in').read_text()
            text=text.replace('/data/yq2',str(home)).replace('@YQUAKE2_BIN@',str(game))
            wrapper=directory/'wrapper';wrapper.write_text(text)
            result=subprocess.run(['bash',str(wrapper),'gl3','+map','base1'],check=True,capture_output=True,text=True)
            args=json.loads(result.stdout)
            self.assertEqual(args[args.index('vid_renderer')+1],'gl3')
            self.assertEqual(args[-2:],['+map','base1'])
            if saved:
                self.assertEqual(config.read_text(),original)
                self.assertNotIn('brook-controls.cfg',args)
                self.assertFalse((config.parent/'brook-controls.cfg').exists())
            else:
                self.assertIn('brook-controls.cfg',args)
                self.assertIn('bind d "+moveright"',(ROOT/'data/config/yquake2-controls.cfg').read_text())

    def test_fresh_controls_keep_gl3_and_map(self):self.run_wrapper()
    def test_saved_preferences_remain_authoritative(self):self.run_wrapper(saved=True)


if __name__=='__main__':unittest.main()
