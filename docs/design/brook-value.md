# What is Brook for?

Status: critique and proposal, written 16 November 2026 at Ian's request. It
scrutinises [Brook beyond compatibility](brook-directions.md) and takes
positions where that document asks questions. Nothing here is implemented or
decided. It does not change the active goal in
[HANDOVER.md](../agent/HANDOVER.md).

Reading time is about 25 minutes. The recommendation is in the last section if
you want to start there.

## Summary

- **Brook today is a learning and research platform, and a very good one.** It
  is not yet a reason to stop using Linux, and on security it is currently
  behind Linux, not ahead of it.
- **Almost everything Ian dislikes about Linux lives in userspace and in
  governance, not in the kernel.** Android and ChromeOS fixed most of it on a
  Linux kernel. So "we have our own kernel" is not, by itself, the value.
- **What Brook has that Linux cannot have is one owner of every layer, and
  control of the kernel side of the Linux ABI.** Brook decides what world each
  unmodified Linux program sees. On Linux, confinement is something each app
  opts into; on Brook it can be the only thing that exists.
- **The thesis I recommend:** Brook is a single-owner personal computer where
  every app, including unmodified Linux and Windows apps, runs inside its own
  confined world built by the OS, and where the whole machine is observable and
  operable by its owner's agents. The Linux ABI is a guest personality, not
  Brook's system model.
- **The test of the thesis is narrow and can be run in about three months:**
  can ordinary Linux apps run confined, with no access to the owner's files
  except through an OS-owned picker, without breaking and without annoying Ian?
  If yes, Brook has something Linux does not offer by default. If no, Brook is
  a learning project, which is a legitimate thing to be.

## What I read and checked

Brook files: `README.md`, `docs/design/brook-directions.md`,
`docs/agent/HANDOVER.md` (top), `docs/agent/DECISIONS.md`,
`docs/agent/WORKING_STYLE.md`, `docs/agent/DEBUG_BRIDGE.md`,
`docs/compositor-direction.md`, `docs/vm-bridge-next.md` (headings and the
accessibility proposal), `docs/identity.md`, `docs/chromium-sandbox.md`,
`docs/exec-permissions.md`, `docs/roadmap.md`, `docs/package-pipeline.md`, and
the launcher and chrome-architecture sections of
`docs/design/clearwater-next/DESIGN.md`. I did not read the kernel source
beyond a line count, and I did not operate the Dell or a VM.

Web checks, all on 16 November 2026 and all from secondary sources:

- Waydroid's requirements: binder in the kernel, a Wayland session, LXC (Arch
  wiki).
- Windows Subsystem for Android: support ended 5 March 2025.
- ChromeOS and Android: Google confirmed the merger in 2025, with Android as
  the base. I could not confirm launch details.
- Fuchsia: still releasing (F30 in April 2026), still confined to Nest Hubs
  plus the Starnix Linux-ABI work.
- Flatpak: broad `filesystem=home`/`host` grants remain common. I found no
  reliable current percentage.
- Anti-cheat on Linux: Easy Anti-Cheat and BattlEye are user-mode and opt-in
  per game; Riot Vanguard has no Linux path.

Everything else about other systems is from my own knowledge and is not
re-verified. Where I am guessing, I say "speculation".

## 1. Scrutiny of brook-directions.md

### What is strong

- **It separates four identities:** the owner, an installed app, a running
  instance, and delegates such as helpers and agents. This is the right
  decomposition, and most desktop systems never made it.
- **It refuses to tie identity to PIDs or UIDs.** Brook already does this for
  windows ("non-reused creation identity" in `DECISIONS.md`, Frame freshness).
- **It keeps two seams apart:** the Linux syscall ABI and the desktop
  integration contract. That separation is what makes the thesis in this
  document possible.
- **It says an adapter must not get unrestricted authority just to make path
  translation convenient.** That one sentence is the hardest discipline in the
  whole programme.
- **It says agents receive delegated authority and do not become the owner,**
  and that the bridge's debug privileges are not the app permission model.
- **It demands evidence** and repeatedly warns against claiming a benefit
  before measuring it.

### What is vague

- **There is no thesis.** The opening question ("what could a personal
  computer become...") is a theme. Nothing says who Brook is for or why they
  would pick it over Fedora, ChromeOS or a Mac.
- **Every section says "explore" or "investigate".** Eight sections, no
  decisions, no priority order, no criteria for dropping an idea. As written,
  it authorises thinking forever.
- **"Total app memory isolation" is undefined,** as the document itself
  admits. Separate address spaces already exist. The real gaps are elsewhere:
  same-UID signals and ptrace, a shared home directory, open window capture
  (`DECISIONS.md`, Window capture permission), input injection and the GPU.
- **"A common desktop contract" has no content.** It lists the things to
  unify but not who owns each one or what an app must do.
- **"Native app" has no answer to the three questions developers ask:** which
  toolkit, which language, how do I ship it.

### What is missing

- **A threat model.** It is deferred to a deliverable. Without it, the
  isolation sections cannot be evaluated.
- **The compatibility tax.** Every real app on Brook is a Linux app
  (`README.md`, Confirmed Working Software). The document never asks which
  native advantages reach those apps. Section 2 below does.
- **The web.** The table of ecosystems lists Native, Linux, Windows and
  Android. The largest app ecosystem on any desktop is the browser, and it is
  absent.
- **Updates, rollback, provenance and backup.** Brook already has a Nix store
  and numbered Dell generations with a rollback pair (`HANDOVER.md`). The
  directions document does not build on them.
- **The kernel as trusted base.** A capability model is only as good as the
  kernel enforcing it. Brook's window manager and software compositor are
  kernel threads (`compositor-direction.md`), and the kernel is unaudited C++
  serving a very large syscall surface to hostile-by-assumption programs.
- **The current security position.** Chromium and VSCodium run with
  `--no-sandbox` because Brook lacks seccomp and PID and network namespaces
  (`chromium-sandbox.md`). Root and `brook` share a home (`identity.md`). The
  most exposed program on the machine is less contained on Brook than on
  Linux.
- **Hardware.** Brook supports one laptop. Nothing says whether that is the
  plan or a stage.
- **Anything for people from phones.** Ian's fourth question has one paragraph
  ("Revisit desktop conventions"), which again proposes exploring.
- **What Brook will not do.**

### What is in tension

- **Anti-cheat against owner control.** The anti-cheat section assumes the
  owner cannot load kernel code into the attested environment. The rest of the
  document is built on one owner with full control, a developer mode, and a
  debug bridge with machine-wide reach. Both can exist only as two separate
  boot states, and then the "protected" state is a console, not a personal
  computer.
- **"Preserve a conventional desktop option while testing alternatives"
  against "one coherent environment".** Keeping the old option beside the new
  one is how Linux ended up with five of everything.
- **Semantic actions against the app catalogue.** The agent section depends on
  native apps exposing actions. The native apps are a handful of small system
  tools (file browser, calculator, terminal, settings, Gallery). For years, any agent value must come through adapters to Linux
  apps, or it will not exist.
- **Per-app UID against "match Linux, never deviate"** (`DECISIONS.md`, Shared
  open-file descriptions). These are compatible only under a rule the document
  does not state: be exact about Linux *semantics*, and be free about the
  *world* an app is shown. I propose adopting that rule explicitly.
- **Proportion.** "Services activated by use" is the longest and most detailed
  section and the least differentiating. It is systemd socket activation, as
  the section says. Detail went where the problem was familiar.

### What is fixable within Linux, and so is not a reason for Brook

| Directions item | Already available on a Linux kernel |
| --- | --- |
| Per-app identity, private app homes | Android (a UID per app since 2008), Flatpak |
| Explicit document handoff | xdg-desktop-portal file chooser and document portal |
| Process-level memory isolation | Yama ptrace scope, PID namespaces, seccomp |
| Services activated by use | systemd socket and D-Bus activation |
| Batched, asynchronous I/O | io_uring |
| Semantic UI for accessibility and automation | AT-SPI (patchy, but it exists) |
| Immutable system, atomic update, rollback | ChromeOS, Fedora Silverblue, NixOS, SteamOS |
| Attested boot | Secure Boot, TPM measurement, dm-verity (ChromeOS, Android) |

Nearly every mechanism in the directions document exists on Linux. What does
not exist on the Linux desktop is the mechanism being *mandatory and uniform*.
That distinction is the whole argument of this document.

## 2. The value question

### What Brook is today

Brook is about 19 months and 3,100 commits old. It has its own kernel, boot
chain, drivers for one Broadwell laptop including a GPU driver, and enough
Linux ABI to run Firefox, Chromium, LibreOffice, Steam and Proton unmodified.
One person and a series of agents built it. That is a remarkable result and
it demonstrates something real: with agents and a semantic debug bridge, a
single owner can now maintain a full-stack OS.

As a thing to *use*, measured against the Linux it imitates:

| Dimension | Brook against Linux on the same Dell |
| --- | --- |
| Hardware | One machine. Wi-Fi is recorded as out of scope (`docs/xps9343-bringup.md`); I found no suspend/resume or battery-management work in the docs I read. |
| Performance | Slower. About 52 per second in the matched Quake demo against a goal of 60 displayed frames (`HANDOVER.md`); one Stardew startup measured at 128 s after a fix (`README.md`). |
| App coverage | A subset, each app needing bring-up work. Recent failures are "mostly Linux-ABI gaps" (`HANDOVER.md` census). |
| Security | Behind. No browser sandbox, shared home, kernel-resident window manager, open window capture. |
| Coherence | Ahead in principle (one desktop), not yet in practice (see the launch recipes in `HANDOVER.md` that set `LD_LIBRARY_PATH` and `ALSOFT_CONF` by hand). |
| Agent operability | Ahead. Nothing on Linux matches the bridge's durable job, window and launch identities. |

So today nobody should choose Brook over Linux to get work done, and that
includes Ian. That is not a criticism. It is the baseline the rest of this
document works from.

### Who would choose Brook, and why

In descending order of how confident I am:

1. **Ian, and people like him,** for the pleasure and understanding of owning
   the whole machine. This value is real and already delivered.
2. **People studying agent-operated computers.** An OS where one team can
   change any layer, from the scheduler to the launcher, to make it more
   legible to an agent is rare. Brook is already that.
3. **A single-owner appliance user:** someone who wants a laptop that runs a
   browser, an office suite and Steam, cannot be wrecked by what they install,
   and never asks them to administer it. This user exists in large numbers.
   Today they buy a Chromebook, an iPad or a Steam Deck. Brook could serve them
   only on hardware Brook fully supports. This is the only route to "worth
   using daily", and it is a long one.

A fourth audience, Linux enthusiasts who want a better Linux, will not come.
They value exactly the choice and openness that Brook would have to remove.

### What would have to be true for daily use

All four, not any one:

1. **One supported machine works completely:** suspend and resume, Wi-Fi,
   battery, external display, camera, Bluetooth. Each is a driver project. The
   hardware treadmill, not the kernel design, is what has killed every
   alternative desktop OS.
2. **The browser is at parity,** including its sandbox, hardware video and
   stability. For most people the browser is the computer.
3. **It does not lose data,** ever, including across updates and panics.
4. **It offers one thing Linux does not,** that the owner notices weekly.

Brook is working on 1 and 2 by accretion. It has good discipline on 3. It has
not yet started on 4, which is what this document is about.

### The compatibility tax

Brook's apps are Linux apps. A native advantage is worth something only if it
reaches them. Sorting the candidate advantages by whether it does:

**Survives, because it is enforced below or around the app:**

- **Confinement.** The app cannot tell, and need not cooperate, when the
  filesystem it sees is a private one.
- **Provenance and rollback.** The Nix store is immutable and
  content-addressed. An app cannot modify itself or another app.
- **System-owned window management, chrome, launcher and notifications.**
  Mostly. Client-side decorations leak through, as the Chromium tab-strip
  decisions in `DECISIONS.md` show.
- **One audio and graphics path,** as long as Brook presents each app with the
  one protocol endpoint it expects.
- **Lifecycle control.** The OS can freeze or stop a whole app.
- **Observability.** The bridge sees every app's jobs, windows and launches.

**Survives only partly, through adapters:**

- **Semantic actions.** Available only where the toolkit publishes an
  accessibility tree. GTK and Qt do. Electron and Chromium need it switched
  on. Games, Wine apps and custom-drawn UIs expose nothing. The bridge
  specification already proposes this route and notes it is unverified
  (`vm-bridge-next.md`, Proposed toolkit accessibility extension).
- **Document handoff without ambient access.** Works for apps that use the
  portal file chooser when told they are sandboxed. Others need a fallback.

**Does not survive:**

- **A consistent look and feel.** GTK, Qt, Electron and Wine draw themselves.
- **A document or task model.** Linux apps think in paths and processes.
- **Any performance gain from a better native API.** Linux apps call the Linux
  API, and Brook's implementation of it is slower than Linux's.
- **Completeness.** Linux adds interfaces every release. Microsoft tried
  syscall-level emulation in WSL1 and replaced it with a real Linux kernel in
  a VM for WSL2, largely because of the long tail. Brook has already gone
  further on graphical apps than WSL1 did, but the tail is the same tail.

There is a second, less obvious tax. Every Linux interface Brook implements
imports Linux's design into Brook's kernel: fork, signals, `/proc`, mount
namespaces, and next seccomp. The kernel becomes Linux-shaped whether or not
anyone wants that. A "do-over" native syscall API would sit beside all of it
and be used by a handful of small tools.

The conclusion I draw: **put Brook's originality where it reaches every app,
which is the layer that constructs each app's world and brokers its access to
everything outside it.** Do not put it in a native API that nothing uses.

## 3. Linux's DNA: what cannot be fixed, and what merely has not been

Three categories matter and they are often confused:

- **Governance-locked:** technically fixable, but nobody has the authority to
  choose one answer and retire the others.
- **Compatibility-locked:** the fix exists but cannot become the default
  without breaking the installed base.
- **Technically hard:** would be hard for anyone, including Brook.

| Problem | Category | Notes |
| --- | --- | --- |
| Ambient authority: any program run by a user can do anything that user can | Compatibility-locked | The UID model protects users from each other, on a machine with one user. Opt-in fixes exist (Flatpak, Landlock). The default cannot change because every existing program assumes `$HOME`. |
| The home directory as shared mutable state for every app | Compatibility-locked | Dotfiles, `~/.ssh`, browser profiles and documents share one permission. Ransomware and credential stealers need nothing more. |
| Five of everything: toolkits, desktops, packaging, formerly audio and display | Governance-locked | No one owns "the Linux desktop". The kernel has a rule, "don't break userspace". Userspace has no equivalent authority. |
| Distribution as the product | Governance-locked | App developers target distributions, and maintainers sit between developer and user. Flatpak, Snap, AppImage and Nix each fix it, which is four more answers. |
| No stable in-kernel driver ABI | Governance-locked, deliberately | Policy since the early 2000s. It keeps drivers in-tree and high quality, and makes out-of-tree drivers and long-lived devices painful (Android's vendor kernels). |
| Unstable userspace library ABI | Governance-locked | The stable part of Linux is the syscall ABI. Above it, glibc versions, sonames and toolkit majors churn. Hence the Steam Runtime, Flatpak runtimes, and the half-joke that Win32 through Wine is Linux's most stable app ABI. |
| fork and exec | Compatibility-locked | fork forces copy-on-write, memory overcommit, and implicit inheritance of everything. `posix_spawn` and `clone3` help. fork cannot be removed. (Baumann et al., "A fork() in the road", HotOS 2019.) |
| Global namespaces: PIDs, ports, `/tmp`, abstract sockets, the session bus | Compatibility-locked | Namespaces were retrofitted one kind at a time and power containers well. The default for a desktop process is still global. |
| Signals | Compatibility-locked | Asynchronous interruption with process-wide handlers, `EINTR`, and a tiny set of safe functions. `signalfd` and `pidfd` mitigate. Brook has already paid for this in its own kernel (`docs/sigcont-wakeup.md`, `docs/signal-wait-restart.md`). |
| The filesystem as IPC and permission substrate | Compatibility-locked | `/proc`, `/sys`, device nodes, sockets and lock files named by path; permission is path plus mode bits; text parsing is the ABI. Brook's census shows apps demanding exactly these (`/sys/class/net`, `/proc/<pid>/cgroup`, `/dev/input`, netlink uevents). |
| `ioctl` as an untyped escape hatch | Compatibility-locked | Every driver invents a private binary protocol. |
| All-or-nothing root, setuid, coarse capabilities | Compatibility-locked | `CAP_SYS_ADMIN` is root by another name. |
| X11: any client can read all input and all windows | Being fixed, slowly | Wayland fixes it. The transition has taken over fifteen years because it is a protocol with several independent compositor implementations and no owner. A governance cost, again. |
| Audio | Mostly fixed | OSS, ALSA, PulseAudio, JACK and then PipeWire, which finally converged them. Linux does converge. It takes a decade and the old APIs never leave. |
| Memory pressure makes the desktop unresponsive | Technically hard, partly fixed | A consequence of overcommit. systemd-oomd and PSI help. |
| A huge monolithic kernel in C, all in ring 0 | Technically hard | Rust is entering slowly. A rewrite is not on offer to anyone. |

Three observations follow.

**First, the kernel is mostly innocent.** Android gives each app its own UID,
its own private directory, mediated IPC, an OS-owned lifecycle and one
toolkit, all on a Linux kernel. ChromeOS adds verified boot, an immutable
system and silent atomic updates on a Linux kernel. SteamOS is one coherent
Linux desktop with one owner. Each solved the governance problem by having an
owner who discarded the conventional userland. None needed a new kernel.

**Second, the retrofits never become the default.** This is the part that is
truly in the DNA. Flatpak's sandbox is declared by the app's own packager, and
many popular apps declare access to the whole home directory. macOS's App
Sandbox is required only in the Mac App Store. Windows's AppContainer model
failed to displace Win32 because Win32 stayed available and more capable. When
the unconfined path remains open on the same system, developers stay on it.

**Third, Linux's lack of a stable driver ABI cuts against Brook, not for it.**
Linux has the drivers. Brook has about ten driver modules and one GPU driver. Brook's kernel is
MIT-licensed, so borrowing GPL drivers through a compatibility shim (as
FreeBSD does for graphics) would be a licensing and engineering project of its
own. Speculation: that is not within reach of this team.

### What others tried

| System | What it tried | What worked | Why it did or did not win |
| --- | --- | --- | --- |
| Android | App as the security principal; intents; OS-owned lifecycle; one toolkit | All of it, at scale | Won. New hardware category, one owner, and apps written fresh for it. Scoped file access still took a decade to retrofit. |
| iOS | The strictest version of the same | Malware is rare; non-experts are safe | Won. The cost is that the owner is not in charge, and general computing arrived late and partially. |
| ChromeOS | Stateless, verified, auto-updating system; the web as the app platform | Near-zero administration | Won in schools. Linux and Android apps were bolted on through a VM and a container. Now being folded into Android. |
| macOS | Retrofit: App Sandbox, per-resource consent prompts, notarisation, a signed system volume | The OS-owned open/save panel grants access invisibly and well | Mixed. Sandboxing is optional outside the store, so the consent prompts carry the load, and users learn to click through them. |
| Fuchsia | A complete do-over: capabilities, components, typed IPC, no ambient authority, plus Starnix to run Linux binaries | The design | Did not win. It shipped on smart displays. A large, well-funded team with the right architecture did not displace Linux, because there was no app-level reason to switch. |
| Genode / Sculpt | A capability-based component OS that its developers use daily | Proves a small team can live on such a system | Tiny adoption. Software must be ported or run in a VM. |
| seL4 | A formally verified microkernel | Verification | It is a kernel, not an OS. It wins in embedded and defence. |
| Plan 9 | Per-process namespaces; every service a file server | Elegance; its ideas were absorbed (`/proc`, UTF-8, namespaces, 9P) | Lost. Unix was good enough and Plan 9 ran no Unix software. |
| Qubes | Isolation by virtual machine, with trusted window borders | Strong compartments that motivated users can operate | Niche. Heavy on hardware and on the user's attention. |
| NixOS | A declarative, immutable, content-addressed system with rollback | The system-state problem is solved | Niche. It does nothing for app confinement or desktop fragmentation, and it is hard to learn. Brook already borrows its store. |
| Flatpak / Silverblue | Immutable base, sandboxed apps, portals | The closest thing to "fix it within Linux" | Partial. Opt-in holes, as above; and the five-of-everything layer is still underneath. |

The pattern: the winners had a single owner, a new channel to users (a new
device category or the web), and something to run. The losers had better
designs and nothing to run, or no reason to switch. Brook has an owner. Its
Linux ABI gives it something to run, as Starnix did for Fuchsia. It has no
channel. Compatibility is necessary for Brook and is not sufficient.

## 4. Positions on Ian's four problems

The unit used throughout is the **compartment**: the confined world one
installed app runs in. A compartment has a stable identity that is not a UID
or PID, a private writable home, a read-only view of the app's own store
closure, private `/tmp` and runtime directories, its own view of processes,
and exactly the service endpoints its manifest declares. The kernel's Linux
personality builds this view. The app sees an ordinary Linux system with one
user and an almost empty home.

### 4.1 One desktop

Position: **Brook owns seven things outright, ships exactly one implementation
of each, and absorbs all variation in per-app adapters.**

What Brook owns:

1. **Windows.** One compositor, one window manager, system-drawn chrome and
   system-owned hit testing (already the Clearwater Next design). No
   alternative desktop environments, ever. Themes are data, as Clearwater's
   theme-engine role already says.
2. **The broker.** One system service for everything that crosses a
   compartment boundary: open and save, share, print, open-a-link,
   notifications, screenshots and screen capture, clipboard, and global
   settings such as scale, dark mode, fonts and accent.
3. **Audio.** One mixer and one policy.
4. **Graphics.** Wayland surfaces and Mesa. Nothing else reaches the screen.
5. **Distribution.** One Gallery, one install unit ("an app"), one update
   mechanism.
6. **Settings.** One place, searchable. Apps do not own system settings.
7. **Identity.** One owner. No login manager, no user accounts in the ordinary
   experience.

The desktop contract is toolkit-independent because it is defined as
protocols, not as a widget library: a Wayland surface, the broker's
interfaces, and a semantic tree. I would not build a Brook widget toolkit.

The important design choice is how the broker is exposed. **Brook should
implement the xdg-desktop-portal D-Bus interfaces as the compatibility face of
its broker.** That API is already a deployed powerbox. GTK, Qt, Electron,
Firefox and Chromium already call it when they believe they are sandboxed. An
app that uses portals becomes nearly native on Brook without modification.
This is the single highest-leverage adapter available. Brook has no portal
implementation today (I searched `docs/`, `src/` and `tools/`).

How fragmentation is kept from leaking back in:

- **Variation lives in the app's manifest, never in system state.** If an app
  wants PulseAudio, its compartment contains a PulseAudio socket backed by the
  one mixer. If it wants ALSA, it gets an ALSA device backed by the same
  mixer. The launch recipes in `HANDOVER.md` (a hand-set `LD_LIBRARY_PATH`, a
  private `pulseaudio -n` started by script) are this idea done by hand. They
  belong in manifests.
- **A fixed adapter list.** Brook serves a named, closed set of compatibility
  protocols: core Wayland with xdg-shell and a short list of extensions, X11
  through rootless Xwayland, the PulseAudio protocol, ALSA, portals, a
  per-compartment session bus, and AT-SPI. Adding one is a decision recorded
  in `DECISIONS.md`. Each new one replaces something or is refused.
- **One Xwayland per compartment.** X11 clients can read each other's input
  and windows. Sharing one X server across apps would reimport X11's security
  model.
- **Nix is the implementation, not the interface.** Closures give each app its
  exact dependencies, which is what the Steam Runtime and Flatpak runtimes
  approximate. The owner never sees a store path, a dependency or a "package".
- **Replace, never add.** When a second way of doing something appears, the
  first is removed in the same change.

What cannot be unified, honestly: the inside of app windows. GTK, Qt and
Electron draw themselves. Brook can make scale, fonts, dark mode and accent
agree through the settings portal, and own the chrome. macOS users live with
the same limit for Electron apps.

### 4.2 "Impossible to get a virus"

**"Impossible" is not achievable, and Brook should not say it.** What is
achievable is narrower and still valuable.

What "virus" means on a desktop today, by how people are actually harmed:

| Threat | Typical form |
| --- | --- |
| Malicious or trojanised app | A fake installer, a cracked game, a "free" utility |
| Supply chain | A legitimate app's update or dependency is compromised |
| Ransomware | Any code running as the user encrypts the home directory |
| Credential and session theft | Any code running as the user reads browser cookies, SSH keys, wallet files |
| Browser exploit | A web page escapes the renderer |
| Phishing and scams | The user is persuaded to type a password or send money |
| Classical virus | Code that spreads by modifying other programs |

The design, in seven parts:

1. **Per-app identity and no ambient filesystem.** Every app runs in a
   compartment. The owner's documents are not in it. Other apps' data is not
   in it. Credential stealers and ransomware find an empty home.
2. **Powerbox document access.** The only way an app reaches a document is
   that the owner picks it in a system-owned picker, drops it on the window,
   or opens it from the system's file view. The act of choosing is the grant.
   No permission dialog is involved.
3. **Immutable, signed code.** App code lives in the read-only store. An app
   cannot modify itself or anything else executable. This makes the classical
   virus structurally impossible, which is the one place the word applies.
4. **Provenance.** Every app is signed. The publisher's key is pinned at first
   install, so an update signed by anyone else is refused. The Gallery shows
   where an app came from. Sideloading is allowed, labelled, and equally
   confined.
5. **Versioned documents.** Any file an app is given write access to is
   versioned by the broker before the app touches it. Ransomware against
   granted documents becomes a "restore previous version" operation.
6. **Reversible system state.** System and apps are generations. Any update
   can be rolled back from the boot menu. Brook's Dell generations and
   recovery loader are most of this already.
7. **Uninstall removes everything.** The compartment is the app's entire
   footprint. Nothing persists outside it. No autostart entries, no services,
   no shell hooks.

Supporting rules: no cross-compartment ptrace, signals or `/proc`; window
capture and input injection go through the broker (which reverses the current
"capture open to all apps" decision once it is cheap); network access is a
declared capability.

What this achieves, stated as a claim Brook could defend: **a program the
owner runs cannot, merely by being malicious, read other apps' data, touch
documents it was not handed, alter the system or other apps, or outlive its
uninstall; and what it damages among the things it was handed can be
restored.** Ordinary malware becomes structurally ineffective. That is the
honest replacement for "impossible to get a virus".

What it does not prevent:

- **Social engineering.** A phishing page in a correctly sandboxed browser
  still collects the password. Nothing in an OS fixes this.
- **The owner granting authority.** If the owner hands a malicious app their
  tax folder, it has the tax folder.
- **A compromised trusted app acting within its grants.** A backdoored update
  from the real publisher's key gets everything that app legitimately holds.
  For a browser, that is every session cookie. Compartments bound the damage
  to one app; for the browser, one app is most of a person's digital life.
- **Browser-internal attacks.** Site isolation is the browser's job. Brook
  must at minimum not weaken it, which today it does.
- **Kernel exploits.** Compartments are only as strong as Brook's kernel,
  which is young, unaudited, written in C++, and exposes hundreds of Linux
  syscalls. A determined attacker would look there first. Until the kernel is
  hardened and the window manager is out of it, Brook's guarantees hold
  against commodity malware and not against a targeted attacker.
- **Firmware and physical attacks.**

Three compatibility costs to plan for:

- **Apps that run code they download.** Steam, and developer tools generally.
  These need a compartment in which downloaded code is executable. Treat Steam
  as a nested platform: everything it installs lives and runs inside Steam's
  compartment. Development gets a "workshop" compartment (section 4.4).
- **Apps that expect to see each other.** Password-manager browser
  integration, editors launching compilers, anything using a shared session
  bus. Each needs a declared, brokered connection.
- **A versioning filesystem.** Brook's data volume is ext2. Versioning at the
  broker (copy before granting write) avoids needing a new filesystem at
  first. Speculation: it will be good enough for documents and too slow for
  large media projects.

### 4.3 Cross-platform

Position: **Linux first, the web second, Windows through Proton third, and
Android not at all for now.**

| Ecosystem | Path | Cost | Priority |
| --- | --- | --- | --- |
| Linux | The existing ABI personality, each app in a compartment, portals for integration | Ongoing ABI gaps; this is already the main cost of the project | 1. It is the substrate for the next two |
| Web | Installed web apps as first-class apps: own launcher entry, own window, own compartment and browser profile | Small. It rides on Chromium or Firefox, which already run | 2. Cheapest large gain |
| Windows | Wine and Proton as Linux programs. One Wine prefix per compartment | Moderate. It already works for several titles (`docs/proton.md`, `docs/wine-unity.md`, `docs/d3d12.md`) | 3. Games are the strongest reason an appliance user needs more than a browser |
| Android | A full Android userland in a container, plus binder in the kernel, a graphics bridge, and ARM translation on x86 | Very large, and the result is poor | Deferred indefinitely |

On Android specifically. Waydroid shows the shape: it needs the binder driver,
a Wayland session, a container, and a multi-gigabyte Android system image.
Brook would need to implement binder, satisfy Android's expectations of
cgroups and SELinux, and bridge Android's graphics stack to Wayland. Then:
many apps ship ARM-only native code and need a translator on x86; apps that
depend on Google Play services do not work without them; and banking and
payment apps check device integrity and refuse. Microsoft built this for
Windows 11 with far more resources and withdrew it in March 2025. The apps
people most want from Android (messaging, banking, media) mostly have web
versions. Revisit only if Brook ever targets ARM hardware.

On anti-cheat, since the directions document raises it under this heading:
drop it. Kernel-level anti-cheat vendors do not support Linux even with the
Steam Deck's volume behind it. An OS with one machine has no leverage, and the
attested mode conflicts with owner control.

What "seamless" should mean. An app from any ecosystem is seamless when all of
the following hold, and none of them requires the app to hold ambient
authority:

| Integration point | Seamless means |
| --- | --- |
| Windows | Each app window is a Brook window with Brook chrome. No desktop-in-a-window (so rootless Xwayland, never Xephyr, for delivery). |
| Launcher | One icon, one name, found by the same search, with launch feedback. |
| Documents | Open and save go through the system picker. Inside a Wine prefix, the chosen file appears at a stable drive path. The app never sees the owner's file tree. |
| Clipboard and drag-and-drop | Work across ecosystems, mediated by the broker, delivered on paste or drop and not readable in the background. |
| Notifications | Appear in the one notification surface, attributed to the app. |
| Audio | One volume control per app in one mixer. |
| Input | The same keyboard layout, input method and pointer settings. |
| Settings | Scale and dark mode are honoured where the toolkit can. |
| Lifecycle | Close means close. The system can stop the whole app, including its helper processes. |

The capability model makes this easier, not harder, in one respect: the broker
is the single place where each ecosystem is adapted, so there are four
adapters to one contract and not six pairwise integrations.

### 4.4 Patterns to eschew, and what to keep

Position: **the default experience is an appliance. The Unix machine still
exists, inside one compartment, for people who ask for it.**

Drop from the default experience:

| Desktop pattern | Replace with |
| --- | --- |
| The filesystem tree as the way to find things | One Documents view with search, recents and per-app recents. Files still exist and can be exported; nobody is shown `/` or a home directory. |
| Installing as a procedure (downloads, installers, dependencies, repositories) | One Gallery. Install is one action. Remove is one action and is complete. |
| Manual updates and "restart to update" prompts | Silent, atomic, reversible updates. The new generation takes effect at next boot. |
| Administrator passwords and `sudo` | One owner. The few dangerous actions need the owner's presence through a system-drawn confirmation that apps cannot imitate or cover. |
| Up-front permission prompts | Authority follows the owner's action: picking a file, pressing a capture button. Standing permissions are few and listed in one place. |
| Modal dialogs | Sheets attached to the window they concern; undo instead of "are you sure". |
| Explicit save, and "unsaved changes" on close | Autosave with versions for native apps. For compatibility apps, the broker's versions make an accidental overwrite recoverable. |
| App-drawn window frames, tray icons, per-app update checkers, per-app autostart | System chrome; one background-activity surface showing what is running and why; none of the rest. |
| Config files and dotfiles | One settings surface. Per-app settings live in the compartment and "reset this app" always works. |
| Invisible background processes | Anything running without a window is listed, attributable and stoppable. The system may suspend idle apps. |
| Logs and error codes as the failure experience | An error says what failed, what state things are in, and what can be done. The existing launch-feedback work (`docs/launch-feedback.md`) is the right instinct. |
| The terminal as a requirement | Never required for any ordinary task. |

Keep, because phones got these wrong or desktops got them right:

- **Real windows and real multitasking,** with a keyboard and a precise
  pointer as first-class input.
- **Files as portable, visible documents in open formats.** The phone model of
  data trapped inside apps is a regression. The picker changes how apps *get*
  files, not whether files exist.
- **Drag-and-drop and clipboard between any two apps.**
- **Information density.** Do not enlarge everything for touch on a laptop.
- **Owner sovereignty.** Sideloading, visible provenance, no account required,
  no vendor able to revoke an app. This is the difference between Brook and an
  iPad, and it is worth stating as a principle.
- **Automation.** On Brook this is the agent and semantic-action surface, not
  shell scripts.
- **A real development environment.** A "workshop" compartment containing a
  conventional Linux userland, a terminal and Nix, with access to the project
  folders the owner grants. ChromeOS's Linux container is the precedent.
  Developers get Unix; nobody else has to meet it.

The Clearwater Next launcher already makes choices in this spirit: one home
per app, no nested folders, an Unsorted inbox and search
(`clearwater-next/DESIGN.md`, Launcher organisation). The directions
document's "preserve a conventional desktop option" should be read as "keep
windows and a taskbar", not "keep a second shell".

## 5. Do-over decisions

Eleven choices I would make differently from Unix if starting today. For each:
why, what it costs Brook given its Linux personality, and the smallest
prototype that would test it. Decisions 1, 2, 7 and 9 are the ones the
recommendation depends on. The others are worth knowing and mostly not worth
building yet.

**1. A process starts with nothing and is handed what it may use.**
*Why:* ambient authority is the root of the malware problem. Plan 9 and
Fuchsia construct a namespace per process; nothing is reachable by default.
*Cost:* none to compatibility if the Linux personality synthesises a normal-
looking filesystem inside the compartment. Apps that expect to share dotfiles
or `/tmp` sockets with other apps break and need declared connections.
*Prototype:* launch GIMP with a private home and no view of `/data/home`. Open
and save one file through a broker-granted path. Count what breaks.

**2. The app, not the user, is the security principal.**
*Why:* a personal computer has one user and a hundred mutually distrustful
programs. *Cost:* UIDs remain as a fiction inside each compartment (always
"uid 1000"). Kernel permission checks gain a compartment check that runs
first. *Prototype:* an unforgeable compartment identity on every process,
inherited across fork and exec, with `kill`, ptrace and `/proc` scoped by it.
Test with a hostile fixture that tries each.

**3. Spawn, not fork; inheritance is explicit.**
*Why:* fork copies an address space to throw it away, forces overcommit, and
leaks every open descriptor by default. *Cost:* fork stays forever in the
Linux personality, so the kernel carries both. The gain is limited to native
system components. *Prototype:* a native `spawn(image, handles[])` used by the
launcher; compare launch latency with fork-and-exec for the same app.

**4. Events on handles, not signals.**
*Why:* signals are asynchronous, process-global and nearly impossible to
handle correctly. *Cost:* signals stay in the Linux personality. *Prototype:*
make "please close" for native apps a message with a deadline and an
acknowledgement, building on `docs/orderly-shutdown.md`. Low priority.

**5. I/O is asynchronous, cancellable and reports completion.**
*Why:* blocking calls with `EINTR` and no cancellation are why UIs freeze and
why "did it finish?" is hard to answer. io_uring is Linux conceding the point.
*Cost:* Linux apps cannot use it. *Prototype:* none yet. The bridge's job
model (durable identity, observable completion) is the pattern; apply it to
broker operations first.

**6. System interfaces are typed, versioned messages, not ioctls and text
files.** *Why:* `/proc` text, `ioctl` numbers and ad-hoc sockets cannot be
checked, versioned or introspected. Fuchsia's FIDL and Android's binder
interfaces can. *Cost:* the Linux-facing `/proc` and `/sys` must still be
synthesised. *Prototype:* define the broker's interface in a schema, and
generate both the portal adapter and the bridge's tool definitions from it.
One contract for apps, accessibility and agents is the directions document's
best idea; this is how to make it literally true.

**7. The system is an immutable, signed image, and state is separated into
four kinds:** system, app code, app private data, and the owner's documents.
*Why:* it makes updates atomic, rollback possible, uninstall complete and
self-modifying code impossible. *Cost:* self-updating apps (Steam, browsers'
own updaters, editor extensions) need a writable, executable area inside their
compartment. *Prototype:* switch a whole generation, including the app
profile, and roll it back from the boot menu on the Dell. Most of the parts
exist.

**8. Every object has an unforgeable identity that is never reused.**
*Why:* PIDs, UIDs, window titles and descriptor numbers are integers that get
recycled, and whole classes of bugs and attacks follow. *Cost:* Linux apps
still see PIDs. *Prototype:* largely done for windows and bridge jobs. Extend
the same identity to processes and compartments in the kernel's job API.

**9. Cross-app access happens through an OS-owned act (the powerbox), not a
permission setting.** *Why:* prompts train people to click "allow". Picking a
file is something the person was going to do anyway. macOS's sandboxed
open panel shows this works. *Cost:* apps that do not use the portal file
chooser need a fallback: an "Import" shelf in the compartment's home that the
owner drops files into. *Prototype:* implement the portal FileChooser and
OpenURI interfaces; run the twenty most-used apps; record which use them
unprompted.

**10. The OS owns app lifecycle.**
*Why:* phones stay responsive because the system may suspend or stop anything
not in use. Desktops degrade because everything runs forever. *Cost:* Linux
apps assume they are immortal; some lose network sessions when frozen.
*Prototype:* freeze a background compartment under memory pressure and thaw it
on focus. Measure against the existing out-of-memory behaviour
(`docs/oom-recovery.md`).

**11. Drivers and the compositor are restartable, isolated components behind a
stable interface.** *Why:* most kernel crashes and most kernel attack surface
are drivers. *Cost:* large, and the GPU driver is the hardest case. I would
not attempt a microkernel conversion. *Prototype:* finish moving the window
manager and compositor out of the kernel, which `compositor-direction.md`
already intends. Then stop, and reconsider with data.

## 6. Recommendation

### Thesis

Brook is a single-owner personal computer on which every program, including
unmodified Linux and Windows software, runs as a guest in a world the OS
builds for it: its own identity, its own private storage, and nothing of the
owner's except what the owner hands over through the system. The system and
its apps are immutable, signed and reversible, so ordinary malware has nothing
to hold on to. Everything that happens on the machine has an identity and an
observable outcome, so the owner's agents can operate it with delegated
authority and the owner can see what they did. Brook does not try to be a
better Linux. It treats Linux as one guest personality, exact in its
semantics and arbitrary in what it is shown, and puts all of its own design
into the layer between apps and everything else.

### The part Ian may disagree with

- **The kernel is not the product.** The user-visible value in this thesis
  could be built as an opinionated Linux distribution, in less time, with
  working hardware. Android and ChromeOS are the proof. The reasons to do it
  on Brook's kernel are that Ian wants to, that owning the ABI makes
  confinement the default at the lowest level with no namespace and seccomp
  layering, and that one team changing any layer is what makes the agent work
  possible. Those are good reasons. They are not the reasons a user would
  give.
- **The do-over mostly should not happen in the kernel.** A clean native
  syscall API is the most attractive work and the least valuable, because
  nothing will call it.
- **More Linux ABI work is required before any of the differentiating work
  counts.** Seccomp and namespaces are needed so that browsers run their own
  sandboxes. That is unglamorous imitation, and it comes first.
- **Android and anti-cheat should be dropped,** not explored.
- **"Impossible to get a virus" should be retired as a phrase.**
- **There is a real chance the thesis fails,** because too many Linux apps
  misbehave when confined. Flatpak's experience says many will need holes.
  Brook's advantage is that it controls the kernel side and can lie more
  convincingly than bubblewrap can. Whether that is enough is an empirical
  question, and it is the first one to answer.

### What to stop doing

- **Open-ended performance work on the Broadwell GPU** once the 60 fps goal in
  `HANDOVER.md` is met. `DECISIONS.md` already records Ian's wish to avoid
  diminishing returns here.
- **App bring-ups as trophies.** Fix a list of about twenty apps that define
  the appliance (two browsers, office, image editor, media player, editor,
  Steam and a handful of games) and decline others until the thesis is tested.
- **Exploring Android, anti-cheat, service activation and alternative desktop
  shells.** The first two are poor bets; the third is solved elsewhere and not
  differentiating; the fourth competes with Clearwater Next.
- **Treating UIDs as a security mechanism.** `identity.md` is honest that they
  are not one. Stop investing in user accounts.
- **Per-app launch recipes held in handover notes and scripts.** Move them
  into manifests.
- **Xephyr as anything but a diagnostic,** as `DECISIONS.md` already says.
- **A second machine or GPU.**

### Next steps

Step 0 is to finish the active 60 fps goal. Nothing below should interrupt it.

1. **Close the gap where Brook is worse than Linux.** Implement seccomp-bpf
   and PID and network namespaces to the extent Chromium's and Electron's
   sandboxes need (`chromium-sandbox.md` has the scope), and remove
   `--no-sandbox` from the launchers.
   *Measure:* Chromium, Firefox and VSCodium start with their own sandboxes
   enabled and pass their existing acceptance checks.

2. **Compartments, version 0.** A kernel compartment identity; a private home,
   `/tmp` and runtime directory per installed app; `/proc`, signals and ptrace
   scoped to the compartment; window capture and input injection denied across
   compartments. The Gallery places every installed app in one.
   *Measure:* a "hostile app" fixture suite, written once and kept as a
   regression test, that tries to read another app's profile, read the owner's
   documents, signal or trace another app, capture another window, and persist
   after uninstall. Target: every attempt fails. And: how many of the twenty
   apps run unmodified in a compartment.

3. **The broker and powerbox, exposed as xdg-desktop-portal.** FileChooser,
   OpenURI, Notification, Settings and Screenshot first, with the Import shelf
   as the fallback for apps that do not use portals. Broker-side versioning of
   any file granted for writing.
   *Measure:* for each of the twenty apps, can the owner open, edit and save a
   document with no filesystem hole in the manifest? Count holes. Then run a
   ransomware fixture against granted files and restore them.

4. **Manifests and one audio endpoint.** Every app's adapters, environment and
   service needs are declared in its manifest. One mixer, with a PulseAudio
   protocol endpoint and an ALSA device per compartment as its only
   compatibility faces. (Speculation: I have not read the audio code, and OSS
   may need to remain for specific games.)
   *Measure:* the number of hand-written launcher scripts and environment
   overrides outside manifests goes to zero for the twenty apps.

5. **Turn the bridge's semantics into a delegated agent interface.** Give an
   agent a compartment-scoped grant instead of machine-wide debug authority,
   and add the AT-SPI ingestion that `vm-bridge-next.md` proposes, for
   compatibility apps.
   *Measure:* a fixed suite of ten everyday tasks (open a document and export
   it, find and play a file, install and remove an app, and so on). Record the
   fraction completed without pixel-level input and without debug authority.

### How to tell whether the thesis holds

Run steps 1 to 3 and then use Brook on the Dell for a defined set of ordinary
tasks, for a fixed period, keeping a log of every time Ian falls back to Linux
and why.

The thesis holds if, at the end:

- the hostile-app suite passes completely;
- at least fifteen of the twenty apps run confined with no filesystem hole;
- document open and save through the picker is no slower or more irritating
  than on Linux, by Ian's own judgement;
- there is at least one recurring task for which Ian prefers Brook.

The thesis fails if more than about half the apps need holes to function, or
if the confinement is correct but Ian turns it off to get things done. In that
case the honest description of Brook is the one in the summary: a learning and
research platform, and a testbed for agent-operated computers. That would
still justify the project. It would not justify describing it as an
alternative to Linux.

The thresholds above (twenty apps, fifteen passing, half) are my proposals to
make the test falsifiable. They are not derived from data and Ian should set
them before starting, not after.
