# Isolated tasks for Codex (16 Nov 2026)

These don't overlap the Claude Code agents still finishing work. **Boundaries:**
- don't touch the physical Dell;
- don't edit `window.cpp`, `compositor.cpp`, waylandd, the AHCI or VFS-mapping code, the scheduler, or the Steam launcher scripts (other branches are changing them);
- QEMU and host only;
- one worktree and branch per task, with a host test for each fix;
- record outcomes in HANDOVER.md when done.

1. **`work/compat-batch2`: small Linux ABI fixes.**
   - The xattr syscall family (get/list/set/remove, plus l and f variants) returns EOPNOTSUPP instead of ENOSYS on filesystems without xattrs. Python `shutil.copy2` fails on Brook today.
   - `status()` on `/proc/self/exe` gives EOPNOTSUPP (seen from OpenAL's std::filesystem); match Linux.
   - Add `strace exec NAME`, which arms tracing at execve for matching names, so short-lived processes can be traced. Lift the pid < 256 limit in `syscall_strace.cpp`.
   - Find out why the `oom_recovery` thread uses ~1.8% CPU with no memory pressure.
2. **`work/dmabuf-sync-file`:** implement `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`, and IMPORT if cheap, with Linux semantics on the existing sync-file infrastructure. VKRENDER hit ENOSYS ~10k times per boot, and Chromium also calls it. Don't change the renderer.
3. **`work/coverage`:** llvm-cov build of the host tests, plus a script giving per-file and per-subsystem line and branch coverage. Add `docs/agent/test-coverage.md` with the current numbers and the least-covered kernel interfaces. This is the first step of the agreed interfaces → tests → Rust path (see `docs/design/brook-value.md` on `work/brook-value`). Tooling and docs only.
4. **`work/quake2-launcher`:** a `brook-quake2` wrapper plus desktop entry, built in the repo but not installed. Use the recipe in HANDOVER.md's gen 419 entry: the OSS OpenAL library on LD_LIBRARY_PATH, `alsoft-oss.conf` (oss, 44100, stereo int16, 4×1024 periods), `-datadir /data/home/quake2`, `+set s_openal 1 +set al_driver libopenal.so.1`, and brook-iris-run. Staging on the Dell needs Ian's approval for a new /nix/store path, so stop at a built package.

## Added 18 Nov 2026

5. **`work/ssd-trim`: batch TRIM for the Dell SSD's ext2 partition** (Ian, 18 Nov). Base it on `work/integration32`. This task is the exception to the AHCI boundary above: it owns the AHCI command and ext2 free-space code it needs. Still QEMU and host only. Never touch the physical Dell or any real block device; use disposable image files.
   - **Why:** the Dell's Samsung PM851 (256 GB, SATA) has run for weeks without TRIM, and the install wrote large raw images, so the drive probably thinks most LBAs are in use.
   - **Layout:** GPT, then a 256 MiB EFI (FAT) partition, then ext2 on the rest, mounted at `/media/system`. See `docs/xps9343-ssd-install-plan.md`, `docs/ssd-boot-generations-plan.md` and `docs/ssd-auto-repair-plan.md` (boot slots, guard).
   - **Command:** ATA DATA SET MANAGEMENT (0x06), TRIM bit, with 8-byte range entries (48-bit LBA + 16-bit count) in 512-byte payload blocks.
     - Gate on IDENTIFY word 169 bit 0, and respect the word 105 payload limit.
     - **Non-queued DSM only.** Linux blacklists queued TRIM on Samsung 8xx drives.
     - Don't rely on deterministic read after TRIM.
   - **Scope:**
     - fstrim-style batch trim of ext2 free runs: coalesced, with a configurable minimum such as 1 MiB, mapped through the GPT partition start.
     - No online discard.
   - **Safety:**
     - Trim only blocks free both on disk and in memory. ext2 writes its bitmaps back lazily.
     - Hold off allocation in each group while it is trimmed.
     - Use a separate, pure, host-tested range validator. Every range must lie inside the ext2 partition and avoid the GPT headers and backup GPT, the EFI partition, the ext2 superblock, group descriptors, bitmaps and inode tables, and every guard or boot-slot reservation.
     - Refuse the whole operation if the free counts disagree with the bitmaps, the filesystem isn't clean, or the guard reports storage uncertain.
   - **Modes:**
     - `dry-run` is the default; `run` issues commands. The result is JSON: ranges, bytes, number of DSM commands, duration and errors.
     - Split work into payload-limited commands, interleaved with normal I/O.
   - **Tests (host):** range coalescing, the exclusions, the DSM payload byte layout, IDENTIFY gating, refusal cases, and a race test (an allocation during a trim must never be trimmed).
   - **Tests (QEMU end to end):** QEMU's AHCI supports TRIM with `discard=unmap`. Use an ext2 image laid out like the Dell's, fill it and delete files, then dry-run and run. Check that:
     - every file reads back byte-identical;
     - the qcow2 allocation drops;
     - host `e2fsck -fn` is clean.
   - **Deliverables:**
     - `docs/ssd-trim.md`: design, safety argument, evidence, and a Dell rollout plan for Ian to approve (IDENTIFY check, dry-run reviewed against the partition table, a small bounded run with readback and fsck, then the full run). Recommend whether to trim periodically or at clean shutdown.
     - A Dell pair (`build_dell_image.py` with the usual four BDW kernel options) that passes `check_dell_image_qemu.py`.
