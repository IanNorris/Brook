# SteamRT device-directory urandom access

Steam's native64 client now starts inside the accepted pressure-vessel container
without its `s_dev_urandom_fd >= 0` fatal assertion. It advances to the existing
missing named-FIFO operation (`m_NamedPipe >= 0`), so this is not a login-screen
or authenticated-client acceptance. The next implementation gate is documented
in `steam-fifo-next-step.md`.

The retained canonical `/dev` bridge now explicitly permits only missing `null`
and `urandom` leaves. Both require the pinned original backing and parent object
identity, ordinary path search, the caller's final-symlink policy, and a NODEV
check in the same namespace transaction. Real leaves and unrelated overmounts
remain ordinary VFS objects. `/dev/shm`, graphics, audio and `/dev/random` do not
gain eligibility through this exception. Urandom uses the existing random-byte
implementation; its entropy algorithm is unchanged. Random-device FD publication
now preserves access/status flags and CLOEXEC, like the null-device path.

All 101 host tests, retained-path ASAN/UBSAN, final ELF audits and mutation checks
pass. The portable urandom lifecycle probe passes 77 fixture plus 15 parent
checks on Linux, and 79 plus 15 on Brook. It covers ordinary and relative opens,
CLOEXEC/access mode, I/O, final symlink flags, namespace copies, an unrelated
`/dev` shadow, NODEV and surviving descriptors, and fresh-root pivot/detach.
Linux uses recursive `/dev` binds for its locked inherited submounts. Brook's
extra checks verify that the nonrecursive device bind still masks global shm.
The existing null, file/socket bind, namespace/container, native application and
Mousepad desktop regressions also pass on this kernel.

Evidence: `build/workloads/urandom-device-20261023/completion-audit.json`.
Session: `b9f75847dbba43839e09ec58d6045c4a`.
ABI-46 kernel: `8b0dbbadcdd07379d617931309431ca36a0c7a540540d7162c9f4b55e9296335`.
The inherited session metadata's historical source label is corrected explicitly
in `capsule-provenance.json`; actual frozen kernel/module hashes were verified
at staging. Original failed and successful operations remain distinct evidence.
The VM shuts down cleanly, its original owner exits zero, heap integrity is OK,
and all four offline filesystem checks pass.

## Runtime output correction

Default copy-runtime execution still exits zero. With unique append-mode output
files it prints all three expected results: the child marker, `glibc 2.42`, and
Steam Runtime 3c build `3c.0.20260824.257593`. This is the previously staged payload
with Nix ELF interpreter patches, not a pristine-interpreter compatibility claim.

The earlier missing getconf output was an output-file overwrite, not proof that
the runtime command failed: forked processes currently have independent file
positions where Linux shares an open-file-description offset. Append-mode log
files preserve their writes. `scripts/workloads/steamrt_check.py` now uses that
mode in a newly created evidence directory. This is a diagnostic workaround;
shared offsets across fork/dup still need a real kernel fix and differential
coverage. No conclusion is drawn about the earlier proc/self/maps diagnostic.

The initial Steam retry overflows the bounded bridge stderr ring with runtime
copy warnings. Its original terminal identity and available diagnostic tail are
retained, and it is not counted as complete-output evidence. The subsequent
retry writes full guest files and returns only hashes and bounded tails; its
recorded named-FIFO failure is the current client result. Multiple non-append
writers can overlap in those first client logs; future client captures should
use the same append-mode policy as the runtime probe.
