# Application regressions — 2026-10-18

## Native GL user review: stale window surfaces and Quake crash

User review of the private Quake image exposed two failures that the shorter
GL3 demo smoke did not cover. After a Quake crash, Chromium displayed Quake's
last corrupted image, including after resizing. Closing SuperTuxKart and
opening App Gallery reproduced the stale-surface behavior. The serial log
shows the replacement application reusing the closed GL window's slot.
`CreateWindowLocked` and `DestroyWindowLocked` did not clear `externalGres`
or its dimensions, so composition continued sampling the previous client's
GPU resource instead of the replacement application's CPU framebuffer.

The candidate clears that state at creation/destruction and on CPU framebuffer
publication, including a live dmabuf-to-SHM transition. A host test executes the
production window functions for slot reuse, destruction, transitions and owner
checks. It passes on the candidate and fails on the unmodified implementation.
Release kernel `246e2d02691ee8001937d65b50173afee51f4015fcb01dd3536d1b7cb8abe5c8`
then passed an accelerated eight-vCPU, 4-GiB run: Quake's 720p GL3 demo rendered,
closed normally, and Gallery reused the exact same slot with its own content.
Gallery close/reopen reused the slot again and displayed correctly. All three
UI launch lineages drained. Checked shutdown, owned launcher exit zero,
independent QEMU death, three offline filesystem checks, and all nine saved
fixture hashes/ownership checks passed. This is a short lifecycle check;
SuperTuxKart teardown and the longer Quake failure remain separate tests.
Local evidence: `build/workloads/window-surface-20261018/candidate/acceptance-audit.json`.

Quake also showed malformed geometry and crashed at the same null-pointer read
in Mesa `dri2_query_image+0xd0`, reached through Wayland EGL buffer creation and
SDL. One launch reported host `GL_INVALID_FRAMEBUFFER_OPERATION` and an illegal
`DRAW_VBO` command. The window fix does not establish the cause of those errors.
GPU resource creation has a cumulative per-slot ID bound; GEM close drops its
bookkeeping without releasing the host resource or guest backing. Allocation
failure diagnostics are included to distinguish these paths in a controlled
reproduction. Neither resource exhaustion nor a particular lifetime failure
has yet been proved as the crash's cause.

Local failure evidence: `build/workloads/quake-openttd-crash-20261018/`.
The user-controlled sessions are diagnostic runs, not checked acceptance seeds.

The earlier acceptance checks below use isolated eight-vCPU, 4-GiB
software-Wayland VMs. Earlier runs use
RAM-backed host disks; the access-inquiry and browser-permission follow-ups use
host-backed clones. Applications launch through the installed catalogue as Brook, UID 1000.
These are correctness observations, not matched startup benchmarks.

## GIMP and document persistence

GIMP opened the transparent Brook B PNG, accepted a native brush edit, saved XCF,
exported PNG, and reopened the XCF. An independent PNG decoder found 2,608 newly
nontransparent pixels relative to the source. The PNG signature, dimensions and
all chunk CRCs also passed. Normal close drained the complete UI launch lineage.
A cold boot preserved both saved files' hashes and ownership. The later
full-width ownership kernel also reopened the saved XCF through GIMP's recent
images, displaying the transparent B and the painted mark. Normal close drained
the complete UI launch lineage; the saved XCF/PNG hashes and Brook ownership
were unchanged afterward. That run passed checked sync/stop, QEMU/launcher exit
zero, serial kernel/device/user-fault audits and all three offline filesystem
checks. Its kernel is
`dd601109c01b5f782263491e84883b75d3229abafee971a63dd7bed1f7bf84b5`,
with local evidence under `build/workloads/identity-uid32-20261018/final/`.
This is a cold file-reopen check, not a startup performance comparison.

The [shared path-walk checkpoint](identity.md#shared-component-walk-checkpoint-18-october)
repeated normal catalogue launch, visible XCF reopen and full native-close cleanup
on kernel `424e461dd8d5e1855c4e1268904be43a69fe3714f0bd3b43b63e761cf2d38371`
with module ABI 17. Saved document hashes and independent offline owner checks
passed, followed by checked shutdown and three clean filesystem checks. Its
local evidence is `build/workloads/path-walk-20261018/review/gimp/`.

The [access inquiry checkpoint](identity.md#access-inquiry-checkpoint-18-october)
on kernel `50fe3e571183c246225af8cbe724133394f8bdd3ce71a5afd7dbcdf997be368e`
(ABI 18) passed normal launch and full native-close cleanup. The visible Welcome
window retained the recent-document list; the XCF was not reopened in this run.
After checked shutdown, independent disk inspection verified all four saved
document hashes and Brook ownership, and all three filesystem checks passed.
Local evidence: `build/workloads/access-checks-20261018/acceptance-audit.json`.

The subsequent [permission-kernel browser refresh](browser-trio.md#permission-kernel-refresh)
renewed actual XCF reopen on that same ABI-18 kernel, with Firefox, Chromium and
Codium still open. The saved painted mark was visible; native close drained
GIMP's full lineage. The original ODT also reopened visibly in Writer after
GIMP exited. Both documents retained their hashes and 1000:100 ownership after
the final full-lineage close, checked shutdown, serial and filesystem checks.
Local evidence: `build/workloads/browser-permissions-20261018/cold/`.

![GIMP reopening the saved XCF after cold boot](images/brook_gimp_permissions_1080.png)

Native 1920×1080 capture. The black mark in the upper-right transparent margin
is the saved brush edit used to distinguish this file from its source.

LibreOffice Writer accepted native text entry and saved an ODT. Independent ZIP,
mimetype and XML checks verified the exact three paragraphs. The ODT retained
its hash and Brook ownership across cold boot and reopened visibly in Writer.

![Writer reopening the saved regression document](images/brook_writer_permissions_1080.png)

Native 1920×1080 capture, with no resizing or post-processing.

An attempted PNG transfer through tracked command stdout exceeded the 64-KiB
retention window. That receipt is rejected as complete output evidence, despite
successful process exit. A separate one-file HTTP transfer with exact size/hash
validation supplied the independently decoded artifact. Binary artifact delivery
should use a dedicated file path rather than the bounded text command channel.

## LibreOffice logging and session-service lifetime

The previous global `SAL_LOG=+INFO.sfx.appl +ERR` is invalid: selectors must be
concatenated and `ERR` is not a logging level. LibreOffice falls back to INFO and
WARN for an invalid selector. Removing the override restores its WARN default.
See the [upstream logging rules](https://docs.libreoffice.org/sal/html/sal_log.html).
A probe against the actual pinned `libuno_sal.so.3` reproduced the invalid,
unset and valid targeted-selector behavior. A five-second idle Writer sample
previously emitted 215 INFO lines; the corrected run emitted zero INFO lines
while warnings remained enabled. Serial byte counts include bridge traffic;
this is not an isolated throughput or startup speedup measurement.

Writer's earlier normal close removed its windows and application workers but
left its automatically spawned `dbus-daemon` in the UI launch lineage. That
run failed the full exit barrier. Desktop startup now starts one `brook-dbus`
as UID 1000/GID 100, outside application launch lineages, and supplies a shared
session address. The pinned package includes the upstream session policy;
startup explicitly selects it because Brook does not provide `/etc/dbus-1`.
The initial package omitted that configuration and failed startup; its serial
log is preserved rather than counted as acceptance.

After the configuration correction, a real libdbus client connected and obtained
a unique bus name and server identity. A direct Unix socket peer-credential
check identified UID 1000. `dbus_connection_get_unix_user` did not expose peer
credentials on the client connection; its initial assertion was rejected and
replaced with the direct socket check, not counted as a successful probe.
Writer then reopened its document and closed with its complete launch lineage
empty, while exactly one launch-zero Brook bus remained. A subsequent cold boot
started the service automatically and repeated the bus, saved-file and Writer
reopen/full-exit checks. Both corrected runs passed checked sync/stop,
authoritative QEMU exit, launcher exit zero, serial panic/device/user-fault
audits and read-only offline FAT and both ext2 filesystem checks.

This is a single desktop session service, not login/accounts or per-app
isolation. D-Bus currently warns about the root-owned `/tmp` runtime directory
and socket chmod. Transient service-directory activation, service recovery,
legacy standalone script launch scopes, and private per-user runtime directories
remain unverified. The global bus address does not establish those capabilities.

## VLC launcher and current-package regression

The installed desktop entry initially launched raw `vlc`, bypassing the
installed `brook-vlc` wrapper. It tried PulseAudio, printed a mutex error and
exited without a window. Both desktop collectors now redirect VLC only when
its real executable and executable compatibility wrapper are present. Bare, absolute and quoted command
paths retain the upstream arguments. This also corrects absolute-path browser
and editor entries in the guest collector. Two host collector tests and all
18 installer tests pass, including real-filesystem missing/disabled-wrapper
and removed-application cases.

The temporary first-window helper once misattributed Writer's new window to a
still-observed failed VLC request. That observation is rejected; explicit
launch-token matching now guards the helper. Writer acceptance uses its actual
title, launch group and independent reopen/exit checks.

The first wrapper run selected the stale cached `vlc-brook-close` package,
store `hlj5f0vyqgcgc0d3r8shn68hijvmjslv-vlc-brook-3.0.23-2`. It predates the
accepted surface-handoff fix and reproduced an invalid `get_viewport` argument.
Its launch group was reclaimed and audio became inactive. This is not evidence
of a regression in the current VLC source. Building the current expression
confirmed output `3x6wrd699l6kmnr9gr5jkq8xlvsh4yi4-vlc-brook-3.0.23-2`.
Guest installation of that exact output under its existing `vlc-brook-window`
cache label verified the active profile target before playback. Existing-name
installation retains its registered store hash; package upgrade acceptance is
separate from this test.

Current-source VLC launched normally as Brook, opened the 1280×720 H.264/AAC
Big Buck Bunny fixture and advanced through distinct video scenes. A six-second
host PCM capture contained 264,980 stereo frames at 44.1 kHz and nonzero audio.
Pause produced zero frames; resume produced 88,364 frames with nonzero samples.
Normal native close drained the complete UI launch lineage and audio became
inactive with zero frames. A separate normal launch played the fixture before
SIGKILL; its entire launch lineage and windows then disappeared and audio again
became inactive. These establish output and cleanup, not smooth playback,
frame rate or A/V sync. Maximize/fullscreen geometry remains unaccepted.
The VM subsequently passed checked sync/stop, authoritative QEMU exit, launcher
exit zero, serial kernel/device/user-fault audit and all three offline filesystem
checks. The earlier stale-package failure remains recorded; this is not a
fresh-image install or package-upgrade acceptance run.

![Current-source VLC playing the 720p fixture](images/brook_vlc_720p_1080.png)

An initial installer check overflowed the bounded stdout channel and then
failed on an unavailable `grep`. Its receipt is not accepted as complete output.
Subsequent installation checks saved verbose output to a guest log and returned
its hash/size, explicit process result and independently checked launcher row.
Staging with Python `shutil.copy2` also exposed unsupported `listxattr` (ENOSYS);
the file contents had already copied, and an independent hash check verified the
executable before it was used. Extended-attribute compatibility remains open.

## OpenTTD follow-up

OpenTTD 15.3 launched normally, generated a 256×256 world and continued advancing
its simulation. Native Save Game wrote an 84,496-byte `OTTX` save as UID 1000
under `/data/home/.local/share/openttd/save/`. Ctrl+A did not replace the generated
filename; the initial exact-name assertion failed. The actual file is
`Unnamed, 1950-02-19brook-regression.sav`, SHA-256
`e8a0614f5863d416d995480304e0773aa0de13806c1853169488568810f790ae`.
An independent check verified those bytes and ownership. A subsequent cold boot
checked the same hash and owner before launching the game; the normal UI then
decoded its metadata and loaded the saved world. Meaningful transport-building
gameplay remains open.

The first mouse click on Exit confirmation left it open. A remaining-task
snapshot initially looked like the reported hang, but settled pixels and the
advancing simulation disproved that inference: the main thread was sleeping
normally between frames. The paused kernel-stack snapshot is diagnostic only,
not proof of a deadlock. A distinct click after a one-second pointer settle
confirmed Exit, and the full 29-task UI launch lineage drained normally. The
cause of the missed interaction is not established. This single normal exit
does not establish that the user's intermittent quit hang is eliminated.

The first run's inactive audio sample did not establish cleanup of playing
music. The cold-boot follow-up captured 220,520 stereo frames at 44.1 kHz with
nonzero samples immediately after normal launch. After loading the world,
Shift+F12 opened the in-game jukebox; native Play produced 132,590 frames with
263,225 nonzero samples over a three-second capture. F11 opens the company league
table in a game: the `music = F11` setting belongs to the scenario editor, not
the in-game toolbar.

Normal quit through its confirmation dialog drained the complete 29-task UI
launch lineage, and a subsequent three-second audio capture was inactive with
zero frames. A separate normal launch produced nonzero music before SIGKILL.
Its full launch lineage and window were reclaimed. The initial assertion that
its first post-exit capture must contain zero frames failed: that three-second
capture held a finite 4,422-frame (0.1003-second) PCM tail. A later three-second
capture was inactive with zero frames and no remaining application tasks.
There was no repeating stale buffer. Capture lacks chunk-arrival timestamps,
so this is not an exact output-stop latency or immediate-silencing guarantee.
Longer-session/repeated quit, music smoothness and broader gameplay remain open.

The cold follow-up passed checked sync/stop, authoritative QEMU exit, launcher
exit zero, serial kernel/device/user-fault audit and all three read-only offline
filesystem checks. Browser audio evidence is documented separately in
[browser-trio.md](browser-trio.md).

![OpenTTD loading its saved world after cold boot](images/brook_openttd_saved_1080.png)

The screenshot is an unmodified native 1920×1080 frame; the game is using its
own 800×600 window. It demonstrates reload, not a completed transport network.

Ignored evidence is in `build/workloads/app-regressions-20261018/`, including
`pre-fix-result.json`, `gimp/`, `writer/`, `lo-log-probe.json`,
`session-acceptance/`, `session-cold-acceptance/`, `vlc-launcher/` and
`openttd-cold/`. Kernel SHA-256 for these
session-service tests is
`f14006ec594b239f331d74619d65e554db44b2467c83e2dd08adc7cf807c2a00`.
All 39 hosted suites and the supported compiled entry audit passed before boot.
Both full bootstrap paths were subsequently renewed at `f4066d0`; see the
[matching refresh](bootstrap.md#matching-39-suite-refresh-after-clock-and-desktop-changes-2026-10-18).
Application evidence above retains its own kernel/package identities and scope.

## Frozen-kernel media rerun — 18 October 2026

Eight-vCPU, 4-GiB private disk copies ran kernel
`9e64f2783804714a8998f0bdaf2cbe756980fa0e668b98d07c38292615646ccd`.
The first two runs used an actual native SDL QEMU window with silent host audio;
the final cold check used headless display. App launches came from the installed
catalogue as Brook. Saved documents and profiles were preserved.

| Check | Result |
| --- | --- |
| OpenTTD saved world | Loaded the existing save and advanced the simulation with nonzero music |
| OpenTTD confirmed in-game exit | Native Yes confirmation drained the complete launch lineage; subsequent capture had zero frames and inactive output |
| OpenTTD separate SIGKILL | Nonzero PCM before termination; full lineage and output drained afterward |
| VLC ordinary playback | New recovery package played 720p video/audio through the normal launcher, including after cold boot |
| VLC pause/resume and close | Pause silenced output, resume restored it; normal exit and a separate active-playback SIGKILL drained all lineage tasks and audio |
| VLC long minimize/restore | Audio recovered and normal close passed; the restored video area remained black and is **not accepted** |

The run found and fixed a VLC OSS callback that retried persistent errors and
zero writes forever. The [package checks](../tools/vlc-brook-pkg/README.md)
reproduce six bounded original loops and pass all ten patched cases. The guest
logged three temporary-backpressure errors and replaced its stream before
resuming audio. Brook's existing blocking-write timeout and the original device
discontinuity still need investigation; this is not smooth-playback acceptance.

All three sessions passed heap checks, acknowledged shutdown, original-launcher
exit zero, serial kernel/device/fatal-user-error checks and the three read-only
filesystem checks. All nine saved fixtures retained hashes and ownership.
Two native-window recordings fully decode; see [recording evidence](vm-recording.md).
Local evidence: `build/workloads/media-regression-bed237cb-20261018/`.

## VLC retained-surface fix — 18 October 2026

The user observed black flashes and corruption during native-SDL playback.
Updating to the current unmodified Wayland server reproduced the flashes.
Its trace showed video subsurface buffers continuing to commit while the parent
window also repainted. The server wrote both directly into the same VFB and
retained neither surface's pixels, allowing a parent repaint to erase video.

The candidate server owns snapshots of committed SHM pixels, composes the root
and its subsurfaces in private memory, then exports the complete image to the
kernel VFB. Composition uses premultiplied alpha and converts once to Brook's
straight-alpha representation. Tests against production code cover parent
repaint, client buffer reuse, repeated alpha composition, sibling order,
negative/nested clipping, shrink/move/detach, and role/surface destruction.
Resource teardown handles both explicit requests and client disconnect.

The user reported the flashes fixed with this candidate. A subsequent short
minimize/restore showed real video again, rather than the earlier black area.
The 40-second recording fully decoded 646 H.264 and 1,877 AAC frames. It reported
one estimated audio discontinuity, so this does not establish smooth audio.
Neither the recording counts nor this uncontrolled live comparison establish
a frame-rate improvement. The final shared-VFB copy and resize publication can
still race kernel reads; atomic CPU presentation is separate work.

A sampled-luma scan of the movie rectangle found 28 near-black frames among
374 baseline frames and none among 646 candidate frames. These are different
movie intervals, and the recorder can coalesce upstream updates; this supports
the live observation without proving every displayed/source frame correct.

The user subsequently confirmed the live fix and handed the VM back. Normal
close drained its full launch lineage and audio. A headless cold run verified
the same server hash, ordinary playback, a measured 55.87-second minimize,
restored real video with nonzero PCM, normal close and a separate active-audio
SIGKILL. Both exit paths drained all tasks in the launch lineage and left audio
inactive with zero captured frames.

The native and cold runs passed heap checks, checked shutdown, original launcher
exit zero, serial fault checks and all six read-only filesystem checks. All nine
saved fixtures retained their hashes and ownership. The separate failed boot
before Wayland started is retained as failure evidence and excluded from these
accepted endpoints; see the [backlog](backlog.md).

Local evidence: `build/workloads/boot-recording-7c72adad-20261018/`, including
`acceptance-audit.json`, `retained-retry/` and `retained-cold/`. Sessions:
`0723c067e5684898b96add5c1a382f83` and `7f5bf0855f4c400dbe419e4ba0bcbaeb`.
Package: `bifkmjncizclcyl7wqz7l0vycdrjr1j4-waylandd-brook-0.1-brook`.
