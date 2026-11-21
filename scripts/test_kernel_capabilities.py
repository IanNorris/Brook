#!/usr/bin/env python3
"""Check artifact binding and app requirement handling for a kernel manifest."""

import json
from pathlib import Path
import tempfile
import unittest

from kernel_capabilities import (
    ELF_HEADER,
    SECTION_HEADER,
    read_kernel_capabilities,
    require_kernel_capabilities,
    verify_kernel_capabilities,
)


def fake_kernel(declaration, *, section_flags=0):
    names = b"\0.shstrtab\0.brook_capabilities\0"
    sections = [
        bytes(SECTION_HEADER.size),
        SECTION_HEADER.pack(1, 3, 0, 0, ELF_HEADER.size, len(names), 0, 0, 1, 0),
    ]
    data = names
    if declaration is not None:
        sections.append(SECTION_HEADER.pack(
            names.index(b".brook_capabilities"), 1, section_flags, 0,
            ELF_HEADER.size + len(names), len(declaration), 0, 0, 1, 0,
        ))
        data += declaration
    header = ELF_HEADER.pack(
        b"\x7fELF\x02\x01\x01", 2, 62, 1, 0, 0, ELF_HEADER.size + len(data),
        0, ELF_HEADER.size, 0, 0, SECTION_HEADER.size, len(sections), 1,
    )
    return header + data + b"".join(sections)


class KernelCapabilitiesTest(unittest.TestCase):
    def test_version_requirement_checks(self):
        with tempfile.TemporaryDirectory() as folder:
            kernel = Path(folder) / "BROOK.elf"
            kernel.write_bytes(fake_kernel(b'{"chromium-sandbox":1}\0'))
            manifest = read_kernel_capabilities(kernel)
            require_kernel_capabilities(manifest, {})
            require_kernel_capabilities(manifest, {"chromium-sandbox": 1})
            with self.assertRaisesRegex(ValueError, "available version 1"):
                require_kernel_capabilities(manifest, {"chromium-sandbox": 2})
            with self.assertRaisesRegex(ValueError, "broadwell-video-engine version 1 .missing"):
                require_kernel_capabilities(manifest, {"broadwell-video-engine": 1})

    def test_manifest_adheres_to_the_exact_kernel_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            kernel = Path(folder) / "BROOK.elf"
            sidecar = Path(folder) / "kernel-capabilities.json"
            elf = fake_kernel(b'{"chromium-sandbox":1,"broadwell-video-engine":1}\0')
            kernel.write_bytes(elf)
            manifest = read_kernel_capabilities(kernel)
            sidecar.write_text(json.dumps(manifest))
            self.assertEqual(verify_kernel_capabilities(kernel, sidecar), manifest)
            require_kernel_capabilities(manifest, {"broadwell-video-engine": 1})

            # The declaration is still identical, but the kernel is different.
            kernel.write_bytes(elf + b"\0")
            with self.assertRaisesRegex(ValueError, "does not match"):
                verify_kernel_capabilities(kernel, sidecar)

            kernel.write_bytes(elf)
            manifest["capabilities"]["broadwell-video-engine"] = 2
            sidecar.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "does not match"):
                verify_kernel_capabilities(kernel, sidecar)

    def test_reject_missing_and_invalid_declarations(self):
        examples = [
            (fake_kernel(None), "does not declare"),
            (fake_kernel(b"", section_flags=2), "invalid kernel capabilities section"),
            (fake_kernel(b'{"a":1}'), "unterminated"),
            (fake_kernel(b'{"a":true}\0'), "invalid kernel capability"),
            (fake_kernel(b'{"a":1,"a":2}\0'), "duplicate kernel capability field"),
            (fake_kernel(b'{"a":1}\0')[:-1], "invalid kernel ELF section table"),
        ]
        with tempfile.TemporaryDirectory() as folder:
            kernel = Path(folder) / "BROOK.elf"
            for elf, reason in examples:
                with self.subTest(reason=reason):
                    kernel.write_bytes(elf)
                    with self.assertRaisesRegex(ValueError, reason):
                        read_kernel_capabilities(kernel)


if __name__ == "__main__":
    unittest.main()
