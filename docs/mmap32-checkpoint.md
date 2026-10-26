# Bounded MAP_32BIT allocation (2026-10-26)

After the PI mutex fix, native Graveyard Keeper creates its OpenGL device but
aborts in Mono assembly initialization. The assertion is
`mini-amd64.c:209: amd64_is_imm32(disp)`, guarding a relative call displacement.
The corresponding public [Unity 2020.3 code manager](https://github.com/Unity-Technologies/mono/blob/cd43bde125114470b0199699e8d348615186c1c1/mono/utils/mono-codeman.c)
requests executable chunks with MONO_MMAP_32BIT, which mono-mmap.c translates
to MAP_32BIT. This source relationship is supplemented by actual requests from
the installed game, rather than assumed from its version alone.

A first process-scoped memory trace attaches too late to capture JIT allocation
and contains no MAP_32BIT call. A separate diagnostic-only preload delegates the
real mmap/mmap64 functions without changing arguments or returned mappings.
It records two mmap(NULL,65536,PROT_RWX,0x62) calls returning `0x18c053000` and
`0x18c5a7000`. Both successful mappings violate the requested below-2-GiB band.
The observer is not enabled for subsequent normal Steam acceptance runs.

The public [map32check](../tools/at-syscall-probe/map32check.c), built static and
non-PIE, first advances ordinary allocation with a lazy 4-GiB reservation.
The same SHA256 `f0bf233ebc57e5a628b95fe419ee4310ab0016b6051ec5ac4921c4787dd4e764`
passes on Linux; before the fix Brook returns the low allocation at
`0x110000000` and the probe exits 42. One initial staging request was rejected
before dispatch because it exceeded the bridge's 32,768-byte limit. A smaller
encoded request submits the probe once; that negative baseline is retained.

## Fix and controls

UserMappingRequest carries a kernel-internal low32 constraint. Non-fixed mmap
with flag 0x40 searches only `[USER_MMAP_BASE,0x80000000)`, including the full
mapping length. Oversized or exhausted requests return ENOMEM; they cannot
fall back to the high ordinary allocation region. High hints and an already
high mmap cursor cannot bypass the bound. Low mappings leave that ordinary
cursor unchanged. Exact-address mappings, including NOREPLACE, retain their
separate semantics and ignore the low constraint. The existing mapping
transaction still serializes range selection, collision checks and publication.
No external module uses this internal request structure; module ABI stays 56.

The real mapping host fixture covers high cursors/hints, exhaustion, fixed
placement, collision preservation, gap reuse and ordinary-cursor isolation.
All 112 host tests and both compiled entry-path mutation audits pass. Astra's
read-only review finds no blocking issue in the bounded change.
The same probe passes natively on the candidate, including 128 disjoint writable
low allocations, exhaustion/reuse, and fixed mappings above the low band.
The PI ownership/contention/robust/signal probes also pass again.

Candidate kernel SHA256:
`fec9ef09445a1720524fa1a992e51c4f7f9369c00069d4f3a03a63b822ce2a42`.
Evidence is private under `build/workloads/pi-futex-20261026` and
`build/reviews/steam-games-map32-20261026/modal-control`.

## Application result

The unmodified native Steam launch now gets through Mono initialization and
renders the animated title background. The game reports MainMenuGUI and
completes general initialization. A three-second QEMU capture contains audio
(RMS 0.0413, peak 5251/32768). This establishes audio delivery, not a subjective
stutter-free playback verdict. No existing game save is loaded.

Normal maximize changes Unity's client size from 1920x1080 to 1916x996 and the
game surface becomes black. Restoring 1920x1080 does not recover it. Direct X11
readback is also black, placing this failure before Brook composition. Normal
WM_DELETE closes the game and all its tasks drain. This remaining rendering/UI
issue means the title is not yet marked compatible; gameplay and save/load
acceptance remain open. The completed PI review before this candidate has
original-owner exit zero, four clean offline filesystem checks, and unchanged
original Stardew, Goose and Graveyard save contents.
