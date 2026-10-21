# Optional client GPU timing probes

These probes run inside a Linux-compatible application. They do not measure
physical scanout or isolate CPU time from scheduling. Build with a compiler and
libc compatible with the guest, and use fresh absolute output paths:

```sh
cc -O2 -shared -fPIC sdl_swap_trace.c -ldl -o swap.so
cc -O2 -shared -fPIC drm_ioctl_trace.c -ldl -o drm.so
```

Inject into the application being measured, for example with these environment
variables supplied through `vm_guest_run`:

```text
LD_PRELOAD=/tmp/swap.so:/tmp/drm.so
BROOK_SWAP_TRACE=/tmp/swaps.csv
BROOK_DRM_TRACE=/tmp/drm.csv
BROOK_DRM_TRACE_SKIP_SWAPS=50
BROOK_SWAP_SETTINGS=/tmp/swap-settings.json
```

The swap probe uses the **SDL2 ABI**, including `SDL_GL_GetSwapInterval(void)`.
It also works with SDL2-compat's SDL2 interface. Do not inject it into a native
SDL3 application. The optional settings file reports the actual getter's
minimum/maximum over recorded swaps; a saved game cvar alone does not prove
that API state. Missing getters or zero queries do not establish VSync state.

The DRM probe groups `drmIoctl` calls by thread, request number and phase:

- `0`: unscoped, including startup, skipped swaps and non-swap threads.
- `1`: between swaps on the swap thread, including shutdown after its last swap.
- `2`: inside swap on that thread.

Phases begin after the configured number of skipped swaps. They do not align
automatically with game demo markers. Without the SDL hooks all calls remain
unscoped. Aggregates have a fixed capacity; elapsed times include waits and
preemption. Logarithmic histogram percentiles are bounds, not exact quantiles.
Concurrent thread totals must not be added as frame wall time. In particular,
bound the shutdown tail before subtracting outside-swap DRM totals from the
between-swap intervals.

Both probes avoid per-frame allocation and output. Normal process teardown
writes files with exclusive creation; existing files are never overwritten.
An abnormal exit can leave no complete trace. DRM trace admission closes at
teardown, and any admitted call still in flight invalidates the capture.
Overflow, clock errors, saturation and inconsistent histograms also invalidate
it. Keep failures as evidence, rather than interpreting them as zero-cost work.

After a clean private VM shutdown and offline filesystem checks, extract the
files and verify their sizes/hashes against guest metadata. Avoid bulk stdout
transfer through the bridge. Analyze with:

```sh
python3 scripts/workloads/analyze_swap_trace.py /path/to/swaps.csv
python3 scripts/workloads/analyze_drm_trace.py /path/to/drm.csv
python3 scripts/test_drm_ioctl_trace.py
```

Only the opt-in preload environment activates these probes. They do not change
normal application launchers or VM settings. Keep instrumented results separate
from uninstrumented throughput controls.
