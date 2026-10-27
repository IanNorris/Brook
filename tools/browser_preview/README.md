# Brook browser preview — local, unpublished

The current Brook kernel boots into its real 1024×768 desktop in Chromium using
QEMU-Wasm software emulation. Mouse launcher toggling and the Super+Enter
keyboard hotkey pass through real DOM input. The small image contains the
native desktop, wallpaper and Kernel Console, without Nix applications, guest
networking or audio. Changes are disposable. This is not the full desktop image.

## Reproduce

Requires a built Brook Release, Python 3, Node.js, and the image tools from
Brook's nix-shell. No private game/home/Nix disks are packaged.

```sh
cd tools/browser_preview
python3 scripts/bootstrap.py
# From Brook's nix-shell, with this directory still current:
python3 scripts/prepare-brook-image.py
npm ci
# QA needs a compatible Playwright browser (the web preview itself does not).
npx playwright install chromium
python3 scripts/serve.py
```

Open http://127.0.0.1:38147/ in a desktop Chromium browser and press Start Brook.
The server supplies COOP/COEP headers for shared WebAssembly memory. One guest
CPU uses 384MiB RAM inside a 1GiB shared-memory runtime; browser overhead is
additional. Native QEMU acceleration is not involved. BROOK_ROOT can override
the automatically discovered checkout. BROOK_CHROMIUM_PATH selects a test
browser when Playwright's default executable is not installed.

```sh
node scripts/test-generated-poll.mjs
node scripts/test-monitor-pty.mjs
node scripts/test-exitfix.mjs
node scripts/test-qmp-input.mjs
BROOK_BROWSER_INPUT_CHECK=1 node scripts/check-browser.mjs
```

The checker owns a separate headless browser and saves its exact process IDs,
logs and screenshots under a unique artifacts directory. Input checks have a
20-second bound. Visually verify the open/closed launcher frames; a boot splash
is not desktop evidence. The normal page opens the patched runtime and ROM
profile without diagnostic query parameters. Restart reloads the disposable VM.

## Repairs and evidence

27 October 2026: the old reference image and then a fresh image using kernel
SHA256 05055c4c7d0fe95b4ea6b30006d30a50272c7fb616ee3e3fdad40981f73bb99c
reach the desktop in approximately 28 seconds host time on the development host.
The fresh initrd is rebuilt privately from matching modules and all FAT files
are read back and hashed. Source/artifact manifests record the exact inputs.

An unmodified checksum-pinned Alpine x86-64 ISO reaches Linux/OpenRC/login in
native TCG, but the original browser runtime stalls before its bootloader.
The interpreter fallback restores firmware and pause progress. The decisive
repair is in the publisher's dynamic-TB SJLJ wrapper: it swallowed numeric
nonlocal CPU exits with `_setThrew`/`return 0` inside an execution loop that retained
its current block. Restoring the stack and rethrowing the original signal lets
the actual enclosing Emscripten invoke boundary perform recovery. A focused
test executes the actual generated wrappers, including an Infinity signal.

The existing poll repair is retained: nonblocking fd probes and bounded sleeping
rechecks, with PTY readiness restricted to requested descriptors. The publisher
JS/Wasm remain available unchanged; generated repaired JS has separate filenames
and provenance. No timing multiplier or Brook kernel workaround is involved.

The first SDL DOM mouse event freezes the browser main thread. Scoped window
capture listeners installed before SDL now route guest-only input through
asynchronous QMP. Motion is coalesced; edges remain ordered; held state is
released on blur/cancel and transport backlog is bounded. Outside-page input
remains untouched. This is a tested bypass, not a claimed SDL source fix.

## Controls and limits

`?runtime=publisher&input=sdl&profile=compatible` selects the unmodified JS and
pflash baseline. `?runtime=exitfix&profile=rom` selects the working runtime.
`?monitor=qmp-oob` enables bounded timed diagnostics; normal launches negotiate
QMP for input without periodic diagnostic traffic. Alpine control assets are
optional (`bootstrap.py --alpine-control`); use `?guest=alpine` to compare the
unmodified ISO. The Brook-specific checker does not establish Linux login
acceptance merely from a successful process exit.

Negative diagnostic experiments are retained separately: `--diagnostics`
generates optional `runtime=chainfix` and `tcg=full-nochain` variants. They did
not fix boot. The interpreter diagnostic (`tcg=interpreter`) is exact-binary
hash-gated and changes the compiler-capacity counter, so it also changes
cooperative yielding. None is selected in the working default.

Firefox/Safari, mobile browsers, full applications, persistence, network/audio,
long sessions and public hosting remain unverified. Review complete runtime
source/license redistribution and hosting headers before publishing. The
bootstrap downloads pinned upstream binaries; it is not a source-rebuilt QEMU.
The older Site identity and local browser profiles are deliberately not included.
