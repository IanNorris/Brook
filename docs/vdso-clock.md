# Userspace clock helper

Brook supplies a versioned x86-64 ELF through `AT_SYSINFO_EHDR` (33), allowing
libc to resolve `__vdso_clock_gettime@LINUX_2.6`. Only `CLOCK_MONOTONIC` has a
fast path. All other clock IDs and unavailable/unstable clock snapshots enter
the existing syscall and retain its semantics, including its unsupported IDs.
Supported and unsupported IDs follow the kernel clock selector; the helper
does not implement additional clock semantics.

The helper uses KVM's stable-clock capability and per-snapshot stable flag,
version validation, ordered TSC sampling and checked shift/add arithmetic.
After 32 unsuccessful snapshots it calls the kernel. That bounds the helper's
retry loop, not the entire request: the existing kernel clock reader may retry
indefinitely. Fast-path bad output pointers can fault directly in userspace,
as with Linux's vDSO; fallback returns raw negative errno to libc.

## Layout and lifetime

One ELF `PT_LOAD` describes an 8 KiB extent: a code page followed by zero-sized
file data for the clock page. One segment matters for glibc's pre-mapped vDSO
base discovery; two segments with the first at address zero caused glibc to
subtract the second segment's address and crash during startup. The kernel
enforces stricter actual permissions than the ELF extent: code is R/X, the
clock page is R/NX, and neither range permits `mprotect` to add write access.

The clock allocation is sanitized across all 4096 bytes **before** registering
it with KVM. Its initial reference is permanently pinned while the host may
write to it. Code and the non-KVM zero-page fallback are also permanently
pinned. Every managed user PTE gets its own reference; normal fork, unmap and
address-space teardown account for those references.

The public page now contains 64 cache-line slots, each holding KVM's 32-byte
per-vCPU record. Slot zero's padding contains pointer-free format metadata
(magic/version/stride/capacity/features); other padding remains zero. Every CPU
registers its own slot before scheduler entry and publishes its CPU index in
`IA32_TSC_AUX` when `RDTSCP` is supported. The helper validates the format before
executing `RDTSCP`, selects that CPU's slot and retries if the CPU tag changes
inside the versioned snapshot. Unsupported CPU features use the syscall path.
Using one shared BSP record allowed stale extrapolation after VM resume; see
[per-vCPU clock validation](pvclock-percpu.md).

The two ranges are installed into an exclusively constructed unpublished
address space after image, interpreter, stack and TLS reservations, with no
replacement of existing mappings. Metadata nodes and page tables are prepared
before publishing either mapping. Allocation failure omits the optional auxv
entry. Exec computes a fresh base and does not retain stale process state.
Ordinary `munmap`, fixed replacement or moving either range with `mremap` can
deliberately break libc's saved helper pointer or its relative clock address.

`scripts/bake_vdso.py` checks the linked ELF and dynamic tags, embeds only the
code page, and removes section-header references. No dependency, relocation,
PLT, writable-data or runtime-library support is needed.

## Verification

- Build: `nix-shell --run 'bash scripts/build.sh Release'`; entry audit and
  hosted tests remain separate checks, not proof of the entire helper ABI.
- ELF input validation: `python3 scripts/test_bake_vdso.py build/release/kernel/brook-vdso.so`.
- Actual ELF host execution: compile `scripts/workloads/vdso_host.c` with
  `cc -O2 -Wall -Wextra -Werror -ldl` and pass the helper's absolute path. Repeat
  against the section-stripped image emitted by `bake_vdso.py --image`.
- Guest: build `scripts/workloads/vdso_guest.c` with static musl and dynamic
  glibc, then run both as Brook. It brackets libc reads with forced syscalls
  across eight threads, checks maximum protections and actual write/NX faults,
  eight fork/unmap/parent-survival
  cycles and exec discovery. This does not force every cross-CPU migration,
  inject allocation failures or prove frame counts return to baseline.
- Pause/resume: `pvclock_resume.c` runs eight readers while the host deliberately
  pauses/resumes the same private VM. It requires all eight CPU tags and checks
  libc monotonic reads against surrounding forced syscalls. Preserve the same
  tracked job; always resume after each injected pause, including host errors.
- Performance: `clock_threads.c THREADS ITERATIONS LEAD libc|syscall` reserves
  512 unrelated VMAs and separates its timed interval from setup/teardown.
  Paired isolated guest measurements are clock-path results, not browser gains.

Real browser startup/playback regression testing, explicit non-KVM guest
fallback, changing-version host stress and allocation-failure/lifetime-counter
injection remain required before declaring broad acceptance.

## 2026-10-18 isolated clock result

The corrected single-load ELF passed static-musl and dynamic-glibc guest
discovery, syscall bracketing, protection changes, fork/unmap and exec checks.
Its console-only 8-vCPU/4-GiB VM completed both repeats of the clock workload,
then acknowledged sync, exited zero and passed all three offline filesystem
checks. Each thread made 10,000 calls; mode order was reversed for repeat two.
The intervals below include worker barrier release and join, and exclude VMA
reservation/teardown. They are ranges across two repeats, not percentiles.

| Threads | Forced syscall (ms) | libc helper (ms) |
| --- | ---: | ---: |
| 1 | 5.41–5.54 | 0.24–0.26 |
| 8 | 193.53–207.28 | 0.68–0.74 |
| 32 | 840.23–843.05 | 2.00–2.14 |

The prior kernel advertised no vDSO; its eight-thread libc runs took
197.01–204.69 ms. These results establish removal of clock syscall overhead
under this workload. They do not establish browser startup or playback gains.
Ignored artifacts are under `build/workloads/clock-libc-baseline-20261018` and
`build/workloads/vdso-clock-single-load-20261018`. The first two-load candidate's
glibc startup crash is preserved separately under `vdso-clock-candidate-20261018`
and is a failed ABI result, despite its successful musl timing run.

The separate desktop acceptance (`build/workloads/vdso-production-20261018`)
passed both libc variants again with three deliberate protection faults each.
Its dynamic-glibc eight-thread probe took 1.31 ms through libc and 192.80 ms
through forced syscalls. Normal Firefox and Chromium launcher windows coexisted
and both lineages drained on native close. Firefox's local media fixture
produced 444,516 stereo PCM frames in a 10.52-second host capture interval with
no silent 10-ms RMS blocks. The fixture reported 242 additional video frames
across 10.02 host seconds, with no reported dropped frames. Post-exit capture
had zero frames and an inactive stream. Heap integrity, acknowledged sync,
launcher exit zero, serial audit and all three offline filesystem checks passed.

Short 250-ms local-playback profiles had no producer drops. The previous
reader-batch kernel recorded 359 non-idle outgoing context switches with
`sys_clock_gettime` on their stacks; this candidate recorded 11. Counts involving
`UserMappingPageAccess` were 740 and 160 respectively. These snapshots support
the intended reduction in kernel clock activity, but do not establish an
end-to-end browser speedup. YouTube audio and complex-page playback remain
separate acceptance work.

References: [Linux vDSO ABI](https://man7.org/linux/man-pages/man7/vdso.7.html),
[glibc pre-mapped ELF discovery](https://raw.githubusercontent.com/bminor/glibc/glibc-2.42/elf/setup-vdso.h),
[KVM pvclock contract](https://docs.kernel.org/virt/kvm/x86/msr.html).
