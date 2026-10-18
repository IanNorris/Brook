# Executable permissions and interpreter preflight — 18 October 2026

Executable loading previously used an ordinary read-only open. On the checked
ext2 data mount, this allowed readable non-executables to run and rejected valid
execute-only files. ELF PT_INTERP loading also happened after the caller's old
address space was destroyed: an inaccessible interpreter terminated the process
instead of returning an exec error. Initial pathname errors could fall through
to basename aliases rather than preserving an ancestor-search denial.

`VfsOpenExecutable` now requests an internal executable-image open. Ext2 checks
regular-file execute permission against the current effective UID/GID and
supplementary groups while opening that inode. Root needs at least one execute
bit. The loader can read the image without requiring user data-read permission;
its retained vnode is consumed internally and never exposed as a readable FD.
Ordinary data opens keep their read/write requirements. The Nix permission
bypass and other backends' existing policies are unchanged.

The ELF interpreter is now opened, authorized, read and header/bounds validated
before sibling termination or address-space replacement in execve. Both owned
image snapshots stay alive until loading finishes. Missing, denied, short or
invalid interpreter images therefore return an error to the existing caller;
its threads and signal handler remain intact. Compatibility aliases are tried
only for ENOENT, never after an authorization denial. The ordinary main-path
lookup likewise preserves non-ENOENT errors and rejects directory execution.

This follows Linux's execute-intent open rather than adding a mandatory data
read check, and its ELF loader authorizes PT_INTERP through the same open path.
Sources: [exec.c](https://github.com/torvalds/linux/blob/master/fs/exec.c) and
[binfmt_elf.c](https://github.com/torvalds/linux/blob/master/fs/binfmt_elf.c).

## Tests

The owned `scripts/workloads/exec_permissions.py` fixture generates tiny x86-64
ELFs that only exit with a recognizable status. Its interpreter exits with a
different status, proving which image actually ran. Script tests use that same
owned interpreter, so they measure kernel authorization independently of a
shell's separate ability to read the script. No downloaded app or user file is
modified by this matrix.

The Linux current-owner control passes 56 cases. The Brook matrix runs 672 cases
across twelve credential combinations: root, owner, primary/supplementary groups,
owner precedence, unrelated identities and differing real/effective IDs. It
covers sixteen permission modes for static ELFs, shebang scripts and ELF
interpreters; execute-only files; symlink targets; ancestor search; directories;
and missing, short and invalid interpreter headers. Four interpreter error cases
also verify that an existing sibling thread and signal disposition survive.
Root setup and the complete credential matrix were available only in the guest;
no privileged Linux comparison is claimed.

The old ABI-19 kernel recorded 337 mismatches using this same matrix. Some
interpreter denials ended the child with status 255. The candidate records zero
mismatches and all expected exec errors return to the caller. Short interpreter
headers produce EIO; full-sized invalid ELF headers produce ELIBBAD. The
existing 686-case ordinary read/write-open matrix also passes. All 40 native
suites and the supported compiled-entry audit pass; ELF host tests additionally
cover bounded PT_INTERP extraction and interpreter load bounds.

Candidate kernel SHA-256:
`0efbc94e6188810f0f816dfba9c7ceeba8457f24e12f8645e728d812186f0134`.
The module layout remains ABI 19; no existing exported process API was changed.
Acceptance uses isolated eight-CPU, 4 GiB, host-backed disks. Evidence is retained
under `build/workloads/exec-permissions-20261018/`.

Firefox's normal catalogue launch reached the owned VP9/Opus page. Ten seconds
of captured guest audio contained 441,157 frames at 44.1 kHz, with peak 2,979,
an estimated 440.14 Hz tone and no silent 10 ms bins. Pause made the stream
inactive. The 18.3-second browser sample measured 0.934 media seconds per page
second, 20.27 video callbacks/s and 38 reported dropped frames. These are smoke
measurements, not proof of unchanged or improved performance; the sample was
slower than earlier timerfd checks and is not a matched comparison. No profiling
or screenshot capture ran during PCM measurement. Host speaker output remained
disabled. The untruncated process census verified all 290 recorded Firefox
lineage tasks as UID/EUID 1000 and GID/EGID 100. Native close drained the complete
launch lineage, and audio remained inactive afterward. Both baseline and candidate
completed checked sync, QEMU/launcher exit zero, heap checks and read-only
boot/data/Nix filesystem checks. All five tracked commands had complete stdout
and stderr, no gaps/drops, zero top-level exit and drained original/full launch
lineages. Intentional baseline exec failures remain test observations, not
successful permission controls. No kernel/device or user fault appeared in either
serial log. Five saved documents retained exact hashes and UID 1000:GID 100.

## Still unsupported or incomplete

This does not implement capabilities, set-ID execution, noexec mounts, an ETXTBSY
write-denial protocol or immutable executable identity across future re-exec.
Permission/header preflight is not a guarantee that allocation or mapping cannot
fail after exec commits. Kernel-created launches still authorize in the dispatch
caller's context before applying the target launch identity; that needs a
separate credential-context review before account isolation. Nix remains a
shared writable compatibility store with a deliberate permission bypass.

Relative PT_INTERP, legacy alias behavior, relative exec path length handling,
retained process CWD and atomic inode-relative namespace operations remain open.
This work does not establish full Linux exec semantics or finish login/accounts.
The bootstrap pair and other application regressions remain at their explicitly
pinned acceptance revisions until rerun.
