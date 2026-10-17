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

## Next investigation at the sparse-loader checkpoint (`24f8a18`)

Measure allocation failures at the failure point and reduce duplicate exec
staging/image memory. Astra reviewed reusing eligible full staging pages with
managed COW references. That design is not implemented in this checkpoint:
`VmmMapPage` strips COW, so any implementation must use a suitable publication
API, preserve staging-image immutability, and test reference cleanup, aliases,
partial pages, overlaps and fork/write isolation. Browser/editor concurrent
edit/save/exit acceptance remains open.

## Follow-up: staging reuse and anonymous reservations

The dedicated `ReadExecutableImage` buffer now supplies eligible full initialized
pages directly to the main executable's user mappings. Each installed leaf owns
an independent reference and uses USER+COW through `VmmTryMapPage`. Partial
pages, BSS, noncongruent source offsets and every overlapping destination page
retain private zero-and-copy handling. The loader never writes a reused frame
through its physical alias. Releasing the staging mapping drops only its own
reference. Raw-buffer process APIs and their ABI retain copying; explicit owned
image entry points serve shell launch and execve. Interpreter loading still
copies. Physical descriptors retain Heap/KernelPid, so per-PID allocation
statistics do not fully attribute reused pages to applications.

Hosted loader coverage now includes read-only staging pages (an accidental
copy would fault on Linux), alias COW, both cleanup orders, single-owner write
promotion after staging release, invalid ownership-context shape and injected
mapping/allocation failures. All 22 hosted suites pass. Astra reviewed the
ownership and API changes. The native `sparse_exec_cow.S` probe checks actual
guest fork/write isolation for initialized pages and BSS, plus mprotect rejection
of the virtual gap. It passed directly and eight times through Bash exec on
the staging-reuse kernel, and again on the later kernel.

Staging reuse alone let concurrent Codium reach its editor, but the 4 GiB run
still exhausted memory. Bounded PMM failure diagnostics captured `free_pages=0`
at the fault, rather than inferring exhaustion from samples collected afterward.
Codium alone passed native editing, save/readback, close-tab/reopen and normal
window exit. The VM returned to ten tasks; heap integrity and all three offline
filesystem checks passed. Session `f924a5c2a1b144f1bbd3b7582df58ec6`, kernel
`93fc1dc43541632f361bb573c8cec06805f6e9cb343a2f2883c65bfcec0d985e`.

Private anonymous mmap previously allocated every accessible page immediately.
Ordinary mappings now reserve metadata and use the existing demand-zero fault
handler; explicit MAP_POPULATE retains eager allocation. Legacy file preload
also skips FileObject-backed vnodes: their faults use the inode's resident cache,
so filling the legacy cache only duplicates pages and pins vnodes unnecessarily.
No measured application-memory gain is attributed specifically to that guard.

`scripts/workloads/anonymous_memory.py` passed all eight semantic checks on
Linux. In otherwise idle 8-CPU, 4 GiB guest controls:

| Measurement | Before demand-zero | After demand-zero |
| --- | ---: | ---: |
| Physical cost of a 256 MiB reservation | 271,163,392 B | 98,304 B |
| Cost after touching four pages | 271,245,312 B | 204,800 B |
| 16 MiB MAP_POPULATE cost | 16,515,072 B | 15,912,960 B |
| Probe checks passing | 6/8 | 8/8 |

Samples use guest MemFree, include incidental runtime activity, and are bounded
by the otherwise idle test setup. The two old-kernel failures are the expected
physical-allocation assertions; kernel copyout, fork isolation, protection,
discard, explicit population and shared-anonymous semantics passed on both.
Control session `01e1a4170aa54169b279d93ed875d85c`; candidate session
`9b2106b3f85f4de9ba7e935a43cfdffa`. Both stopped with all three filesystem
checks passing. Candidate kernel:
`e71477f5c072e3914d3de3f37087f21d0c2364bd296288969dd0eb17aeb6a1f5`.

The later 4 GiB combined run saved and reopened the document, but still logged
allocation exhaustion and a Codium fault during window close. It is **not**
accepted as stable. The sampled headroom briefly settled near 61 MiB and later
fell below 30 MiB; neither sample establishes a safe bound.

## Concurrent acceptance with 8 GiB

With the same candidate kernel, 8 CPUs, software Wayland and audio disabled,
Chromium and Codium ran together as Brook. Native input dismissed Google's
consent prompt, submitted a search and navigated back. Codium edited, saved and
reopened `/data/home/codium menu acceptance.txt`; independent readback matched
all 151 bytes, owner 1000:100, SHA256
`326e5769361988651dad8bce05fd983f194ccce37bfcb9a2ff62d71816bcbe48`.
Both windows closed through their native controls without forced termination.
The guest returned to ten tasks, heap integrity passed, and serial contained no
PMM allocation failures, user faults or negative process exits. Checked shutdown
and all three offline filesystem checks passed. Session:
`7582fa88668049bf8a1c4a4e383cdbb8`.

`docs/images/brook_chromium_codium_1080.png` is an unmodified native 1920×1080
capture from that run. It shows a bounded browser/editor acceptance case, not
arbitrary extensions, long sessions or a solution to the 4 GiB limit.

Reboot session `9ea8c238505e4c8a9f81b7682fd4d710` independently confirmed the
same file hash, 151-byte length and 1000:100 ownership. Codium reopened the saved
contents through the standard wrapper, then exited through its native close
control. Ten tasks remained, heap integrity passed, no allocation failures or
user faults were logged, and checked shutdown plus all three offline filesystem
checks passed again. Local results and frozen artifacts are under
`build/workloads/browser-memory-20261017/`.

Next memory work should attribute resident pages by mapping kind and distinguish
live allocations from retained caches. Independent exec images still duplicate
initialized pages across executions. Demand file-backed exec would need bounded
header reads, TLS preparation, file-gate publication and executable write
exclusion or immutable-version backing; the current file-map pin blocks truncate
but does not block ordinary writes. A bounded immutable content-dedup cache is
another option, subject to admission, lifetime and pressure-recovery testing.
