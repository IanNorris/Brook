"""`nix classify` / `nix launcher-add` against private store and profile trees.

Fixture programs are real host ELF images: a GUI program is one whose
DT_NEEDED names a windowing library (a stub libX11.so.6 built here), and the
Nix wrapper shapes are the ones nixpkgs generates (makeWrapper shell scripts,
makeCWrapper binaries, plain `exec` launchers such as Mesen's).
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
CC = os.environ.get('HOST_CC', 'cc')


def store_name(letter, name):
    return letter * 32 + '-' + name


class ClassifyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='brook-classify-')
        build = Path(cls.build.name)
        cls.root = build / 'fixture'
        cls.binary = build / 'nix'
        paths = dict(STORE_DIR='store', NARINFO_DIR='narinfo', INDEX_PATH='index', PROFILE_DIR='profile',
                     PROFILE_BIN='profile/bin', PROFILE_LIB='profile/lib',
                     PROFILE_APPS='profile/share/applications', MANIFEST_PATH='profile/manifest.tsv',
                     APPS_DIR='apps', APPS_IDX='apps/applications.idx', APPS_ICONS_DIR='apps/icons',
                     BROOK_BIN='wrappers')
        definitions = [f'-D{name}="{cls.root / path}"' for name, path in paths.items()]
        definitions.append(f'-DNIX_FETCH_PATH="{shutil.which("true")}"')
        subprocess.run([CC, '-O2', *definitions, str(ROOT / 'nix-install.c'), '-o', str(cls.binary)], check=True)
        # Stub windowing library and two programs: one needs it, one does not.
        source = build / 'main.c'
        source.write_text('int x11_stub(void);\nint main(void){return x11_stub();}\n')
        (build / 'stub.c').write_text('int x11_stub(void){return 0;}\n')
        subprocess.run([CC, '-shared', '-fPIC', '-Wl,-soname,libX11.so.6', str(build / 'stub.c'),
                        '-o', str(build / 'libX11.so.6')], check=True)
        cls.gui_elf = build / 'gui'
        subprocess.run([CC, str(source), str(build / 'libX11.so.6'), '-o', str(cls.gui_elf)], check=True)
        cls.cli_elf = build / 'cli'
        (build / 'cli.c').write_text('int main(void){return 0;}\n')
        subprocess.run([CC, str(build / 'cli.c'), '-o', str(cls.cli_elf)], check=True)
        cls.build_dir = build

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        if self.root.exists():
            shutil.rmtree(self.root)
        (self.root / 'store').mkdir(parents=True)
        (self.root / 'profile/bin').mkdir(parents=True)
        self.index = []

    def package(self, letter, name, files, index_name=None, main_program=None):
        """files: relative path -> bytes | Path (copied) | str (script)."""
        store = store_name(letter, name)
        base = self.root / 'store' / store
        for relative, content in files.items():
            path = base / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(content, Path):
                shutil.copy2(content, path)
            elif isinstance(content, str):
                path.write_text(content.replace('@STORE@', '/nix/store/' + store))
            else:
                path.write_bytes(content)
            if relative.startswith('bin/') or relative.endswith('-wrapped') or relative.startswith('lib/app/'):
                path.chmod(0o755)
        row = f'{index_name or name}\t1\t{store}\tFixture {name}'
        if main_program:
            row += f'\t{main_program}'
        self.index.append(row)
        (self.root / 'index').write_text('\n'.join(self.index) + '\n')
        return base

    def nix(self, *args, success=True):
        result = subprocess.run([str(self.binary), *args], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def classify(self, name):
        self.nix('install', name)
        out = self.nix('classify', name).stdout
        return dict(line.split('=', 1) for line in out.splitlines())

    def test_elf_needing_windowing_library_is_gui(self):
        self.package('a', 'viewer', {'bin/viewer': self.gui_elf})
        info = self.classify('viewer')
        self.assertEqual(info['kind'], 'gui')
        self.assertEqual(info['launcher'], 'none')
        self.assertEqual(info['command'], '/nix/profile/bin/viewer')
        self.assertIn('libX11.so.6', info['reason'])

    def test_make_wrapper_script_is_followed_to_wrapped_elf(self):
        # bsnes-hd/most GTK apps: wrapProgram's shell script.
        script = ('#! /bin/bash -e\nexport GDK_BACKEND=x11\n'
                  'exec -a "$0" "@STORE@/bin/.emu-wrapped"  "$@" \n')
        self.package('b', 'emu', {'bin/emu': script, 'bin/.emu-wrapped': self.gui_elf})
        self.assertEqual(self.classify('emu')['kind'], 'gui')

    def test_make_c_wrapper_binary_is_followed(self):
        # makeBinaryWrapper: a small ELF that records its target in a comment.
        letter = 'c'
        target = f'/nix/store/{store_name(letter, "cwrap")}/bin/.cwrap-wrapped'
        source = self.build_dir / 'cwrapper.c'
        source.write_text('const char note[] = "# makeCWrapper \'' + target + '\' \\\\ --inherit-argv0";\n'
                          'int main(void){return note[0]==0;}\n')
        wrapper = self.build_dir / 'cwrapper'
        subprocess.run([CC, str(source), '-o', str(wrapper)], check=True)
        self.package(letter, 'cwrap', {'bin/cwrap': wrapper, 'bin/.cwrap-wrapped': self.gui_elf})
        self.assertEqual(self.classify('cwrap')['kind'], 'gui')

    def test_dotnet_avalonia_app_is_probably_gui_and_prefers_main_program(self):
        # Mesen: bin/Mesen execs the .NET apphost beside Avalonia/SkiaSharp.
        script = '#! /bin/bash -e\nexport DOTNET_ROOT=/x\nexec "@STORE@/lib/app/Mesen"  "$@"\n'
        self.package('d', 'mesen', {'bin/Mesen': script, 'bin/mesen-helper': self.cli_elf,
                                    'lib/app/Mesen': self.cli_elf, 'lib/app/Avalonia.X11.dll': b'MZ',
                                    'lib/app/libSkiaSharp.so': b''}, main_program='Mesen')
        info = self.classify('mesen')
        self.assertEqual(info['kind'], 'probably-gui')
        self.assertEqual(info['program'], 'Mesen')
        self.assertIn('Avalonia', info['reason'])

    def test_terminal_tool_is_cli_and_cannot_be_added(self):
        self.package('e', 'ripgrep', {'bin/rg': self.cli_elf}, main_program='rg')
        info = self.classify('ripgrep')
        self.assertEqual(info['kind'], 'cli')
        self.assertEqual(info['program'], 'rg')
        result = self.nix('launcher-add', 'ripgrep', success=False)
        self.assertIn('run rg from Terminal', result.stderr)
        self.assertIn('Run it from Terminal: rg', self.nix('install', 'ripgrep').stdout)

    def test_gui_program_among_tools_is_chosen(self):
        self.package('f', 'suite', {'bin/suite-cli': self.cli_elf, 'bin/suite-gui': self.gui_elf})
        info = self.classify('suite')
        self.assertEqual((info['kind'], info['program']), ('gui', 'suite-gui'))

    def test_32_bit_program_is_unsupported(self):
        header = bytearray(64)
        header[:4] = b'\x7fELF'
        header[4] = 1  # ELFCLASS32
        header[5] = 1
        header[16] = 2  # ET_EXEC
        header[18] = 3  # EM_386
        self.package('g', 'oldgame', {'bin/oldgame': bytes(header) + bytes(4096)})
        self.assertEqual(self.classify('oldgame')['kind'], 'unsupported')

    def test_data_package_has_nothing_to_run(self):
        self.package('h', 'fonts', {'share/fonts/a.ttf': b'font'})
        self.assertEqual(self.classify('fonts')['kind'], 'none')

    def test_desktop_entry_package_reports_its_launcher(self):
        self.package('i', 'editor', {'bin/editor': self.gui_elf,
                                     'share/applications/editor.desktop':
                                         '[Desktop Entry]\nType=Application\nName=Editor\nExec=editor %F\n'})
        info = self.classify('editor')
        self.assertEqual((info['launcher'], info['launcher_name']), ('package', 'Editor'))
        self.assertIn('Open it from the launcher: Editor', self.nix('install', 'editor').stdout)

    def test_launcher_add_is_explicit_and_removed_with_the_package(self):
        script = '#! /bin/bash -e\nexec "@STORE@/lib/app/Mesen"  "$@"\n'
        self.package('j', 'mesen', {'bin/Mesen': script, 'lib/app/Mesen': self.cli_elf,
                                    'lib/app/Avalonia.X11.dll': b'MZ'}, main_program='Mesen')
        runtime = self.root / 'profile/bin/brook-run'
        runtime.write_text('#!/bin/sh\nexec "$@"\n')
        runtime.chmod(0o755)
        self.nix('install', 'mesen')
        apps = self.root / 'apps/applications.idx'
        self.assertNotIn('Mesen', apps.read_text() if apps.exists() else '')
        self.nix('launcher-add', 'mesen')
        entry = self.root / 'profile/share/applications/brook-added-mesen.desktop'
        self.assertIn('X-Brook-Added-Package=mesen', entry.read_text())
        program = self.root / 'profile/bin/Mesen'
        self.assertIn(f'Mesen\t--uid 1000 {runtime} {program}\t', apps.read_text())
        self.assertEqual(self.nix('classify', 'mesen').stdout.count('launcher=added'), 1)
        self.assertFalse((self.root / 'profile/.mutation-lock').exists())
        self.nix('remove', 'mesen')
        self.assertFalse(entry.exists())
        self.assertNotIn('Mesen', apps.read_text())

    def test_uninstalled_package_is_an_error(self):
        self.package('k', 'absent', {'bin/absent': self.cli_elf})
        self.nix('classify', 'absent', success=False)


if __name__ == '__main__':
    unittest.main()
