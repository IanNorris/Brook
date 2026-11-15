# Chromium sandbox on Brook

_14–15 Nov 2026, Dell gen 401 (`dell-integration8b-20261115`). Probes: a static C program
(`nsprobe`) and the installed `brook-chromium` launcher without `--no-sandbox`, private profile._

## Status: Chromium still needs `--no-sandbox`

Chromium's Linux sandbox has two layers, and Brook currently provides neither:

1. **The layer-1 process sandbox.** Either the namespace sandbox (unprivileged user, PID and network
   namespaces) or the setuid `chrome-sandbox` helper.
2. **The layer-2 seccomp-bpf syscall filter** for renderers and the GPU process, installed with
   `PR_SET_NO_NEW_PRIVS` plus `seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC)`.

What the kernel provides today (uid 1000):

| Primitive | Result |
|---|---|
| `clone(CLONE_NEWUSER)` | works (the Steam/bubblewrap foundation, `d7600678`) |
| `clone(CLONE_NEWUSER\|CLONE_NEWNS)` | works |
| `clone(CLONE_NEWUSER\|CLONE_NEWPID)`, `…\|CLONE_NEWNET` | `EINVAL`: no PID or network namespaces |
| `unshare(2)` (any namespace flag) | `ENOSYS` |
| `setns(2)` | not implemented |
| `/proc/self/ns/*` in the probe | absent (namespace handles exist for `/proc/PID/ns` lookups used by bubblewrap; see `proc-namespace-handles.md`) |
| `/proc/self/uid_map` in a new user namespace | empty |
| `prctl(PR_SET_NO_NEW_PRIVS)` | works |
| `seccomp(2)` (`GET_ACTION_AVAIL`, `SET_MODE_FILTER` with TSYNC) | `ENOSYS` |
| `prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER)` | `EINVAL` (explicitly refused in `prctl`) |
| Landlock (`landlock_create_ruleset` etc.) | not implemented. No branch has it; the Steam work covered user and mount namespaces, mount propagation and `pivot_root` |

Chromium without `--no-sandbox` (with or without `--disable-seccomp-filter-sandbox` or
`--disable-namespace-sandbox`) aborts at startup:

```
FATAL:sandbox/linux/suid/client/setuid_sandbox_host.cc:166] The SUID sandbox helper binary was found,
but is not configured correctly. ... /nix/store/s8d4…-chromium-148.0.7778.96-sandbox/bin/__chromium-suid-sandbox
is owned by root and has mode 4755.
```

Chromium probes the namespace sandbox first. That needs user, PID and network namespaces together, so
on Brook it falls back to the setuid helper. Nix ships that helper without the setuid bit, and Brook
has no setuid-root install path for it, so Chromium refuses to run.

## Current mitigation

- **Keep `--no-sandbox`** in `brook-chromium`.
- **Hide the infobar with a managed policy**, `{"CommandLineFlagSecurityWarningsEnabled": false}`, at
  `/etc/chromium/policies/managed/brook.json`. nixpkgs Chromium reads `/etc/chromium/policies`.
  - **Delivery:** Brook's `/etc` comes from the initrd's FAT root, so `prepare_hardware_kernel_pair.py`
    writes the file into every Vulkan-desktop initrd. `verify_guarded_desktop_candidate.py` requires it
    to match `data/chromium/brook-managed-policy.json`.
  - **Checked on gen 401:** with the policy the bar is absent; without it the bar reads "You are using
    an unsupported command-line flag: --no-sandbox".
- **`--test-type` rejected.** It would also hide the bar, but it is a test-harness switch with other
  effects (it changes some first-run, infobar and automation-related behaviour), so the policy is
  preferred.

## Real sandbox: what Brook would need

**Option A: namespace sandbox plus seccomp-bpf (no setuid).** Recommended; it matches modern
distributions.

1. **PID namespaces** for `clone(CLONE_NEWPID)`: per-namespace PID numbering, init (PID 1)
   semantics including reaping orphans and namespace teardown when init exits, and `getpid`/`kill`/
   `wait` translation across levels. This is the largest item, because Brook has a single global PID
   table (`TaskIdCapacity` 1024).
2. **Network namespaces** for `clone(CLONE_NEWNET)`: Chromium only needs an empty namespace with no
   interfaces (a loopback is optional), so a minimal "no network" namespace that fails socket
   connects outside AF_UNIX would do.
3. **User-namespace completeness:** writable `/proc/PID/{uid_map,gid_map,setgroups}` (Chromium writes
   these in the namespace sandbox), and `/proc/self/ns/{user,pid,net}` links.
4. **`unshare(2)` and `setns(2)`**, used by parts of the zygote and by bubblewrap.
5. **seccomp-bpf:** `seccomp(2)` with `SET_MODE_FILTER`, `TSYNC` and `GET_ACTION_AVAIL`, plus classic
   BPF validation and an interpreter run on syscall entry (filters inherited over fork and stacked);
   return actions `ALLOW`, `ERRNO`, `TRAP` (SIGSYS with `si_syscall`/`si_arch`, which Chromium's broker
   relies on), `KILL_PROCESS` and `TRACE` (may be `ENOSYS`). Chromium's policies also inspect
   arguments, so `seccomp_data` must carry `nr`, `arch`, `instruction_pointer` and `args[6]`.
6. **`/proc/sys/kernel/yama/ptrace_scope` and `/proc/self/status` `Seccomp:` fields:** cheap, but
   Chromium checks them.

**Option B: the setuid helper.** A root-owned mode-4755 copy of `__chromium-suid-sandbox`, and setuid
exec support, still needs PID and network namespaces underneath (the helper creates them with clone),
plus seccomp for layer 2. It saves only the user-namespace mapping work, and adds a setuid-root binary
to the store install path. Not recommended.

**Size estimate (option A):**

| Item | Estimate |
|---|---|
| seccomp-bpf (cBPF validator, interpreter, filter inheritance, TSYNC, SIGSYS delivery, host tests) | 1.5–2.5k lines, about a week |
| PID namespaces (translation layer over the global table, init/reaper semantics, procfs views) | 2–3k lines, 1–2 weeks; touches scheduler, signal and wait paths |
| Minimal network namespace, `unshare`/`setns`, uid/gid map writes, `/proc/self/ns` links | about 1k lines, a few days |

That is 2–4 weeks of kernel work in total. Seccomp-bpf alone has value: with
`--disable-namespace-sandbox` Chromium could run layer-2 filtering only. It also helps
bubblewrap/pressure-vessel (`--seccomp`). So it is the natural first step.
