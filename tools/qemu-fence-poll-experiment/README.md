# Private QEMU fence polling comparison

This builds two matched x86-64 QEMU variants to test completion-observation
cadence while retaining every guest fence, timeout, response validation and
resource-ownership rule. It is a diagnostic control, not a default change.

```sh
nix-build --expr 'import ./tools/qemu-fence-poll-experiment { fencePollMs = 10; }' --no-out-link
nix-build --expr 'import ./tools/qemu-fence-poll-experiment { fencePollMs = 1; }' --no-out-link
```

Both use the repository's pinned QEMU and dependencies, x86_64-softmmu only,
and omit documentation and auxiliary tools. Test the rebuilt 10-ms baseline
before comparing the 1-ms candidate; neither is binary-identical to the normal
multi-target package. The only source substitution between these variants is
the `virtio_gpu_fence_poll` rearm interval. The build rejects a changed/missing
patch site. QEMU package tests are disabled upstream in the pinned Nix recipe;
a successful build alone is not acceptance.

Select an output explicitly using the existing `BROOK_GPU_QEMU` launch override
on fresh private disks. Verify the actual executable, dependencies, guest
kernel/module hashes and audio settings. Keep guest synchronization unchanged.
Use separate timestamped host fence traces (`-msg timestamp=on`), complete app
jobs, source/frame progress, accounting and clean shutdown/filesystem checks.
Any presentation gain would implicate observation cadence, not establish that
a guest fence is redundant. Faster polling can add host wakeups/CPU cost.

See [the timing audit](../../docs/timing-scheduler-audit.md) for unresolved
clock, scheduling, audio continuity and default-promotion requirements.

Both variants now pass build, QMP smoke, timestamp and disassembly checks. One
matched native pair shows a substantial presentation improvement with 1-ms
polling, with variable additional CPU cost and remaining short audio gaps. See
[the measured comparison](../../docs/gpu-fence-timing-control.md). This remains
a private diagnostic override; no launch default changes.

The later bounded follow-up in `tools/qemu-fence-poll-pkg` also accepts
`fencePollUs = 250` or `1000` with `fencePollMs = 1`:

```sh
nix-build tools/qemu-fence-poll-pkg --arg fencePollUs 250 --no-out-link --cores 8 --max-jobs 1
```

This opt-in variant changes the fence timer allocation to nanosecond units and
rearms from `qemu_clock_get_ns`, with an explicit microsecond interval. Both
patch sites must match exactly. Other timers, fence creation/response handling,
guest synchronization and ownership remain unchanged. Comparing it with the
legacy 1-ms output tests finer polling including its deadline quantization;
use the nanosecond-unit 1,000-us arm to isolate interval from quantization if
the broader comparison justifies further work. Leaving `fencePollUs` unset
resolves to the exact existing legacy derivation/output, verified by evaluation.
No finer-poll performance or correctness acceptance is established by a build.
