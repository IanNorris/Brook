# Brook chrome concepts and toolkit appearance

For the later Raven-inspired glass feasibility review, three visual directions
and interactive prototype, see the [27 October material study](design/glass/README.md).

Design review, 16 October 2026. These are proposals grounded in the native
renderer and existing [Midnight Settings capture](images/brook_settings_midnight_1080.png).
They are not implemented themes or new guest captures. No VM, recording, or
build was started for this review.

## Recommended direction

**Current** gives Brook a recognizable, restrained visual identity: a short
accent stroke above the focused window's app icon and title, a quiet neutral
perimeter, proportional text, and the same accent under its taskbar icon.
Concentrating color in these small focus indicators leaves application content
and the landscape wallpaper room to breathe.

| Concept | Color tokens | Distinctive treatment | Renderer work |
| --- | --- | --- | --- |
| Current | Surface `#19232B`; raised `#24333D`; text `#E7EEF1`; secondary `#A5B5BF`; accent `#82C9D4`; edge `#40515B` | A 64 by 2 pixel focus stroke over the title's identity area; neutral perimeter; matching taskbar underline | Existing rectangles, icons, and text suffice. Retain outer geometry initially and paint the inner border pixel with the surface color. |
| Instrument | Surface `#151D29`; raised `#202B3B`; text `#E7ECF5`; secondary `#ABB8CD`; accent `#B4A0EA`; edge `#39475C` | A subtle inset group for caption controls; short vertical accent beside the app icon; deliberate technical character | Paint inset control surfaces inside existing caption hit regions. No shaped windows needed. |
| Dawn / Paper | Surface `#F0EDE6`; raised `#FAF8F2`; text `#29343B`; secondary `#5D6A70`; accent `#267C82`; edge `#C8CFCA` | Pale warm panels and a restrained teal focus mark, sharing Current's geometry | A light expression of the same system, using the same primitives. |

Keep Brook, Dawn, and Midnight as the existing persisted theme identities
until a deliberate migration is needed. These concepts explore their visual
language; they do not require adding three more palette IDs.

Shared initial geometry and interaction tokens:

- Retain 36 pixel titlebars, 44 pixel taskbars, and 46 by 36 caption hit targets.
  Use 12 pixel title insets, an 8 pixel icon/text gap, and an 8 pixel spacing
  rhythm. Preserve 20 pixel title icons and 24 pixel launcher/taskbar icons.
- Use 15 pixel regular sans UI text with approximately 20 pixel line height;
  use 13 pixel secondary labels where the rendering pipeline supports it.
  Keep monospace for terminals and optional diagnostics.
- Keep close controls neutral until hover; use the existing red close-hover
  role. Show pressed feedback immediately and activate on release inside the
  pressed target, allowing a drag away to cancel.
- If animation is introduced later, begin with 100--140 ms state fades and
  honor reduced motion. Animated geometry and glass are not prerequisites.
- Give launcher rows 40 pixel height and more panel padding. Add keyboard
  selection, Escape dismissal, and a visible scroll-position cue before
  adding a search field; any visible search must actually work.

## Source grounding and implementation boundaries

The shared colors live in [brook_theme.h](../src/shared/brook_theme.h).
[window.h](../src/kernel/src/window.h) owns the chrome geometry;
[window.cpp](../src/kernel/src/window.cpp) contains `RenderWindowChrome`,
`GetTaskbarLayout`, `WmRenderTaskbar`, and `GetLauncherLayout`.
Those functions already share rendering and hit-test geometry.

The main typography improvement is a **separate UI font atlas**. Native chrome
currently uses the terminal's Hack atlas, described in
[font_atlas.h](../src/kernel/src/font_atlas.h). Terminal layout depends on its
fixed advance, so replacing the global atlas would affect terminal behavior.
Extend [bake_font.py](../scripts/bake_font.py) and pass a separate sans atlas
through UI measurement and rendering. DejaVu Sans is already included by
[brook-fonts](../tools/brook-fonts-pkg/default.nix). Check font size in native
1080p output, rather than judging a reduced README preview.

The compositor has GPU opacity and blur controls, but its current global blur
demo affects entire windows. It is not a ready-made titlebar material. Keep
the initial design equally legible in CPU and GPU composition, with opaque
content and predictable hit regions.

For launcher identity, replace the Applications asset through the existing
`applications.rgba` loading path. Use the established Brook mark, referenced
in [design notes](design/README.md), rather than inventing initials or keeping
the generic four-square launcher glyph. Inspect the mark at 24 pixels before
deciding whether it needs a simplified small-size variant.

## Rough edges, in priority order

1. **Normal close must allow save confirmation and cancellation.** The old
   native-caption path automatically sent SIGTERM after three seconds and
   SIGKILL after five; repeated close clicks immediately killed the process.
   Its high-bit timestamp marker also caused unsigned elapsed-time wrap after
   SIGTERM. The focused compositor change accompanying this note removes
   timer and repeated-click escalation. Each normal WM-API close sends
   `WM_EVT_CLOSE_REQUESTED`; the application owns confirmation and teardown.
   The existing native/legacy close behavior and Super+Q request path remain.
   No new force-close UI was added. A bounded guest WM client has now survived
   two real close clicks and ten seconds after the last request, then exited
   itself (`build/workloads/file-registry-20261016/close-result.json`). Actual
   unsaved-document confirmation/cancellation still needs application testing.
2. **Respect client-side headerbar controls.** `CompositorInputWM` currently
   assumes the top 36 pixels of CSD clients are draggable except for the last
   184 pixels. This can intercept actual headerbar controls. Prefer the
   client's move request. This is a separate follow-up, not part of the close
   fix.
3. **Caption actions should activate on release.** Preserve the pressed
   target, render its state, and cancel when released outside it. Reuse the
   same caption rectangles for rendering and input.
4. **Keep taskbar labels near their icons.** Hover currently replaces the
   clock and diagnostics at the far right. Use a bounded tooltip above the
   hovered button after a short delay; keep the clock position stable.
5. **Improve title text handling.** The renderer processes bytes against an
   ASCII atlas and clips glyphs at the width limit. Add UTF-8 decoding with
   visible fallback, and measured ellipsis for long document titles.
6. **Make the Settings preview represent the selected appearance.**
   [settings.c](../tools/brook-settings-pkg/settings.c) hardcodes a light
   content area inside every theme preview. Add semantic content, control,
   selection, and text roles to the shared palette before generating toolkit
   styles. Reuse these roles in preview drawing.
7. **Make diagnostics an intentional setting.** The GPU and FPS readouts are
   useful for development. Allow a quieter everyday taskbar without tying
   that choice to a particular color palette.

## GTK and Qt appearance bridge

Use one persisted Brook appearance selection, exported to each supported
toolkit. Native decorations and client content have different owners;
successful server-decoration negotiation alone does not theme GTK widgets.
See [desktop settings](desktop-settings.md) for the existing persistence and
decoration behavior.

The source trace below establishes that `/home` and `/data/home` are separate
locations in the experiment layout. Brook Settings uses the latter, while
ordinary launcher and terminal children default to the former. Toolkit
exports need an explicit, verified persistent account location before they
can reliably follow the native appearance.

| Consumer | First useful bridge | Limits and next step |
| --- | --- | --- |
| GTK3 | Package Brook CSS theme variants and write the effective `gtk-3.0/settings.ini` with theme, font, icon theme, and decoration-layout values | Verify newly launched applications first. File writes alone should not be advertised as live updates; applications can override settings. |
| GTK4 | Provide compatible GTK4 theme resources/configuration; verify the packaged GTK version | GTK3 CSS is not a universal GTK4 stylesheet. On Wayland, a Settings portal provides the desktop-wide settings channel. |
| libadwaita | Publish system light/dark preference and accent through supported settings integration | Aim for coherent appearance and accent rather than promising identical custom widget geometry. Applications may explicitly choose their own scheme. |
| Qt Widgets | Use a compatible platform theme to provide Brook palette and font roles while retaining the Wayland display backend | A style name alone does not export the palette. A custom `QPlatformTheme` must match the packaged Qt version. |
| Qt Quick / custom-drawn apps | Verify the actual style and application's response to palette/scheme changes | Do not infer coverage from a Qt Widgets success. Explicit application colors can remain independent. |

GTK documents [GTK3 settings-file fallback and application overrides](https://docs.gtk.org/gtk3/class.Settings.html),
and [GTK4's Wayland portal and settings-file fallback](https://docs.gtk.org/gtk4/class.Settings.html).
For live appearance propagation, verify session D-Bus and a portal service
before implementing `org.freedesktop.portal.Settings` reads and change
notifications. Its standardized appearance values include color scheme
(`1` dark, `2` light, `0` no preference), accent color, contrast, and reduced
motion. [Settings portal specification](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Settings.html)

Libadwaita supports system appearance and accent colors, with application
overrides. [Libadwaita styles and appearance](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1-latest/styles-and-appearance.html)
Qt exposes platform palette/font integration through QPA, whose classes have
no cross-version source or binary compatibility guarantee.
[Qt platform abstraction](https://doc.qt.io/qt-6/qpa.html)
Qt's scheme changes update default palettes but preserve colors explicitly
set by applications. [Qt style hints](https://doc.qt.io/qt-6.10/qstylehints.html)

## Application configuration source trace

This is source evidence, not a fresh inspection of a running guest's mounts.
The paths follow from [kernel.cpp](../src/kernel/src/kernel.cpp),
`FindMount` in [vfs.cpp](../src/kernel/src/vfs.cpp), and the image marker scripts:

| Path | Backing when the normal image markers are used | Source |
| --- | --- | --- |
| `/` | A writable in-memory copy of the FAT initrd, or the embedded FAT fallback | Kernel initrd setup copies the image into RAM and mounts `fatfs` at `/`. |
| `/boot` | Persistent boot image | [create_disk.sh](../scripts/create_disk.sh) writes `BROOK.MNT=/boot`. |
| `/data` | Persistent experiment ext2 image | [create_experiment_disks.py](../scripts/create_experiment_disks.py) writes `BROOK.MNT=/data`; its `/home` directory therefore appears at `/data/home`. |
| `/home` | A dedicated ext2 home volume if attached; otherwise falls back to the root FAT ramdisk | [create_home_disk.sh](../scripts/create_home_disk.sh) writes `/home` in its marker. VFS chooses the longest matching mount prefix. |

Consequently, without the home volume, `/home` does **not** denote the
persistent `/boot` filesystem or alias `/data/home`. Its writes can disappear
at reboot, and FAT has no chmod hook. `VfsChmod`/`VfsChmodPath` return `-95`
when the filesystem lacks that operation. This is consistent with reported
Mousepad chmod failures; it does not prove the cause of every LibreOffice
exception. The data image creator assigns `/data/home` uid 1000, gid 100.

`run_experiment.sh` selects isolated data/nix/boot images but does not itself
disable unrelated default home/GPT images. `run-qemu.sh` optionally attaches
those and prefers an existing GPT image over the home-image selection.
[bootstrap_acceptance.py](../scripts/bootstrap_acceptance.py) explicitly sets
both home and GPT paths to absent files to avoid that ambiguity. Verify the
specific session's mounts rather than infer them solely from the profile name.

Application environment routes also differ:

- [shell.cpp](../src/kernel/src/shell.cpp) supplies `HOME=/home`, writable XDG
  paths under `/home`, `USER=root`, `LOGNAME=root`, `GTK_USE_PORTAL=0`, and
  `GSETTINGS_BACKEND=memory`. Desktop-entry launches call the shell's `run`
  path. [terminal.cpp](../src/kernel/src/terminal.cpp) inherits those same
  defaults before adding terminal-specific values. A shell `--uid` override
  changes process credentials, not these environment strings.
- [experiment-packages.nix](../nix/experiment-packages.nix) defines
  `brook-run`, which sets `GTK_THEME=Adwaita` and
  `XDG_DATA_DIRS=/nix/profile/share`. That forced theme would defeat a Brook
  GTK theme selected through ordinary toolkit settings.
- The Ladybird and VLC wrappers set HOME plus config/cache/data XDG roots to
  `/data/home`. The Office wrapper sets HOME and an explicit LibreOffice
  `UserInstallation=file:///data/home/libreoffice`, but leaves inherited
  XDG paths under `/home`. A generic desktop entry can bypass these wrappers.
- Settings saves a mode-0600 file under `/data/home/.config/brook`; native
  restoration reads that fixed path. Changing the application uid later
  requires checking ownership of files already created by root.

The smallest coherent experiment is a userspace launch wrapper for selected
GTK/Qt applications, preserving the known launch uid and Wayland environment.
After verifying that `/data` is the intended mounted persistent filesystem,
set HOME to `/data/home` and set config/cache/data/state XDG roots beneath it.
Create and check those directories as the intended account. Keep the existing
Wayland runtime socket location while investigating persistence; changing
`XDG_RUNTIME_DIR` at the same time would introduce a separate connection issue.
Exercise create/chmod/rename/save/relaunch/reboot before reusing the wrapper
for all launcher entries or adopting kernel-wide defaults.

For theme acceptance through that wrapper, remove the forced Adwaita override
only in the tested launch route and ensure packaged Brook theme/icon resources
are discoverable. Then generate GTK configuration beneath the same config root
as native Settings. Leave existing application-specific profiles intact until
their migration is intentional. No global environment or script changes were
made by this source review.

## GIMP plugin provisioning mismatch

The reviewed kernel environment globally set
`GIMP3_PLUGINDIR=/nix/share/gimp-lite`. Root has since removed that global
override from `shell.cpp` and reports a passing production build in
`gimp-environment-build.log`. The isolated full-package GIMP test with an
explicit persistent HOME/XDG wrapper is still pending; this correction does
not establish runtime GIMP acceptance. Legacy scenarios that explicitly name
a restricted gimprc remain available.

Only the older
[create_nix_disk.sh](../scripts/create_nix_disk.sh) and
[update_nix_disk.sh](../scripts/update_nix_disk.sh) provisioning paths create
that tree. They copy five file plugins (PNG, JPEG, BMP, TIFF, WebP), write a
restricted `gimprc`, and separately create a batch set with Script-Fu. In the
updater this work is inside `--tools` and the host Nix-tools availability
branch. The generated plugin-cache paths are still under `/home`.

The experiment image creator copies the GIMP package/profile closure but has
no corresponding lite/batch staging. Guest
[nix-install](../tools/nix-install/nix-install.c) has no GIMP-specific staging
hook either. Thus a fully installed GIMP package can coexist with an absent
`/nix/share/gimp-lite` directory. The legacy
[GIMP shortcut](../data/scripts/wayland_gimp.rc) additionally requires that
missing directory's explicit `gimprc`.

GIMP uses `GIMP3_PLUGINDIR` as the default plugin/module base if present;
`gimp_env_get_dir` in the 3.0.8 source returns an absolute override without
testing existence. The default plugin path includes the user's plugin
directory and `${gimp_plug_in_dir}/plug-ins`. A missing override therefore
does not automatically restore the compiled system-plugin directory.
Explicit preferences/gimprc paths can override the search. This distinguishes
a stale environment from an intentional full-plugin scan.
[GIMP plugin directory API](https://developer.gimp.org/api/3.0/libgimp/type_func.PlugIn.directory.html),
[GIMP 3.0.8 environment source](https://raw.githubusercontent.com/GNOME/gimp/GIMP_3_0_8/libgimpbase/gimpenv.c)

For an isolated full-package test, unset `GIMP3_PLUGINDIR` in the GIMP wrapper
and use the package's normal configuration, with persistent HOME/XDG paths.
For a deliberately restricted test, provision the matching lite plugins and
gimprc first, place its plugin cache on `/data`, and declare that subset in
the acceptance result. Do not assume a missing lite directory provides either
working image export or a valid full-plugin baseline. No image provisioning
changes were made by this review; the global-environment correction and build
status above were supplied by the integrating root task.

## Acceptance work

The close fix requires an integrated kernel build and guest checks. No build
or runtime result is claimed here.

1. In a verified editor with native Brook decorations, create unsaved content
   and click the titlebar close button. Leave the save prompt open for at
   least ten seconds. The process and document must remain alive. Cancel,
   edit again, and close again to confirm a new request still works.
2. Repeat close on the same window while its confirmation is pending. It
   must not terminate the process. Save or discard explicitly, then verify
   normal window/process cleanup. Check a multi-window application to ensure
   closing one document does not kill its siblings.
3. Use a minimal WM-API/Wayland client that logs and deliberately ignores
   close events. Close twice, wait ten seconds, and verify it remains alive
   and receives requests. Then end that known test process through the
   existing explicit process-control mechanism. No automatic timeout or
   compositor force-kill should appear in serial logs.
4. Check Super+Q still delivers graceful close, and a native terminal still
   follows its existing close path. Confirm other windows remain interactive
   while a save prompt or deliberately unresponsive client remains open.
5. For appearance, test a GTK3 application, plain GTK4 application,
   libadwaita application, and Qt Widgets application separately. Exercise
   launch, theme change, restart, and reboot, including CSD and SSD clients.
   Verify font, icons, menus, selection contrast, and disabled text in each
   theme; record unsupported live-update behavior accurately.

Further authorized workload/polish work is separate from these proposals:
verify useful GIMP and OpenTTD interactions; investigate boot flicker and
Quake pointer capture; give launcher and fastfetch the real Brook logo; and
adopt zsh/kitty/starship defaults only after those paths work in the guest.
Recording work remains preparation and a rough script. This note does not
authorize or claim production of a final showcase or recording.
