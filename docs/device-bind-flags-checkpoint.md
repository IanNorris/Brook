# NOSUID-only bind remounts

The bounded remount API accepts attributes 2, 3, 6 and 7: NOSUID is mandatory,
with optional NODEV and RO. Restrictions remain monotonic. Unknown flags,
requests without NOSUID, removing NODEV and removing RO fail without mutation.
Existing read-only admission and retained-root restrictions remain in force.
This matches bundled bubblewrap's device-bind flag calculation, which preserves
existing flags and adds NOSUID without adding NODEV for --dev-bind.

Identical portable probes passed Linux and Brook. The RO fixture passed 111 / 8
cumulative checks on Linux and 118 / 8 on Brook, including real pending-writeback
admission. The restriction fixture passed 91 / 8 on each. Host tests cover staged
0→2→3 and 2→6→7 transitions and reject weakening. All 100 host tests, affected
ASAN/UBSan tests, final-ELF entry/scheduler audits with mutations, existing
container probes, nine native regression programs and Mousepad passed. All jobs
and groups finished empty; heap, checked shutdown, original VM wait and four
offline filesystem checks passed.

Evidence: `build/workloads/device-bind-flags-20261023/completion-audit.json`.
Session: `d5afaebf39fc446d8446a348eeb9db10`.
ABI-44 kernel, 13 modules:
`61f17ef5b9d22a2d1edd328d628cc5b1a3ff62cb777f0cfdf773c5c420192c8d`.

## Actual runtime result

Unchanged SteamRT passes the precheck mounts, then execvp(true) fails because
/nix/profile is absent inside that minimal container. Adding the real installed
coreutils store bin directory to PATH lets the original precheck run its actual
true executable. This preserves normal multicall argv[0] dispatch and uses the
existing /nix/store bind; it does not substitute a successful checker.

The first store-PATH diagnostic overflowed bridge output with approximately
8 MB of warnings. Its original identity and empty terminal state were preserved;
its incomplete output is explicitly excluded from acceptance. A subsequent
file-backed diagnostic saved the complete 8,046,101-byte stderr file, verified
after checked shutdown against its guest-recorded SHA256. It confirms the next
failure during runtime setup:

```
Unable to bind-mount "etc-passwd" on "/etc/passwd": fcntl(F_ADD_SEALS): Invalid argument
```

The runtime falls back to copying because hard links are still unsupported.
Its requested no-copy diagnostic option did not avoid the observed temporary
copy path. Profiling and Python elapsed times differ, so these logs do not
establish a performance comparison. No SteamRT child or login acceptance yet.

`scripts/workloads/steamrt_check.py` reproduces the installed runtime launch as
the desktop user with container-visible coreutils PATH and file-backed logs.
It prints only paths, hashes and bounded tails, and returns the real child exit.
Default invocation preserves the runtime's normal copy configuration; the
optional no-copy request is a separately labeled diagnostic.
