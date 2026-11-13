#!/usr/bin/env python3
"""Check RAM transfer digest admission and executable permission changes."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="brook-ram-exec-") as directory:
    path = Path(directory)
    source = path / "harness.c"
    source.write_text(r'''
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
static int test_open(const char *path, int flags) {
    (void)path;
    return open(getenv("TEST_ARTIFACT"), flags);
}
#define open test_open
#define geteuid() 0
#include "hardware_ram_exec_stage.c"
''')
    binary = path / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(root / "tools"),
                    str(source), "-o", str(binary)], check=True)
    artifact = path / "artifact"
    artifact.write_bytes(b"bounded maintenance fixture")
    artifact.chmod(0o600)
    env = os.environ | {"TEST_ARTIFACT": str(artifact)}
    request = "1" * 32
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
    # Host test owns the fixture as the current user; emulate the guest's root owner.
    # The source's root-owner check is separately exercised in the real guest.
    if os.geteuid() != 0:
        text = source.read_text().replace("#define geteuid() 0", "#define geteuid() getuid()")
        # Owner checks require root, so override only the fixture's stat UID.
        text = text.replace("#define open test_open", """
static int test_fstat(int fd, struct stat *st) {
    int result = fstat(fd, st); if (!result) st->st_uid = 0; return result;
}
#define fstat test_fstat
#define open test_open""")
        text = text.replace("#include <fcntl.h>", "#include <fcntl.h>\n#include <sys/stat.h>")
        text = text.replace("#define geteuid() getuid()", "#define geteuid() 0")
        source.write_text(text)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(root / "tools"),
                        str(source), "-o", str(binary)], check=True)
    assert subprocess.run([binary, request, "0" * 64], env=env).returncode != 0
    assert artifact.stat().st_mode & 0o777 == 0o600
    assert subprocess.run([binary, request, digest], env=env).returncode == 0
    assert artifact.stat().st_mode & 0o777 == 0o700
    assert subprocess.run([binary, "../bad", digest], env=env).returncode != 0
print("RAM executable stage: PASS")
