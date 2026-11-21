# Chromium on Brook

Brook Chromium uses Chromium's Linux namespace sandbox and seccomp-BPF for
renderers. The launcher disables the setuid helper. Use the matching kernel
and network desktop profile in the same deployment and rollback. The launcher
checks the kernel's `/proc/self/fdinfo` directory and `/proc/self/ns/net`
namespace handle before starting, and exits with an explanation if either is
missing.

## Isolation and verification

The zygote creates user, PID and network namespaces, seals its root through
a short-lived helper sharing its filesystem context, and drops privileges.
Brook retains the sealed directory after the helper exits and keeps its
identity tied to that task's incarnation. Chromium installs the renderer
filter with `PR_SET_NO_NEW_PRIVS` and seccomp-BPF. Procfs shows the live state
through `NoNewPrivs` and `Seccomp` in `/proc/<pid>/status`.

In the browser, open `chrome://sandbox` and check for a Namespace layer 1,
PID and network namespaces, seccomp-BPF, and TSYNC support. For a renderer
process, the corresponding procfs status is `NoNewPrivs: 1` and `Seccomp: 2`.
Brook implements Chromium's UID/GID-map and setgroups operations, namespace
clone and unshare, and the seccomp filter interface. Browser startup does not
require `setns`, ptrace/Yama or Landlock, which remain unimplemented. Chromium
reports the Yama checks as No.

## Accelerated GPU rollout

The ANGLE/Iris and opt-in VA-API configurations temporarily pass
`--disable-gpu-sandbox`. This applies to the GPU process. Renderer namespace
and seccomp isolation stays enabled. In the software configuration
(`BROOK_BROWSER_SOFTWARE=1`), Chromium can install the GPU-process filter
too.

The hardware verification for removing that flag is a real render node
with active Chromium compositing and rasterization, a GPU process reporting
`Seccomp: 2`, and sustained playback through Wayland GPU buffer offers
without GPU-process restarts. The headless guest verifies the selected
options and renderer isolation. It has no Broadwell render node.
