# App Gallery recovery work

The current installer publishes the manifest before refreshing binaries,
libraries, desktop links and the launcher index. Per-file atomic replacement
prevents an individual failed update from deleting its old live link. It does
not make the complete package operation atomic. An install can return an error
with its registration already present and some links already changed.

## Next recovery contract

Use an explicit, durable operation record beneath the profile mutation lock.
The record needs the old and intended registrations, operation identity and a
complete plan for exported paths, including previous ownership and targets.
Do not infer recovery from PID liveness alone. A stale lock still requires
establishing that its original writer cannot run; a reused PID is insufficient.

A profile operation should follow these stages:

1. Fetch and validate complete store paths without changing the active profile.
2. Resolve all exports and conflicts, including base-image registrations and
   dependency libraries, before publishing. Record every destination's owner;
   reject unsupported collisions instead of silently choosing by install order.
3. Stage the replacement manifest, exported links and launcher metadata, then
   flush the plan and staging directories before making the record recoverable.
4. Publish under an explicit commit state and synchronize directories. A crash
   during publication must leave enough information for idempotent forward
   completion or rollback; report which state recovery chose.
5. Verify the active profile matches the plan before retiring its operation
   record. Removal must restore another valid owner's export when appropriate,
   rather than delete a shared file or expose an unregistered cached package.

Decide between a profile-generation switch and an operation journal after
checking Brook's directory/symlink resolution and open-file semantics. A
single generation switch is attractive for profile links, but launcher state
outside `/nix/profile` must also participate or be rebuilt from the committed
generation. Mutable runtime-library links currently added inside store packages
must be accounted for rather than assumed immutable. The base image needs an
export/ownership inventory before its packages become removable in Gallery.

## Required failure matrix

Test forced termination before and after every persistent stage, including
midway through a many-file update. Reboot and recover the same private disks;
check exact manifest bytes, valid binary/library targets, launcher eligibility,
shared ownership and preserved user documents. Include repair and removal,
not only install. Failed recovery must remain visible and retain evidence.

Run a genuinely full private ext2 volume, distinct from injected syscall errors.
Exercise space exhaustion during store extraction, manifest staging, symlink
creation, launcher generation and journal cleanup. Release reserved test space
and verify a subsequent repair can finish without redownloading complete store
paths. Require clean command lineages, checked shutdown and offline filesystem
checks at each accepted endpoint. Power-loss durability is a separate claim
from process termination and requires corresponding storage-ordering tests.
