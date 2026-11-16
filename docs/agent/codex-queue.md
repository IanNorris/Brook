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
