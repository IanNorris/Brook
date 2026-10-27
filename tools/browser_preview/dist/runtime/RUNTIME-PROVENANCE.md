# Brook browser emulator provenance

This runtime is QEMU 8.2.0 with the qemu-wasm WebAssembly CPU backend and SDL2
software framebuffer display. It executes the supplied Brook x86-64 kernel.
It has no KVM or virgl graphics acceleration.

## Verified published binary and corresponding source

Runtime origin: https://dbontr.github.io/linux-lab/qemu/

The JS, Wasm, worker, and license downloads were checked against the upstream
SHA256SUMS included under source/UPSTREAM-SHA256SUMS. Upstream runtime metadata
is included under source/UPSTREAM-runtime.json. These are publisher checksum
and source-pin assertions, not an independently reproduced build.

QEMU source commit: 0ef7b4e2814b231705d8371dd7997f5b72e70baf
https://github.com/ktock/qemu-wasm/tree/0ef7b4e2814b231705d8371dd7997f5b72e70baf
https://github.com/ktock/qemu-wasm/archive/0ef7b4e2814b231705d8371dd7997f5b72e70baf.tar.gz

SDL source commit: 55b03c7493a7abed33cf803d1380a40fa8af903f
https://github.com/libsdl-org/SDL/tree/55b03c7493a7abed33cf803d1380a40fa8af903f
https://github.com/libsdl-org/SDL/archive/55b03c7493a7abed33cf803d1380a40fa8af903f.tar.gz

Build scripts and patches, commit 3a2233b01d65743886413c1f3c3dd76eee931e31:
https://github.com/dbontr/linux-lab/tree/3a2233b01d65743886413c1f3c3dd76eee931e31/runtime-build/qemu
https://github.com/dbontr/linux-lab/archive/3a2233b01d65743886413c1f3c3dd76eee931e31.tar.gz

The complete runtime-build/qemu directory is also bundled under
source/build-recipe/, including its build script and every source patch.
The source Dockerfile identifies dependency sources and build versions:
Emscripten 3.1.50, glib 2.75.0, pixman 0.42.2, zlib 1.3.1 (replaced by the
build's pinned zlib commit), libffi commit adbcf2b247696dde2667ab552cb93e0c79455c84.
See source/build-recipe/patch-upstream.mjs for the exact zlib commit and hash.

QEMU's GPLv2 license is COPYING.qemu; SDL's license is LICENSE.sdl. Linux Lab
build scripts carry the MIT license included at source/LICENSE.linux-lab.
The source trees retain the individual compatible component license notices.
Firmware provenance and licenses are tracked separately with the boot assets.

## Brook experiment modification

source/prepare-runtime.py reproduces the only Wasm modification: the shared
memory import's minimum and maximum change from 2300 MiB to 1024 MiB.
The minified memory import is named a.a. The byte count is unchanged. The
upstream JS and worker are unchanged. brook-runtime.js explicitly supplies
INITIAL_MEMORY=1024 MiB. runtime.json records original and modified hashes.

The source-pinned binary includes dormant optional OneDrive/network bridge
code inherited from Linux Lab. This page provides no token, account
connection, filesystem mount, network stack, or guest networking argument.
No Linux distribution, demo disk, TempleOS ISO, or third-party game asset is
included in this runtime package.

## Optional diagnostic poll variant

`?runtime=pollfix` explicitly selects `qemu-system-x86_64.pollfix.js`, including
for pthread worker initialization. The default continues using the unchanged
publisher JS. `source/POLLFIX.json` records original/modified JS hashes and
`source/prepare-pollfix-runtime.mjs` reproduces the edit from the project scripts
location. `scripts/test-generated-poll.mjs` extracts and tests the actual
publisher/patched functions without executing an emulator.

The publisher poll binding probes descriptors with an infinite terminal wait,
even when its caller requested nonblocking polling. Empty terminal input can
therefore prevent checking ready pipes or timers. This variant probes every
fd without blocking, then uses the existing PTY wait path for up to 10 ms
before rechecking all descriptors. The worker sleeps via Atomics.wait; the
browser main thread schedules a timer and wakes it via Atomics.notify. There
is no spin loop or additional host permission. Periodic wakeups add CPU and
scheduling overhead; this remains an unverified guest-boot experiment.

## 27 October repair and current source checkpoint

The default is now the generated exitfix JS variant, ROM firmware and scoped
DOM-to-QMP input. The pinned upstream Wasm remains unchanged except for the
previously documented shared-memory import limit. prepare-exitfix-runtime.mjs
reproduces the dynamic-TB nonlocal-exit propagation repair and emits EXITFIX.json.
qmp-input.js bypasses the observed SDL browser-input deadlock without changing
SDL source. The adjacent preview README records actual boot/input acceptance.

The repository bootstrap fetches hash-pinned runtime binaries and compatibility
firmware inputs. It does not populate the complete historical archived build
recipe/source bundle referred to above; those remain in the original local
experiment, and the pinned upstream source links remain the build references.
Public distribution still requires a complete corresponding-source/license
review. This checkpoint is a local preview, not a publication package.
