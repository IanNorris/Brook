# Read-only retained bind mounts

Private retained bind roots now support a monotonic transition to
`ro,nosuid,nodev`. Ordinary writes, append, truncate, creation, removal, rename,
symlink/socket creation and attribute changes reject with EROFS through that
attachment, including retained directory handles after detach. Access and
filesystem/mountinfo flags report the restriction. Ext2 reads skip relatime
updates. Independent writable aliases remain writable.

The transition conservatively returns EBUSY while any non-directory data handle
exists, including ordinary read handles and vnode references held by mapping
writeback. O_PATH handles and directories do not prevent it. Read-only opens
with APPEND or CREATE on an existing file remain valid. Private COW mappings
remain valid; a read-only FD cannot create a shared writable mapping.

Restrictions cannot be removed. This checkpoint accepts attributes 6 or 7 only;
NOSUID-only device binds are the next extension. First RO transitions on copied
roots without a retained bind root return EOPNOTSUPP: legacy synthetic storage
does not yet carry full attachment provenance. This is not general device or
container isolation acceptance.

The added vnode admission field bumps the module ABI to 44. All 13 modules were
rebuilt. Descriptor-based utimensat also now pins its FD across the sleeping VFS
transaction, matching the existing chmod/chown ownership pattern.

## Validation

Identical portable probes passed Linux (111 fixture / 8 parent cumulative checks)
and Brook (118 / 8). Both exercised tmpfs-backed binds and a disk-backed source.
The native mapped-file test actually observed pending writeback holding EBUSY
after FD close/unmap; sync retired it and allowed the transition. It then
verified RO mutation rejection, writable source aliases, private COW, inherited
and detached protection. The separate restriction probe passed both systems.

All 100 host tests passed. The affected VFS/Ext2 tests passed ASAN/UBSan, with a
real Ext2 check that RO reads preserve atime. Hosted retained-reference tests
model writeback ownership; native acceptance exercises the actual FileObject
path. Final-ELF entry/scheduler audits and mutations passed. Existing container
probes, nine native regression programs and Mousepad launch/render/close passed.
Original jobs/groups finished empty; heap integrity, checked shutdown, original
VM exit zero and all four offline filesystem checks passed.

Evidence: `build/workloads/read-only-bind-20261023/completion-audit.json`.
Native session: `c7cc73989df442808b1d8d18bfdd1d84`.
Kernel: `69aea3ac7742998d0187ea7ecfde1fa8dc9b4a54d4a0ff9f52f91819210a7e91`.

## Unchanged runtime result

Bundled bubblewrap and its NNP/exec child still pass. Unchanged SteamRT now
passes the read-only host binds and exits 1 at the NOSUID-only remount for
`--dev-bind /dev /dev`. The source confirms this request preserves existing
flags and adds NOSUID without adding NODEV. SteamRT's child has not executed;
no runtime checks were bypassed or authentication attempted.
