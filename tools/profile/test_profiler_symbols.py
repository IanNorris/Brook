"""Regression coverage for bridge symbol exports outside nix-shell."""
import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "profiler_converter",
    Path(__file__).resolve().parents[2] / "scripts/profiler_to_speedscope.py")
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)


class SymbolExportTests(unittest.TestCase):
    def test_missing_demangler_preserves_elf_symbol_resolution(self):
        extracted = subprocess.CompletedProcess(
            ["nm"], 0, stdout="ffffffff80000000 T KernelMain\n", stderr="")
        with patch("subprocess.run", side_effect=[
                extracted, FileNotFoundError("llvm-cxxfilt")]):
            path = converter._symmap_from_elf("fixture.elf")
        self.assertIsNotNone(path)
        try:
            symbols = converter.load_symmap(path)
            self.assertEqual(converter.resolve_rip(
                0xffffffff80000010, symbols, sorted(symbols)),
                "KernelMain+0x10")
        finally:
            Path(path).unlink()


if __name__ == "__main__":
    unittest.main()
