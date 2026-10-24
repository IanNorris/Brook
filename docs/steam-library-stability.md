# Steam Library and resource-limit queries — 24 October 2026

The user successfully authenticated in Steam. The next failure was a recurring
CEF renderer breakpoint while attempting to use Library. This checkpoint fixes
an independently reproduced resource-limit reporting defect and records the
first successful Library rendering; wider stability and rootless Xwayland
integration remain acceptance work.

## Measured failure and repair

Eight baseline renderer exits report exception vector 3 at the same address.
A temporary private loader audit, followed by a bounded QEMU hardware
breakpoint capture, identifies libcef ELF offset `0x6807dce`: its out-of-memory
termination path. Saved candidate return addresses include `0x6807de9`,
`0x6807e06`, `0x9d8816c` and `0x3c61cf4`. The caller supplies zero as an unknown
allocation size; this does not mean a zero-byte allocation failed. No matching
failed mmap was established by the bounded memory trace.

Raw Linux syscall 97 reports unlimited CPU, file-size and data limits, while
syscall 302 and libc report an 8 MiB soft limit for all three. The stack limit
was accidentally shared with these unrelated resources in `prlimit64`.
Both query entry points now use one default-value helper. Stack remains
8 MiB/unlimited and the descriptor limit remains 1,024/1,024.

This is a plausible consumer-visible mechanism: Chromium's POSIX virtual-memory
query reads `RLIMIT_DATA`, and V8 can derive a code-range request from that
reported limit divided by eight. The public sources are nearby versions rather
than a complete source-identification proof of the shipped client:
[Chromium query](https://chromium.googlesource.com/chromium/src.git/+/refs/tags/131.0.6778.22/base/system/sys_info_posix.cc),
[V8 sizing](https://raw.githubusercontent.com/v8/v8/12.6.228/src/api/api.cc).

The change repairs reporting only. Resource-limit setters, target-process
authorization and enforcement remain incomplete; it does not implement full
Linux resource-limit semantics.

## Validation and limits

`scripts/workloads/resource_limit_query_check.py` compares raw 97, raw 302 and
libc without changing any limits. Linux passes its 12 portable comparisons.
The baseline Brook control fails nine checks; the fixed guest passes all 18,
including explicit preservation of stack and descriptor defaults.

The release kernel builds. Compiled entry-state checks pass for 12 supported
templates, with their mutation checks; scheduler-start and lock-wait audits
also pass. A separately retained strict entry audit reports four unsupported
templates and no failed checked contracts. It is not a universal handler proof.

The original signed-in review was synced, stopped with original launcher exit
zero, and passed all four offline filesystem checks. Candidate testing uses
private copies of those authenticated disks and the unchanged helper wrapper,
without preload or loader audit. The valid retry reaches Library, game titles
and artwork. Longer navigation/download acceptance remains in progress.

Two failed candidate attempts remain recorded: an inherited QEMU option was
duplicated before boot, and an existing private PulseAudio runtime made its
starter fail. The latter client attempt subsequently faults in the main
process; that is not counted as a successful stability test or established to
be caused by the missing server. The valid retry uses a fresh private runtime
whose cookie-authenticated `pactl info` verifies its null sink before Steam starts.

Private evidence is under `build/workloads/steam-rejection-20261024` and reviews
`steam-nonblock-login-k-20261024` and `steam-rlimit-stability-m-20261024`.
Authenticated images, profiles, raw logs, debugger stack words, downloaded games
and proprietary binaries remain outside Git. The rootful 800×600 Xwayland
desktop is still present and does not satisfy individual-window integration.
