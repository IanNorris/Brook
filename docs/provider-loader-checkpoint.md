# Reproducible provider cache and loader

`steamrt_provider_cache.py --loader PATH` can package the exact installed x86-64
host glibc loader alongside its validated cache/configuration. It checks that
the loader's sibling libc belongs to the cache and records source and hashes.
The installed store closure must be retained; regenerate the payload when that
profile changes. This is not a generic host /etc import.

Set BROOK_PROVIDER_PAYLOAD to the output directory while making the initrd.
The boot-image script checks manifest hashes and ELF architecture, packs
/etc/ld.so.cache, /etc/ld.so.conf and /lib64/ld-linux-x86-64.so.2, then verifies
all three hashes by reading the image back. Failed copy/space checks or hash
validation cannot publish a partial image. A corrupted-cache negative test
confirmed the previous image survives and the unpublished temporary is removed.
The actual payload fits the existing 2.88 MiB image. No loader or generated cache
is committed. The Nix loader's compiled-in paths remain unchanged.

Guest read-back matched the accepted cache and the actual installed loader:
loader source /nix/store/fjkx1l5cnskzrqacf08z7i8z17256w0j-glibc-2.42-61/lib/ld-linux-x86-64.so.2,
SHA256 `1640ec4d1cfcc3c19430b368cbbb057c5652eac340ecba12cf9dfbe2c3769d07`.
Direct loader --version reported glibc 2.42. Normal true, Python, Mousepad,
all container probes and nine native regressions passed. Re-running the payload
generator in the guest produced the same validated 1,474-entry cache.
All groups/jobs ended empty; heap, checked shutdown, original wait and four
filesystem checks passed. Kernel/13 modules remain the accepted ABI-44 dirent
checkpoint, unchanged.

Evidence: `build/workloads/provider-loader-20261023/completion-audit.json`.
Session: `aff83e9ffaf746a78cc8da4a2dc2d5df`.

Unchanged SteamRT passes graphics capture and now opens the real host loader.
It exposes two next pathname gaps: regular /proc/self/fd links report diagnostic
labels rather than genuine paths, and readlinkat on an empty name does not read
the retained O_PATH symlink. The mutable-runtime resolver consequently mistakes
its /lib64 symlink for a non-directory. Full stderr was extracted after checked
shutdown and matched its guest SHA256. No SteamRT child or login acceptance yet.
