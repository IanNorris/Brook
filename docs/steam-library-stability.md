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
and proprietary binaries remain outside Git. These earlier reviews use the
rootful 800×600 Xwayland desktop and do not prove individual-window integration.

The subsequent live download test collected a lossy CPU/switch profile and a
separate ten-second counter interval. Of 3,214 CPU samples, 2,592 are idle;
43,764 profiler events were dropped. Frequent Steam switch-out sites include
mapping-mutex waits during user-buffer validation, including clock queries.
The counter interval shows about 2.9 KB/s received, 1.27 MB/s data-disk reads
and 1.35 MB/s writes, averaging roughly 735 and 1,532 bytes per request. These
are leads for local lock/I/O contention, not proof of blocked-time fractions or
the Steam install phase; the profiler itself also writes disk data.

After those captures the host QEMU process exits with SIGSEGV. Its saved core
notes identify the main thread in `libSDL3.so.0.4.2` at module file offset
`0x274cfc`. The host core is truncated; the bounded serial tail contains no
guest-panic marker. This identifies a host display-library fault location, not
its cause. The review disks remain preserved without assuming a clean sync.
Consequently this run does not yet establish prolonged Steam stability.

The subsequent `steam-rootless-xwayland-n-20261024` review uses the same frozen
kernel with the owned rootless compositor/worker and a headless host display.
Authenticated Store and Library render in an independent native window;
maximized Library has a 1916×996 client buffer. Friends opens as a separate
300×650 native window and moves independently. A native Wayland editor runs
on that same compositor. See [the window-integration record](xwayland-window-integration.md)
for controls and remaining acceptance. Account captures remain private.

A later profiling request in this rootless review captured 755 CPU samples
with no reported event drops, including 676 idle samples. A separate ten-second
interval receives about 2.7 KB/s and writes about 164 KB/s to the data disk.
At observation time Steam shows Library without an active download and its
content log is unchanged for more than ten minutes. This is not a representative
download benchmark. Another three-second profile loses 2,461 events and includes
the profiler's own disk writer; neither capture establishes the download
bottleneck.

The subsequent user-controlled run completes a small game's download and
verification, then starts a separate Steam runtime dependency before launching.
A ten-second transferring interval receives about 1.11 MB/s and records about
1.02 MB/s logical data-disk reads and 3.43 MB/s logical writes. The driver counts
logical read/write calls including cache hits, not physical device requests;
the roughly 158-byte average read-call size is not an SSD IOPS measurement.

Three 100 ms reservation captures and one 100 ms installation capture have no
reported event drops. They repeatedly show `AccessAt` switch-out stacks waiting
in `UserMappingPageAccess`. The access implementation validates and reads each
pathname byte separately, acquiring the mapping mutex for each byte. This is a
concrete optimization lead, not a measured blocked-time fraction or proof of the
entire download bottleneck. The longer transfer capture drops many events and
cannot support precise timing claims. The game and its runtime finish installing; two launch attempts exit 125 in
`steam-runtime-launch-client`, before any game-code crash is established. A
separate no-op launch also returns 125 because its companion launcher service
is missing. The diagnostic launcher is being changed to Valve's
`srt-client-container`, which supplies that service and its handoff environment.
The data filesystem reports about 3.9 GiB available to the ordinary user with
ample free inodes.

`AccessAt` now uses the existing fault-recovering, page-bounded C-string copier
instead of validating every pathname byte separately. The 256-byte path limit
and error results are retained. All 29 access checks pass on Linux and in the
fresh native review, including readable-page crossings, protected-page faults
and a terminating NUL immediately before an inaccessible page. The release
build and supported entry/scheduler-start/lock-wait audits pass. This verifies
semantics; a download throughput improvement has not yet been measured.

The N review shuts down after an acknowledged final sync, and offline checks
of all four filesystems return zero. Its original VM launcher wait returns zero;
the outer review gate separately fails on an earlier recovered virtio-blk queue
timeout and periodic writeback error. That incident remains a stability lead,
even though final sync and filesystem checks succeed.
