# Directory types and SteamRT provider cache

getdents64 no longer labels every non-directory as DT_REG. The VFS DirEntry
currently distinguishes only directories, so other entries return DT_UNKNOWN.
This is a valid Linux result and lets callers inspect the exact type with
lstat. No module ABI change is required.

The incorrect DT_REG caused a concrete failure in unchanged SteamRT's bundled
ldconfig: two symlinks to the same library bypassed its symlink handling and
triggered a duplicate auxiliary-cache entry abort. A reduced directory and a
scoped syscall trace reproduced it. Python scandir reported is_symlink=false
while lstat reported S_IFLNK. The corrected kernel generates the real cache
without that abort.

The portable raw-getdents64/readdir fixture checks regular files, directories,
links to files/directories, chained links and dangling links. It passed 54
checks on Linux and 55 on Brook under /data/home and /tmp, both Ext2-backed
in that image (the extra check is the
DT_UNKNOWN fallback). Existing seal, mount/container and nine native regression
programs plus Mousepad passed. Final-ELF entry/scheduler mutation audits, empty
job/groups, heap, checked shutdown, original wait and four offline filesystem
checks passed.

Evidence: `build/workloads/dirent-types-20261023/completion-audit.json`.
Session: `d4d4a0f0d3224f0e824b9b5a71772cc2`.
ABI-44 kernel with 13 modules:
`a69a9f7aeb77f4211aeffc9252fe1d1a4b93512309189e9632949d39e9406f91`.

`scripts/workloads/steamrt_provider_cache.py` generates a genuine cache from
installed Nix exports and selected provider directories. It follows exported
library symlinks within the installed store closure, records its approved
manifest, disables symlink rewriting and rejects cache targets outside that
manifest. It does not scan unrelated cached packages or Steam runtime library
directories. Actual guest generation validated 1,474 library entries from 309
directories. Cache SHA256:
`0882a7c77773d082a3709432334581b9fc1e31eac8a7a6d5f1d913bb21b12b7e`.

The RAM FAT root cannot create new /etc files dynamically. For this diagnostic,
a preserved /etc snapshot with the cache was bound over /etc. Unchanged SteamRT
then completed its main graphics/provider library capture and libc collection.
The next failure is a missing real /lib64/ld-linux-x86-64.so.2 pathname: execution
fallback aliases do not supply an openable retained file. Reproducible initrd
cache/loader provisioning is next. No SteamRT child or login acceptance yet.
Failed provisioning experiments are retained explicitly as diagnostics.
