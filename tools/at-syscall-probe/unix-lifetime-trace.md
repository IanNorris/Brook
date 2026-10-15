# Unix socket lifetime forensic capture

This diagnostic does not fix the recurring Firefox socket UAF. Arm it **after**
other acceptance probes, immediately before launching Firefox: `unixlife on`.
This clears history and watches; `unixlife off` preserves history but stops
recording. Neither command enables heap poisoning or syscall/UART tracing.
Existing sockets created before arming have ref events but no allocation watch.

The C-linkage symbol `g_unixLifeTrace` is 265248 bytes. Resolve its address from
the exact candidate ELF (`nm -S ... | rg g_unixLifeTrace`). Pause the owned VM
before extracting that exact raw virtual-memory range through QMP; do not reset
or reboot it. Decode with `python3 tools/decode-unix-life.py dump.memory`, or add
`--handle 0xffff...` for the corrupt socket. Caller addresses use the same ELF's
symbols. A panic without all other CPUs paused is not an atomic whole-ring dump.

Layout is little-endian, declared in `unix_lifetime_trace.h`: 32-byte header,
128 24-byte watches, then 4096 64-byte records. Sequence is written last with
release semantics, zero means uncommitted. Each record has handle, caller PC,
heap range address/size where applicable, exact atomic ref previous/next values,
PID/TGID, FD (or -1), event and watch generation. Non-ref events use previous/next
as zero; heap events use them as watch-active before/after. Retain records may
arrive out of atomic transition order across CPUs; previous/next remain exact.
Publication runs under the descriptor lock before a closer can retire the slot.
CopyRetain caller PCs distinguish fork and dup ownership acquisition.

HeapAttempt includes even invalid-header/double-free attempts at a watched
pointer. HeapFree records actual accepted kfree/coalesced ranges. HeapWriteFree
records all free WriteBlock ranges, including allocator split/expansion.
HeapAllocate records allocations overlapping an existing watch (including freed
tombstones). This distinguishes explicit socket teardown, foreign stale frees,
and erroneous coalescing/reallocation. Watch removal occurs only on an actual
free range; freed tombstones remain until reused by a new watch. Saturation is
counted, never silently presented as complete coverage. Ring overwritten counts
also identify lost history; no absence-of-event conclusion is valid after loss.

Normal overhead: one disabled-flag check per hook, no scans, allocation or UART.
When armed, a hook scans at most 128 watches and writes bounded records; an
IRQ-safe recorder lock protects commits. Lock order is heap/FD/Unix-registry
then recorder; recorder code never takes any other lock or invokes callbacks.
Reset clears the fixed 259-KiB buffer under that lock and is an explicit command,
not a per-event cost. Host tests exercise the actual core's coalesced overlap,
tombstone/reuse generation, saturation and 4096-record wrap/commit behavior.
Their timing is host core-only, not a guest latency claim.
