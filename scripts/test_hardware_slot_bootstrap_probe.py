#!/usr/bin/env python3
"""Host-only coverage for the old-receiver journal prerequisite."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r"""
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "brook_system_esp.h"

static int mode(const char *name) { return strcmp(getenv("MODE"), name) == 0; }
uid_t __wrap_geteuid(void) { return mode("unprivileged") ? 1000 : 0; }
int __wrap_openat(int dir, const char *name, int flags, ...) {
    assert(flags == (O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if (dir == AT_FDCWD && !strcmp(name, "/media/system")) return 7;
    assert(dir == 7);
    if (!strcmp(name,"RECOVERY")) return mode("no-recovery") ? -1 : 8;
    assert(!strcmp(name,"UPDATES")); return mode("no-updates") ? -1 : 9;
}
int __wrap_ioctl(int fd, unsigned long request, ...) {
    if (fd == 7) {
        assert(request == BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT);
        return mode("foreign-root") ? -1 : 0;
    }
    assert((fd == 8 || fd == 9) && request == BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER);
    return mode("foreign-recovery") && fd == 8 ? -1 : 0;
}
int __wrap_open(const char *name, int flags, ...) {
    assert(!strcmp(name,"/tmp/hardware-update.lock"));
    assert(flags == (O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));
    return mode("no-lock") ? -1 : 10;
}
int __wrap_fstat(int fd, struct stat *st) {
    assert(fd == 10);
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | (mode("open-lock") ? 0666 : 0600);
    st->st_uid = mode("foreign-lock") ? 1000 : 0;
    st->st_nlink = 1;
    return mode("unreadable-lock") ? -1 : 0;
}
int __wrap_fstatat(int fd, const char *name, struct stat *st, int flags) {
    assert(fd == 8 && flags == AT_SYMLINK_NOFOLLOW && !strcmp(name,"BOOTSTAT.BIN"));
    memset(st, 0, sizeof(*st));
    if (mode("empty")) { st->st_mode = S_IFREG | 0600; return 0; }
    if (mode("present")) { st->st_mode = S_IFREG | 0600; st->st_size = 1024; return 0; }
    if (mode("symlink")) { st->st_mode = S_IFLNK | 0777; return 0; }
    errno = mode("eio") ? EIO : mode("access-denied") ? EACCES : ENOENT;
    return -1;
}
"""


class BootstrapProbe(unittest.TestCase):
    def test_only_literal_absence_is_accepted(self):
        with tempfile.TemporaryDirectory() as folder:
            fixture = Path(folder) / "fixture.c"
            probe = Path(folder) / "probe"
            fixture.write_text(FIXTURE)
            wrap = ["open", "openat", "ioctl", "fstat", "fstatat", "geteuid"]
            subprocess.run(
                [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", ROOT / "src/shared", ROOT / "tools/hardware_slot_bootstrap_probe.c",
                 fixture, *(f"-Wl,--wrap={name}" for name in wrap), "-o", probe],
                check=True,
            )
            for mode, expected in {
                "absent": 0, "unprivileged": 1, "foreign-root": 1, "no-recovery": 2,
                "no-updates": 2, "foreign-recovery": 2, "no-lock": 3, "open-lock": 3,
                "foreign-lock": 3, "unreadable-lock": 3, "empty": 4, "present": 4,
                "symlink": 4, "eio": 5, "access-denied": 5,
            }.items():
                with self.subTest(mode=mode):
                    result = subprocess.run([probe], env={**os.environ, "MODE": mode},
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, expected)
                    self.assertEqual(result.stdout, "BROOK_SLOT_BOOTSTRAP_JOURNAL_ABSENT\n" if not expected else "")


if __name__ == "__main__":
    unittest.main()
