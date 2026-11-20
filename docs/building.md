# Building and running

## Prerequisites

A Linux host with [Nix](https://nixos.org/download). Everything else (clang
and lld, CMake, Ninja, the musl cross-compilers, QEMU and OVMF) comes
from the pinned `shell.nix`:

```bash
nix-shell
```

## Build

```bash
./scripts/build.sh Release        # kernel, bootloader, drivers and host tests
./scripts/create_nix_disk.sh      # the Nix app disk (needed once; large first download)
./scripts/build_all.sh Release    # everything above, plus disk images and bundled games
```

The desktop needs the Nix app disk: its boot scripts start the Wayland server
and apps from it.

Use `Debug` instead of `Release` for a debug build. Output goes to
`build/release` (or `build/debug`).

## Run in QEMU

```bash
./scripts/run-qemu.sh --release                        # one VM
./scripts/run-qemu.sh --release --script desktop       # boot straight to the desktop
./scripts/run-qemu.sh --release --headless --vnc :1    # no window; connect with VNC to :1
./scripts/run-qemu-pair.sh --release --script wm       # two VMs on a virtual network
```

`--script NAME` boots with `data/scripts/NAME.rc`. Other options include
`--no-audio` (audio otherwise uses PipeWire), `--bridge` (enable the debug
bridge) and `--debug` (QEMU's GDB stub on port 1234). The VM gets 8 CPUs and
8 GB of memory by default; set `BROOK_SMP` and `BROOK_MEMORY` to change that.

Graphics are software-rendered unless you ask for the host GPU, which needs a
GPU render node on the host:

```bash
BROOK_GPU=gl ./scripts/run-qemu.sh --release --script desktop      # OpenGL through VirGL
BROOK_GPU=venus ./scripts/run-qemu.sh --release --script desktop   # also Vulkan through Venus
```

Add `BROOK_GPU_DISPLAY=sdl` to show the accelerated output in a window. On Windows, `scripts\run-qemu.bat` boots a Linux-built image with
[QEMU for Windows](https://www.qemu.org/download/#windows).

## Tests

```bash
ctest --test-dir build/release/host_tests -LE stress   # host-native tests
ctest --test-dir build/release/host_tests -L stress    # stress tests (slower)
./scripts/run-tests.sh release                          # bare-metal tests in QEMU
```

The chrome tests compare the desktop chrome pixel for pixel against images in
`src/tests/goldens`. After an intentional visual change, regenerate them with
`build/release/host_tests/test_chrome_goldens --update` and review the diff.

## Building for the Dell XPS 13

The laptop boots a kernel and initrd pair built by:

```bash
python3 scripts/build_dell_image.py --shared PRIVATE_DIR --payload-id my-build --output build/dell/pair \
  --clearwater-shell \
  --kernel-option BROOK_BDW_SCANOUT_FLIP=ON --kernel-option BROOK_BDW_PERSISTENT_SCANOUT_MAP=ON \
  --kernel-option BROOK_BDW_ZERO_COPY_SCANOUT=ON --kernel-option BROOK_BDW_VIDEO_ENGINE=ON
python3 scripts/check_dell_image_qemu.py --pair build/dell/pair --attempts 4
```

`--clearwater-shell` is required: without it the image has no taskbar or
launcher. The QEMU check boots the same pair in a guarded virtual machine
before it goes near the laptop. `scripts/dell_deploy.py` installs a checked
pair over the debug bridge, keeping the previous one for rollback. The
installed apps live in the laptop's own Nix store and are updated separately
from the kernel image.

`PRIVATE_DIR` holds machine-specific files kept outside the repository: the
signed loader assets, the update key and, optionally, `panic-ingest-url`
(the address of the panic QR scanner).

## The debug bridge

`tools/vm_bridge/server.py` exposes Brook to tools and AI agents as an MCP
server, or from the command line:

```bash
BROOK_ROOT=$PWD python3 tools/vm_bridge/server.py --help
```

It can attach to QEMU runs (`vm_*` tools) and to opted-in real machines
(`hw_*` tools) for screenshots, input, logs, command jobs, file transfer and
deploys. Start with [the orientation](../tools/vm_bridge/ORIENTATION.md) and
[the bridge reference](../tools/vm_bridge/README.md).

## Building Brook on Brook

Brook can rebuild itself: a fresh system installs the SDK with `nix-install`,
clones the repository, builds and boots a new kernel, and passes the native
test suites. See [bootstrap](bootstrap.md).
