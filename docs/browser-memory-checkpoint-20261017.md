# Browser memory checkpoint — 17 October 2026

The main ELF loader now allocates the union of page-aligned `PT_LOAD`
segments rather than the entire virtual span between the first and last
segment. Holes have neither physical pages nor Image mapping ranges. Shared
segment boundary pages are allocated once; file copying and BSS initialization
retain their existing ordering. The image's recorded virtual span and program
break are unchanged. Segment permission hardening is separate work.

The installed Chromium ELF has a 500,654,080-byte virtual span but only
334,540,800 bytes of page-aligned load segments: 166,113,280 bytes of gaps per
loaded image. Codium has only 8,192 bytes of gaps, so this change mainly benefits
Chromium and its independently executed helpers.

## Validation

- All 22 hosted suites passed. The new suite compiles the actual ELF loader and
  mapping-range implementation with modeled physical frames/page tables. It
  covers sparse static/PIE images, reversed headers, shared boundary pages,
  BSS, malformed input, pre-existing mappings, allocation failure and mapping
  failure cleanup. Its sparse-image assertion fails against the old loader.
- The candidate kernel was built twice to refresh generated symbols. Its SHA256
  is `ed82eeae669656639e120121e10f15b22ebc892abbb4617fbab225136bec1e55`.
- Baseline and candidate VM disks came from the same clean seed with the same
  current Chromium/Codium wrappers and profiles: 8 CPUs, 4 GiB RAM, software
  rendering and audio disabled. Chromium rendered Google's consent page on
  both. Settled physical used memory was approximately 2,519,896,064 bytes
  before and 1,885,331,456 bytes after (about 605 MiB less). This is a sampled
  run comparison, not a startup-time benchmark or a peak-memory guarantee.
- Concurrent Codium startup still failed on both kernels. The baseline logged
  ELF allocation exhaustion. The candidate logged a stack allocation failure
  and a loader write fault on a COW mapping; Codium displayed a crashed-window
  dialog. Transient memory pressure is a working hypothesis, not a proven
  explanation for every fault. Post-failure free memory cannot establish the
  amount available at the instant of failure.

Baseline session: `7125e62d44d3464390d0ac3bd2594150` (checked shutdown and all
three offline filesystem checks passed). Candidate session:
`2513284b98a54ce5a91f8ec130cd6ec9` (investigation in progress at this checkpoint).
Local evidence is under `build/workloads/browser-memory-20261017/`, including
`hosted.log`, `elf-layouts.json`, baseline statistics and frozen boot artifacts;
VM serial logs and captures are under their bridge session directories.

## Next investigation

Measure allocation failures at the failure point and reduce duplicate exec
staging/image memory. Astra reviewed reusing eligible full staging pages with
managed COW references. That design is not implemented in this checkpoint:
`VmmMapPage` strips COW, so any implementation must use a suitable publication
API, preserve staging-image immutability, and test reference cleanup, aliases,
partial pages, overlaps and fork/write isolation. Browser/editor concurrent
edit/save/exit acceptance remains open.
