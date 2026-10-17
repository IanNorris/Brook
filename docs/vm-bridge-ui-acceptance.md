# Semantic VM window controls — acceptance, 17 October 2026

Bridge 0.5.0 adds `vm_windows`, `vm_window` and `vm_ui`. It implements the
window-control portion of [the bridge specification](vm-bridge-next.md).
Installed-app discovery/launcher activation and the remaining observation/event
conveniences are still pending. Reload the MCP process after updating its source;
its previously loaded tool schema does not acquire new tools automatically.

The kernel and the rebuilt `waylandd` package are both required for Wayland
connection-origin metadata. An older server leaves origins unknown and close
unsupported, rather than attributing a client window to the shared server.

## Environment and evidence

Tests used one isolated headless QEMU VM, eight vCPUs, 4 GiB RAM, software
composition at 1920×1080, and private boot/data/Nix disk images. Speaker playback
was disabled. No host desktop input/capture was used. The tested frozen kernel
SHA-256 was `f751a9e14a71b986be31945f6c41fe00c64260e138380839e1033e480939dad2`;
`waylandd` SHA-256 was
`d7d9725ecd2e6294b1e03fdd5a16996d9cb32ff112ecf4025af37596ec2c2893`.

Runtime evidence stays ignored below
`build/workloads/gallery-fresh-20261017/`:

- `ui-acceptance/actions.json`: native Terminal and GTK Mousepad focus, minimize,
  maximize while minimized, explicit maximized/normal restore, identical request
  recovery, actual Wayland buffer extent, and graceful close/group exit. Closing
  Mousepad preserved the native app and shared Wayland server.
- `ui-acceptance/minimized-terminal.json`: a terminal exec'd `busybox sleep`, was
  minimized, then closed by SIGHUP. Window disappearance and exact group exit
  completed. Terminated-owner cleanup now covers invisible/minimized windows;
  registry-backed process references and exact terminal lookup protect cleanup.
- `ui-acceptance/recovery.json`: a reused window slot rejected the old creation
  identity; takeover blocked both the host path and direct guest requests;
  release restored normal controls.
- `ui-acceptance/reboot.json`: a fresh guest boot with unchanged host session
  generation rejected old window references, marked old jobs/UI operations lost,
  and completed a new command. The experiment launcher's no-reboot policy first
  stopped QEMU; the test explicitly changed its reboot action, reset and resumed
  it. See the reset follow-up in [the backlog](backlog.md).
- `ui-job-acceptance/jobs.json`: ten command-job scenarios passed again on job
  API 2, including literal argv/environment and identity, separate streams,
  nonzero/signal exit, deduplication, delayed descendants/EOF, retention loss,
  interrupted full-pipe producers and recovery through a separate host process.
- `ui-final-build.log`: 28 hosted kernel tests passed, including concurrent
  first-wins epoch initialization and rejection of invalid/zero tokens.
- `ui-final-check/filesystems.json`: checked guest filesystem/device flush,
  QEMU exit, serial panic/I/O-error screening and read-only data/Nix/FAT checks
  completed; all three filesystem checker exit codes were zero.
- Bridge suite: 110 tests completed successfully with five optional encoder tests skipped.
  Host tests cover lost replies, read-only recovery, stale slots/boots, reboot
  between status and output, abandoned staging, and serialized two-client
  takeover/release with reversed completion pressure.

`tools/vm_bridge/accept_ui.py --session SESSION --out build/ui-acceptance`
repeats the normal workflow on an already prepared disposable VM with a clean
Mousepad document and a native Terminal. It closes both apps. It does not claim
to exercise Applications launcher activation.

## Timing and limits

Twelve final action-and-observation calls had a median of 1.54 seconds and a
range of 1.37–5.03 seconds. These are host wall times for dispatch, bounded wait
and window observation, not compositor rendering latency or sampled CPU time.
An earlier lost observation was recovered by operation ID without repeating the
mutation. Framed TCP clients now skip debug-ring replay, including fragmented
commands. The listener's idle accept sleep and per-chunk connections remain
visible overhead; no CPU-overhead reduction or broad workload speedup is claimed.

Completed WM actions are distinct from client configure acknowledgements, redraw,
window disappearance and application exit. Process liveness is sampled after the
locked window snapshot. A Wayland origin is an accepted Unix connection origin,
associated by its provider; inherited/passed sockets can outlive that origin.
It grants no authority to kill that PID. Current waits track its exact group,
not every application descendant.

Whole-VM snapshot rollback still requires a changed host generation. Legacy
coordinate input and index-based focus do not yet share the new boot guard.
Manual user-started VM acceptance, adversarial syscall registration cases and
remaining spec items are follow-ups. This checkpoint is not full-spec or
real-workload acceptance.
