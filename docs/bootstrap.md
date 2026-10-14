# Building Brook inside Brook

Use an isolated, writable experiment clone with the VM bridge enabled, one vCPU,
and silent host audio. Keep the normal prepared checkpoint and source checkout
separate from the test. The development toolchain package and installation
commands are in [tools/bootstrap-pkg](../tools/bootstrap-pkg/README.md).

The test has distinct milestones: guest package installation, a real HTTPS Git
clone and the two required submodules, building the kernel/bootloader/modules
and bare-metal tests in the guest, running hosted tests in Brook, then booting
the resulting guest-built kernel. A host-exported binary SDK supplies tools;
that does not imply that Nix or the compiler was rebuilt inside Brook.

## Compatibility defects found during preparation

The first real Git clone found that BusyBox fallback advertised every unknown
system-bin command as an executable. Git stopped PATH search before reaching
its real remote helper. The fallback now checks the shipped static BusyBox's
actual applet names. `scripts/generate_busybox_applets.py ./busybox_static`
regenerates the allowlist when replacing that binary; rebuild the kernel too.

Git's ordinary failure exit code 128 was also reported as successful: wait4
inferred signal death from every code >=128, turning 128 into zero. Termination
now records Linux wait status explicitly, preserving all eight normal exit bits
and distinguishing signals. The `waitcheck` probe validates raw exit/exit_group
codes 0/1/127/128/255, SIGTERM/SIGKILL and Bash `set -e` external-child behaviour.
The process state release stores also use the field's actual byte width.

The real kernel link command contains over 100 arguments. Previous exec copying
and stack setup silently capped vectors at 64 entries. Shared limits now allow
512 arguments, 256 environment entries, 64 KiB argument strings and 32 KiB environment
strings. Invalid pointers return EFAULT and count/byte overflow returns E2BIG
before image replacement. Shebang argument insertion is checked and private;
it no longer uses a shared scratch buffer or discards trailing arguments.

`execcheck` exercises 128/256 arguments, over 48 KiB strings, exactly 512 argv and 256
env entries, a 20 KB single string, count/byte overflow, unmapped-page pointer and
string boundaries, a NUL at the final mapped byte and shebang preservation.
Both probes passed in isolated Brook regression guests, as did 13 host tests.
Their sources and invocation examples live in [tools/at-syscall-probe](../tools/at-syscall-probe/README.md).

Runtime evidence is retained under ignored `build/experiments/results/bootstrap-20261014/`
and `bootstrap-compat-20261014/`, and the VM registry keeps serial logs and the
actual booted kernel identities. Package payloads, images and logs stay out of git.
