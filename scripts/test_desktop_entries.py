#!/usr/bin/env python3
"""Check launcher discovery against an isolated miniature installed store."""
import contextlib
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import collect_desktop_entries as collect

class DesktopEntriesTest(unittest.TestCase):
    def test_installed_apps_svg_absolute_icons_and_alpha(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); store=root/'store'; package=store/'example'; output=root/'applications'
            (package/'bin').mkdir(parents=True)
            (package/'bin/example').write_text('#!/bin/sh\n');(package/'bin/example').chmod(0o755)
            apps=package/'share/applications';apps.mkdir(parents=True)
            icons=package/'share/icons/hicolor/scalable/apps';icons.mkdir(parents=True)
            svg=icons/'example.svg'
            svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="48" height="24"><rect x="8" y="4" width="32" height="16" fill="red"/></svg>')
            (apps/'org.example.App.desktop').write_text('[Desktop Entry]\nName=Example\nExec=example %F\nIcon=example\nType=Application\n')
            (apps/'missing.desktop').write_text('[Desktop Entry]\nName=Missing\nExec=missing\n')
            (apps/'hidden.desktop').write_text('[Desktop Entry]\nName=Hidden\nExec=example\nNoDisplay=true\n')
            (apps/'cached.desktop').write_text('[Desktop Entry]\nName=Cached but removed\nExec=example\n')
            active=root/'profile/share/applications';active.mkdir(parents=True)
            (active/'org.example.App.desktop').symlink_to(apps/'org.example.App.desktop')
            (active/'missing.desktop').symlink_to(apps/'missing.desktop')
            (active/'hidden.desktop').symlink_to(apps/'hidden.desktop')
            self.assertEqual(collect.find_icon(str(store),'/nix/store/example/share/icons/hicolor/scalable/apps/example.svg'),str(svg))
            with patch('sys.argv',['collect',str(store),str(output)]),contextlib.redirect_stdout(io.StringIO()):collect.main()
            manifest=(output/'applications.idx').read_text().splitlines()
            self.assertEqual(manifest,['Example\texample\texample.rgba\t\torg.example.App'])
            pixels=(output/'icons/example.rgba').read_bytes()
            self.assertEqual(len(pixels),24*24*4)
            self.assertEqual(struct.unpack_from('<I',pixels,0)[0],0)
            self.assertEqual(struct.unpack_from('<I',pixels,(12*24+12)*4)[0],0xffff0000)
            for builtin in ('applications','application','terminal'):
                self.assertEqual((output/'icons'/f'{builtin}.rgba').stat().st_size,2304)

if __name__=='__main__':unittest.main()
