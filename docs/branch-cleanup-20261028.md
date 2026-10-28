# Branch cleanup — 28 October 2026

Main at `85e3d9db` contains the validated rounded-glass pass and Quake III
README hero. Fetched origin before auditing; there were no stashes. Three
historical comparison worktrees are detached and are left untouched.

Audited 29 non-main local branches and 68 non-main remote branches. Of these
97 refs, 76 are ancestors of main. The other 21 refs resolve to 20 distinct
archive names (one local/remote duplicate). Deleted the 23 merged local and 53 merged remote branch names. The six
unmerged local and 15 unmerged remote branches remain pending explicit
approval: automatic review rejected the original combined deletion. Unique
history is also preserved with the published annotated remote tags below.
This is a namespace cleanup, not acceptance or merging of those experiments.

Ancestry was checked with `git merge-base --is-ancestor`; unmerged refs were
also compared with `git cherry`. Patch equivalence is not proof that a whole
feature is present, and patch-unique commits are not automatically valuable.
The older selective-recovery assessment in [branch-recovery.md](branch-recovery.md)
and its REC backlog remain relevant.

## What remains worth recovering selectively

- `diag/bro208-switch-txn`: 69 patch-distinct commits, including crash/profiler
  tools, read-ahead and old input/window work. It also changes the graphics and
  scheduler baseline; do not merge the tip wholesale.
- `cherry-pick/stable-inodes-valuable`: old input/window/shell changes; its
  reverted shift-state patch must stay reverted.
- Nix closure repair/transitive-library branches: source candidates for targeted
  installer tests. Old generated executables should not be restored.
- Intel i801 SMBus driver: potentially useful when physical hardware is resumed.
- Six isolated newer ext2/GPU/frame-clock experiments: retain as unaccepted
  measurements/prototypes, with rejection and lifetime caveats in existing notes.
- Older demand-paging/process-lifecycle, visual, yquake and diagnostic branches
  are historical references. Compare against today's implementation first.
- The old FPU-state and profiler-fsync fixes are patch-equivalent to main.

No unique tip is discarded merely because its branch name looks obsolete.

## Recovery tags

- `archive/20261028/cherry-pick/stable-inodes-valuable` → `0076f9d4951a3a35991f0c5dbc59dbf310cf6d8c`
- `archive/20261028/diag/bro208-switch-txn` → `13783b3c135333d5b4299a03abc0ba0e1ab20d75`
- `archive/20261028/diag/hang-dump-block-reasons` → `e43f042dd365b87386ee1c86d6f3270921307a1d`
- `archive/20261028/experiment/ext2-indirect-batching-20261015` → `a8e02e50d0e95e2bc29d67121a74cb537d69c5a8`
- `archive/20261028/feat/screenspace-alpha-compositor` → `e638be742a742669f290e91999e3d26e15815005`
- `archive/20261028/feat/waterline-chrome` → `dbcfee11b09685de29b4972acc1a5e04f5b3f925`
- `archive/20261028/feat/yquake2-gl` → `c6810b198e82e89679c3e32c2bae58a8ae1e9ebb`
- `archive/20261028/feature/demand-paging` → `9cfcc67860c4861c1ed6fa47b127ad1338b428e4`
- `archive/20261028/feature/i2c-driver` → `91802110abca1ffe0cb44c741c8d045887751cec`
- `archive/20261028/feature/process-lifecycle` → `7348f0b973684962a6229d896746b9d969f2f483`
- `archive/20261028/fix/bro176-aslive-incarnation` → `47e4828203f44f97def2ab79bd6c023ac456c07b`
- `archive/20261028/fix/bro187-fpu-state-across-interrupts` → `b5cda3cbaebedddde792be40fcdbf2b75a6a12ca`
- `archive/20261028/fix/nix-install-closure-verify` → `fe75fe216a8eaf29f4ae9be200546f5b49a7e9ca`
- `archive/20261028/fix/nix-install-transitive-libs` → `820b42a4f47e3b231c32db76d98e4a914316be5f`
- `archive/20261028/fix/profiler-durability-fsync` → `b54327ce0270aa507476c50b383edd9d9bca3242`
- `archive/20261028/wip/fenced-detach-experiment-20261022` → `031e2d482afe518e90360c9cc8c9fdcd304fef8a`
- `archive/20261028/wip/fine-fence-poll-experiment-20261022` → `c3b295296c2d0e3b89da8347245a3915877c8a13`
- `archive/20261028/wip/late-vertex-recovery-20261022` → `a9486a7a77750ef61ab084e7b3796e74226c0af3`
- `archive/20261028/wip/layer-damage-experiment-20261022` → `88875cf23e89acbeaca8979df9d4e15c4e7087dc`
- `archive/20261028/wip/phase-frame-clock-experiment-20261022` → `4d8612bbd12378ab3486cc327907cd852b295ee6`

Restore a branch with `git switch -c recovered-example archive/20261028/<old-name>`.
Fetch tags first in a fresh clone. A verified full local Git bundle, exact ref
manifest, patch-equivalence results and remote deletion receipt are retained
under `build/branch-audit-20261028/`. No worktree, disk image or VM artifact is
removed by this cleanup. Completed remote deletion used exact-tip leases and an atomic merged-only
push; archive publication was a separate successful non-destructive push.
The rejected combined operation did not execute.

## Pending branch-name deletion approval

All of these tips have published archive tags above. Deleting the branch names
would retain their code in those tags. The local ext2 branch and its remote
counterpart share a tip. Other worktrees and disk images remain untouched.

- `refs/heads/experiment/ext2-indirect-batching-20261015` at `a8e02e50d0e9`
- `refs/heads/wip/fenced-detach-experiment-20261022` at `031e2d482afe`
- `refs/heads/wip/fine-fence-poll-experiment-20261022` at `c3b295296c2d`
- `refs/heads/wip/late-vertex-recovery-20261022` at `a9486a7a7775`
- `refs/heads/wip/layer-damage-experiment-20261022` at `88875cf23e89`
- `refs/heads/wip/phase-frame-clock-experiment-20261022` at `4d8612bbd123`
- `refs/remotes/origin/cherry-pick/stable-inodes-valuable` at `0076f9d4951a`
- `refs/remotes/origin/diag/bro208-switch-txn` at `13783b3c1353`
- `refs/remotes/origin/diag/hang-dump-block-reasons` at `e43f042dd365`
- `refs/remotes/origin/experiment/ext2-indirect-batching-20261015` at `a8e02e50d0e9`
- `refs/remotes/origin/feat/screenspace-alpha-compositor` at `e638be742a74`
- `refs/remotes/origin/feat/waterline-chrome` at `dbcfee11b096`
- `refs/remotes/origin/feat/yquake2-gl` at `c6810b198e82`
- `refs/remotes/origin/feature/demand-paging` at `9cfcc67860c4`
- `refs/remotes/origin/feature/i2c-driver` at `91802110abca`
- `refs/remotes/origin/feature/process-lifecycle` at `7348f0b97368`
- `refs/remotes/origin/fix/bro176-aslive-incarnation` at `47e4828203f4`
- `refs/remotes/origin/fix/bro187-fpu-state-across-interrupts` at `b5cda3cbaebe`
- `refs/remotes/origin/fix/nix-install-closure-verify` at `fe75fe216a8e`
- `refs/remotes/origin/fix/nix-install-transitive-libs` at `820b42a4f47e`
- `refs/remotes/origin/fix/profiler-durability-fsync` at `b54327ce0270`
