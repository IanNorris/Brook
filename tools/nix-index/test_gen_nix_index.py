"""Index join rules: only x86_64-linux and binary-cache-servable packages are offered."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('gen_nix_index', HERE / 'gen-nix-index.py')
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


class JoinTests(unittest.TestCase):
    def setUp(self):
        self.paths = {
            'mesen': {'path': '/nix/store/' + 'a' * 32 + '-mesen-2.1.1', 'system': 'x86_64-linux'},
            'zsnes': {'path': '/nix/store/' + 'b' * 32 + '-zsnes-1.51', 'system': 'i686-linux'},
            'snes9x-gtk': {'path': '', 'system': 'x86_64-linux', 'unfree': True},
            'legacy': '/nix/store/' + 'c' * 32 + '-legacy-1',
        }
        self.meta = {
            'mesen': {'pname': 'mesen', 'version': '2.1.1', 'system': 'x86_64-linux',
                      'meta': {'description': 'Multi-system\temulator', 'mainProgram': 'Mesen'}},
            'zsnes': {'pname': 'zsnes', 'version': '1.51', 'system': 'i686-linux',
                      'meta': {'description': 'SNES emulator', 'mainProgram': 'zsnes'}},
            'snes9x-gtk': {'pname': 'snes9x-gtk', 'version': '1.63', 'meta': {}},
            'legacy': {'pname': 'legacy', 'version': '1', 'system': 'x86_64-linux',
                       'meta': {'mainProgram': '../escape'}},
        }

    def test_filtering_and_reasons(self):
        entries, excluded = gen.build_entries(self.paths, self.meta, 'x86_64-linux')
        offered = {e[0]: e for e in entries}
        self.assertEqual(sorted(offered), ['legacy', 'mesen'])
        self.assertEqual(offered['mesen'][3:], ('Multi-system emulator', 'Mesen'))
        self.assertEqual(offered['legacy'][4], '')  # unsafe mainProgram dropped
        reasons = {e[0]: e[3] for e in excluded}
        self.assertIn('i686-linux', reasons['zsnes'])
        self.assertIn('unfree', reasons['snes9x-gtk'])

    def test_written_index_has_optional_fifth_column(self):
        entries, excluded = gen.build_entries(self.paths, self.meta, 'x86_64-linux')
        with tempfile.TemporaryDirectory() as directory:
            pin = Path(directory) / 'pin.nix'
            pin.write_text('{}')
            output = Path(directory) / 'packages.idx'
            gen.write_index(output, entries, excluded, pin, 'x86_64-linux')
            rows = [line.split('\t') for line in output.read_text().splitlines()]
            self.assertEqual([len(r) for r in rows], [4, 5])
            self.assertEqual(rows[1][4], 'Mesen')
            self.assertIn('zsnes\t1.51\tzsnes\t', Path(str(output) + '.excluded.tsv').read_text())
            import json
            meta = json.loads(Path(str(output) + '.json').read_text())
            self.assertEqual((meta['entries'], meta['excluded_other_system'], meta['excluded_unfree']), (2, 1, 1))


if __name__ == '__main__':
    unittest.main()
