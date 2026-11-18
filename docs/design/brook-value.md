# What is Brook for?

Status: critique and proposal, written 16 November 2026 at Ian's request and
revised the same day after his response. It scrutinises
[Brook beyond compatibility](brook-directions.md) and takes positions where
that document asks questions. The section "Ian's response and the agreed
direction" records decisions. Everything else is analysis or proposal, and
nothing here is implemented. It does not change the active goal in
[HANDOVER.md](../agent/HANDOVER.md).

Reading time is about 35 minutes. The agreed direction is next, and the plan
is in the last section.

## Summary

- **Brook is a platform for exploring OS concepts, not a production OS, and it
  will probably never be one.** That is Ian's framing and this revision adopts
  it. The critique below still measures Brook against Linux, because an honest
  baseline is what makes an experiment's result mean something.
- **Almost everything Ian dislikes about Linux lives in userspace and in
  governance, not in the kernel.** Android and ChromeOS fixed most of it on a
  Linux kernel. So "we have our own kernel" is not, by itself, the value.
- **What Brook has that Linux cannot have is one owner of every layer, and
  control of the kernel side of the Linux ABI.** Brook decides what world each
  unmodified Linux program sees. On Linux, confinement is something each app
  opts into; on Brook it can be the only thing that exists.
- **The headline concept is compartments:** a desktop OS that runs ordinary
  Linux and Windows software with phone-like separation between apps, without
  asking app developers for anything. UWP asked developers to rewrite.
  Compartments ask nothing of them.
- **Concepts are only worth what their validation is worth,** so performance,
  compatibility and capable hardware come first, and security claims wait for
  a kernel that can carry them.

## Ian's response and the agreed direction

Ian read the first version of this document on 16 November 2026, broadly
agreed it was realistic, and corrected its framing. These are recorded as
decisions. Where the first version recommended otherwise, it has been changed.

| Subject | Decision | What changed in this document |
| --- | --- | --- |
| Purpose | Brook is a way of exploring concepts. It is not, and likely never will be, a production OS. | "Would someone use it daily" is kept as context, not as the goal. The goal is that concepts can be validated on real workloads. |
| Anti-cheat | Keep it, as a thought experiment: how can the OS make kernel-level anti-cheat unnecessary, and offer guarantees it cannot? Supporting a specific title such as Valorant is not the point. | "Drop anti-cheat" is withdrawn. Section 4.5 is new. |
| Hardware | The Dell proved the foundations and is not a long-term platform. Bring Brook up on more powerful hardware that can game. | "No second machine or GPU" is withdrawn. See workstream 2 of the plan and the draft `docs/discrete-gpu-bringup-plan.md`. |
| Games and browsers | They are demos, and they are also the best available tests of syscall and ABI compatibility and stress tests of the whole system. Ian is a games developer and can steer optimisation. | "Stop trophy bring-ups" is withdrawn. Workloads are chosen for the coverage they add. |
| Security timing | Leading with security is the wrong order. A hand-written C++ kernel produced mostly by AI is not a foundation for security claims. A move to Rust may be sensible, after interfaces are formalised and tested: build abstraction interfaces, test every element as far as feasible, convert the tests to Rust, then port the code. | The plan is resequenced. Compartments are built as a concept and an architecture now; guarantees are claimed only as the kernel earns them. |
| xdg-desktop-portal | An obvious short-term goal, preferably forked to work natively with Brook rather than shimmed. Not yet, because performance is not there. | Moved after the performance work; reworked as a native fork. |
| Compartments | The major hook: Linux support with phone-like compartmentalisation, seamless for most apps. | Promoted to the headline. The mechanism is made concrete in the plan. |
| Multiple users | Not excluded; personal tablets support several users. User-level isolation is not enough by itself, and Brook should lean into per-app isolation. | "No user accounts" is withdrawn. Multi-user is a non-goal for now, with the app as the primary boundary and users as a possible outer one. |

Two positions from the first version stand, as risks and not as instructions:
Android is still the most expensive ecosystem for the least return on x86, and
a native syscall API still has almost no callers. Neither blocks anything
above.

## What I read and checked

Brook files: `README.md`, `docs/design/brook-directions.md`,
`docs/agent/HANDOVER.md` (top), `docs/agent/DECISIONS.md`,
`docs/agent/WORKING_STYLE.md`, `docs/agent/DEBUG_BRIDGE.md`,
`docs/compositor-direction.md`, `docs/vm-bridge-next.md` (headings and the
accessibility proposal), `docs/identity.md`, `docs/chromium-sandbox.md`,
`docs/exec-permissions.md`, `docs/roadmap.md`, `docs/package-pipeline.md`, and
the launcher and chrome-architecture sections of
`docs/design/clearwater-next/DESIGN.md`. For the revision I also read
`docs/agent/syscall-split-plan.md`, `docs/agent/REFACTOR_ISSUES.md` and the
untracked draft `docs/discrete-gpu-bringup-plan.md` in the legacy checkout. I did not read the kernel source
beyond a line count, and I did not operate the Dell or a VM.

Web checks, all on 16 November 2026 and all from secondary sources: Waydroid's
requirements (binder, Wayland, a container); the end of Windows Subsystem for
Android on 5 March 2025; the ChromeOS and Android merger (confirmed in 2025,
launch details unconfirmed); Fuchsia (still releasing, still on Nest Hubs, plus
the Starnix Linux-ABI work); Flatpak home-directory grants (common; no
reliable current figure); anti-cheat on Linux (Easy Anti-Cheat and BattlEye
are user-mode and opt-in per game, Riot Vanguard has no Linux path); and, for
the revision, firmware requirements of NVIDIA RTX 30 and Intel Arc cards under
Linux.

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
  boot states. Section 4.5 takes the idea seriously as a thought experiment and
  accepts that cost.
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
includes Ian. That is not a criticism, and daily use is not the goal. It is
the baseline against which any concept tested on Brook has to be read.

### Who would choose Brook, and why

In descending order of how confident I am:

1. **Ian, and people like him,** for the pleasure and understanding of owning
   the whole machine. This value is real and already delivered.
2. **People exploring OS concepts and agent-operated computers.** An OS where
   one team can change any layer, from the scheduler to the launcher, and then
   run Firefox and Steam on the result, is rare. This is the audience Ian has
   chosen, and Brook already serves it.
3. **A single-owner appliance user:** someone who wants a machine that runs a
   browser, an office suite and Steam, cannot be wrecked by what they install,
   and never asks to be administered. Today they buy a Chromebook, an iPad or
   a Steam Deck. Brook is not aiming at them, but they are the right imagined
   user when judging whether a concept is any good.

Linux enthusiasts who want a better Linux will not come. They value exactly
the choice that Brook removes.

### What daily use would require, for the record

Daily use is not the goal. It is still worth knowing the distance. All four
would be needed: one machine supported completely (suspend, Wi-Fi, battery,
Bluetooth, each a driver project); a browser at parity including its sandbox;
no data loss, ever; and one thing Linux does not offer that the owner notices
weekly. The hardware treadmill, not kernel design, is what has ended every
alternative desktop OS.

### What a concept platform requires instead

A concept is validated on Brook only if the result would survive contact with
real software. That needs three things, and they set the order of the plan:

1. **Real workloads run well enough that a result is not an artefact of
   slowness or missing ABI.** A confinement experiment that fails because
   Stardew takes two minutes to start has measured nothing.
2. **Hardware with headroom.** A 2015 integrated GPU cannot show whether an
   idea costs 2% or 20% on a demanding game.
3. **A kernel that can be changed safely.** Concepts will be prototyped by
   changing core code. Without interfaces and tests, each prototype risks the
   last one.

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
Linux has the drivers. Brook has about ten driver modules and one GPU driver.
Every new machine is a driver project, and that now matters, because the
agreed direction includes a discrete GPU. One mitigating fact, from memory and
worth verifying: Linux's GPU drivers are largely MIT-licensed, unlike most of
the kernel, so they can be read and adapted for an MIT kernel. The obstacle is
their size, not their licence.

### What others tried

| System | What it tried | What worked | Why it did or did not win |
| --- | --- | --- | --- |
| Android | App as the security principal; intents; OS-owned lifecycle; one toolkit | All of it, at scale | Won. New hardware category, one owner, and apps written fresh for it. Scoped file access still took a decade to retrofit. |
| iOS | The strictest version of the same | Malware is rare; non-experts are safe | Won. The cost is that the owner is not in charge, and general computing arrived late and partially. |
| ChromeOS | Stateless, verified, auto-updating system; the web as the app platform | Near-zero administration | Won in schools. Linux and Android apps were bolted on through a VM and a container. Now being folded into Android. |
| macOS | Retrofit: App Sandbox, per-resource consent prompts, notarisation, a signed system volume | The OS-owned open/save panel grants access invisibly and well | Mixed. Sandboxing is optional outside the store, so the consent prompts carry the load, and users learn to click through them. |
| Fuchsia | A complete do-over: capabilities, components, typed IPC, no ambient authority, plus Starnix to run Linux binaries | The design | Did not win. It shipped on smart displays. A large, well-funded team with the right architecture did not displace Linux, because there was no app-level reason to switch. |
| Genode / Sculpt | A capability-based component OS that its developers use daily | Proves a small team can live on such a system | Tiny adoption. Software must be ported or run in a VM. |
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
7. **Identity.** The app is the primary boundary. Multiple users are not
   excluded; if they arrive, a user is an outer boundary around a set of
   compartments. The ordinary single-owner experience has no login ceremony.

The desktop contract is toolkit-independent because it is defined as
protocols, not as a widget library: a Wayland surface, the broker's
interfaces, and a semantic tree. I would not build a Brook widget toolkit.

The important design choice is how the broker is exposed. **Brook should
offer the xdg-desktop-portal D-Bus interfaces as the compatibility face of its
broker, as a fork that talks to Brook natively and not as a shim over stock
backends** (Ian's refinement). That API is already a deployed powerbox. GTK,
Qt, Electron, Firefox and Chromium already call it when they believe they are
sandboxed. An app that uses portals becomes nearly native on Brook without
modification. This is the single highest-leverage adapter available. Brook has
no portal implementation today (I searched `docs/`, `src/` and `tools/`). It
is sequenced after the performance work.

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

Compatibility costs to plan for: apps that run code they download (Steam,
developer tools) need a compartment where that is allowed; apps that expect to
see each other need declared, brokered connections; and versioning needs
either broker-side copies or a filesystem better than ext2. The plan's
compartment table covers each.

### 4.3 Cross-platform

Position: **Linux first, the web second, Windows through Proton third, and
Android last, as a risk to weigh and not a ruling.**

| Ecosystem | Path | Cost | Priority |
| --- | --- | --- | --- |
| Linux | The existing ABI personality, each app in a compartment, portals for integration | Ongoing ABI gaps; this is already the main cost of the project | 1. It is the substrate for the next two |
| Web | Installed web apps as first-class apps: own launcher entry, own window, own compartment and browser profile | Small. It rides on Chromium or Firefox, which already run | 2. Cheapest large gain |
| Windows | Wine and Proton as Linux programs. One Wine prefix per compartment | Moderate. It already works for several titles (`docs/proton.md`, `docs/wine-unity.md`, `docs/d3d12.md`) | 3. Games are the strongest reason an appliance user needs more than a browser |
| Android | A full Android userland in a container, plus binder in the kernel, a graphics bridge, and ARM translation on x86 | Very large, and the result is poor | 4. My advice is to defer it |

On Android specifically. Waydroid shows the shape: binder in the kernel, a
Wayland session, a container and a multi-gigabyte system image. Brook would
also need to meet Android's expectations of cgroups and SELinux and bridge its
graphics stack. Then many apps ship ARM-only native code, apps that depend on
Google Play services fail without them, and banking apps refuse on integrity
checks. Microsoft built this for Windows 11 and withdrew it in March 2025. As
a concept study it has some interest, since Android's app model is the closest
relative of compartments. As a workload it costs more than anything else here.

On anti-cheat: existing kernel-level anti-cheat will not run on Brook, and
its vendors do not support Linux even with the Steam Deck's volume behind it.
That is accepted. The interesting question is the inverse one, taken up in
section 4.5.

What "seamless" should mean. An app from any ecosystem is seamless when all of
the following hold, and none of them requires the app to hold ambient
authority:

| Integration point | Seamless means |
| --- | --- |
| Windows and launcher | Each app window is a Brook window with Brook chrome, never a desktop-in-a-window. One icon, found by the same search. |
| Documents | Open and save go through the system picker. Inside a Wine prefix the chosen file appears at a stable drive path. |
| Clipboard and drag-and-drop | Work across ecosystems, delivered on paste or drop and not readable in the background. |
| Notifications and audio | One notification surface, attributed to the app. One mixer with a volume control per app. |
| Input and settings | The same layout, input method, scale and dark mode, where the toolkit can honour them. |
| Lifecycle | Close means close. The system can stop the whole app, helpers included. |

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
| Explicit save, and "unsaved changes" on close | Autosave with versions for native apps. For compatibility apps, the broker's versions make an accidental overwrite recoverable. |
| App-drawn window frames, tray icons, per-app update checkers, per-app autostart | System chrome; one background-activity surface showing what is running and why; none of the rest. |
| Config files and dotfiles | One settings surface. Per-app settings live in the compartment and "reset this app" always works. |
| Invisible background processes | Anything running without a window is listed, attributable and stoppable. The system may suspend idle apps. |
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

### 4.5 A thought experiment: making kernel anti-cheat unnecessary

The question, as Ian put it: if the OS can attest the state of the machine,
and apply rules about memory and cross-app interaction when an app asks for
them, can it offer guarantees that kernel-level anti-cheat cannot? This is a
concept study. Whether any vendor would adopt it is a separate question,
addressed under the limits.

**Why kernel anti-cheat exists.** On Windows the owner is an administrator
and can load drivers, so cheats run in the kernel to read and write game
memory. Anti-cheat follows them into the kernel, and loads at boot to be there
first. It is an arms race at ring 0 that exists only because the OS hands the
owner ring 0 and gives a game no way to ask for less. Everyone pays:
third-party code with total access to the machine, and the privacy, stability
and security problems that come with it.

**The threat model is unusual: the adversary is the machine's owner.**

| Cheat class | How it works |
| --- | --- |
| Memory read | Another process or driver reads game state (wallhacks, radar). |
| Memory write and injection | Code or data is altered in the game process. |
| Synthetic input | Software generates aim or trigger input. |
| Screen reading | Software captures the game window and reacts to it. |
| Overlays | Another program draws over the game. |
| Modified client | Game files or libraries are replaced. |
| Below the OS | A hypervisor, or tampered firmware, sits under the kernel. |
| DMA hardware | A PCIe device reads memory directly. |
| Outside the machine | A camera or capture card feeds a second computer, which sends input through a device that looks like a mouse. |

**What the OS would attest.** A signed statement, bound to a TPM quote, of
facts a server can check:

- the measured boot chain: firmware, loader, kernel and system generation;
- that this boot has no owner-loaded kernel code and the debug bridge is
  absent, not merely idle;
- that DMA remapping is on and covers every device;
- the identity of the game's compartment: the hash of its immutable code;
- the policy currently applied to that compartment, and that any change ends
  the session.

**The policy an app could request.** A game's manifest asks for a protected
session, and the OS applies, for that compartment only:

- no memory access from outside it by any route (ptrace, `/proc`, shared
  mappings it did not create), including by the owner's tools;
- no code in the process except from its signed closure, so no preloaded or
  injected libraries;
- no capture of its windows by other apps and no overlays above them
  (`DECISIONS.md` already anticipates a capture opt-out for competitive games);
- input delivered only from physical devices, with injected events excluded;
- no memory dumps, and GPU buffers isolated from other contexts.

Most of this is the compartment model with stricter settings. That is the
point: the mechanism is the same one that protects a password manager.

**What this offers that kernel anti-cheat cannot:**

- **No third-party kernel code at all.** The game learns the answer to a
  defined question. It does not get to inspect the machine.
- **Prevention by construction.** Anti-cheat detects known cheats after the
  fact. Here the capability to read another compartment's memory does not
  exist in a protected session.
- **Input provenance and capture denial as facts the OS states,** not
  heuristics the game guesses at.
- **A statement the owner can read.** Exactly what is attested is public, and
  the code enforcing it is open.
- **Scope.** The restriction applies to one compartment in one session. The
  rest of the machine stays the owner's.

**Honest limits.**

- **The protected state is a property of the boot, not only of the session.**
  If the owner had kernel access earlier in this boot, the kernel cannot vouch
  for itself. So there are two boot states, as on ChromeOS, and the owner picks
  one at startup. This resolves the tension noted in section 1; it does not
  remove it. (Speculation: a late-launch mechanism could avoid the reboot.)
- **Anything outside the machine is invisible.** A camera, a second computer
  and an input-emulating device defeat every OS-level scheme, this one
  included. Server-side detection remains necessary.
- **DMA and firmware.** An IOMMU stops ordinary DMA devices if it is enabled
  from boot. It does not stop tampered firmware, and TPM attestation can be
  relayed or virtualised by a determined attacker.
- **The kernel is the weak point.** One exploitable bug in Brook's kernel is a
  universal cheat, and a young C++ kernel with a Linux-sized syscall surface
  has such bugs. This concept is credible only after the hardening path in the
  plan.
- **Vendor trust.** A server must trust a signing key for "blessed" Brook
  builds, and someone must operate verification and revocation. Windows games
  under Proton expect their own Windows driver and need the vendor's
  cooperation regardless. No vendor will do this for a research OS.
- **Collateral.** The same rules block mods, overlays, streaming software and
  accessibility tools that synthesise input, unless each is a declared, signed
  partner of the session.
- **Privacy.** Attestation can identify a machine. A real design needs an
  anonymising step.

**Smallest prototype.** No vendor is needed. Take an open-source multiplayer
game Brook already runs (Quake II has LAN deathmatch per `README.md`), add a
toy server that verifies a quote from a software TPM in QEMU, and write a
cheat fixture suite: a memory reader, a library injector, an input
synthesiser and a window grabber. Success is that every fixture fails in a
protected session, works in a normal one, and the server refuses a boot with
debugging enabled.

## 5. Do-over decisions

Ten choices I would make differently from Unix if starting today. For each:
why, what it costs Brook given its Linux personality, and the smallest
prototype that would test it. Decisions 1, 2, 6 and 8 are the ones the
compartment concept depends on. The others are candidates for later study.

**1. A process starts with nothing and is handed what it may use.**
*Why:* ambient authority is the root of the malware problem. Plan 9 and
Fuchsia construct a namespace per process; nothing is reachable by default.
*Cost:* none to compatibility if the Linux personality synthesises a normal-
looking filesystem inside the compartment. Apps that expect to share dotfiles
or `/tmp` sockets with other apps break and need declared connections.
*Prototype:* launch GIMP with a private home and no view of `/data/home`. Open
and save one file through a broker-granted path. Count what breaks.

**2. The app, not the user, is the security principal.**
*Why:* a personal computer has one or a few users and a hundred mutually
distrustful programs. *Cost:* UIDs remain as a fiction inside each compartment (always
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

**4. Events and completions, not signals and blocking calls.**
*Why:* signals are asynchronous, process-global and nearly impossible to
handle correctly; blocking calls with `EINTR` and no cancellation are why UIs
freeze. io_uring and `signalfd` are Linux conceding both points. *Cost:* both
stay in the Linux personality, and Linux apps cannot use the replacement.
*Prototype:* make "please close" for native apps a message with a deadline
and an acknowledgement, building on `docs/orderly-shutdown.md`.

**5. System interfaces are typed, versioned messages, not ioctls and text
files.** *Why:* `/proc` text, `ioctl` numbers and ad-hoc sockets cannot be
checked, versioned or introspected. Fuchsia's FIDL and Android's binder
interfaces can. *Cost:* the Linux-facing `/proc` and `/sys` must still be
synthesised. *Prototype:* define the broker's interface in a schema, and
generate both the portal adapter and the bridge's tool definitions from it.
One contract for apps, accessibility and agents is the directions document's
best idea; this is how to make it literally true.

**6. The system is an immutable, signed image, and state is separated into
four kinds:** system, app code, app private data, and the owner's documents.
*Why:* it makes updates atomic, rollback possible, uninstall complete and
self-modifying code impossible. *Cost:* self-updating apps (Steam, browsers'
own updaters, editor extensions) need a writable, executable area inside their
compartment. *Prototype:* switch a whole generation, including the app
profile, and roll it back from the boot menu on the Dell. Most of the parts
exist.

**7. Every object has an unforgeable identity that is never reused.**
*Why:* PIDs, UIDs, window titles and descriptor numbers are integers that get
recycled, and whole classes of bugs and attacks follow. *Cost:* Linux apps
still see PIDs. *Prototype:* largely done for windows and bridge jobs. Extend
the same identity to processes and compartments in the kernel's job API.

**8. Cross-app access happens through an OS-owned act (the powerbox), not a
permission setting.** *Why:* prompts train people to click "allow". Picking a
file is something the person was going to do anyway. macOS's sandboxed
open panel shows this works. *Cost:* apps that do not use the portal file
chooser need a fallback: an "Import" shelf in the compartment's home that the
owner drops files into. *Prototype:* implement the portal FileChooser and
OpenURI interfaces; run the twenty most-used apps; record which use them
unprompted.

**9. The OS owns app lifecycle.**
*Why:* phones stay responsive because the system may suspend or stop anything
not in use. Desktops degrade because everything runs forever. *Cost:* Linux
apps assume they are immortal; some lose network sessions when frozen.
*Prototype:* freeze a background compartment under memory pressure and thaw it
on focus. Measure against the existing out-of-memory behaviour
(`docs/oom-recovery.md`).

**10. Drivers and the compositor are restartable, isolated components behind a
stable interface.** *Why:* most kernel crashes and most kernel attack surface
are drivers. *Cost:* large, and the GPU driver is the hardest case. I would
not attempt a microkernel conversion. *Prototype:* finish moving the window
manager and compositor out of the kernel, which `compositor-direction.md`
already intends. Then stop, and reconsider with data.

## 6. Recommendation

### Thesis

Brook is a platform for testing what a personal computer could be if one
owner controlled every layer. Its headline concept is that every program,
including unmodified Linux and Windows software, runs as a guest in a world
the OS builds for it: its own identity, its own private storage, and nothing
of the owner's except what the owner hands over through the system. Linux is
one guest personality, exact in its semantics and arbitrary in what it is
shown. Brook's own design goes into the layer between apps and everything
else. Because a concept proves nothing on a system that cannot run demanding
software well, Brook first earns the ability to validate: real games and
browsers at good frame rates on capable hardware, on a kernel whose interfaces
are defined and tested well enough to be changed, and eventually rewritten,
with confidence.

### Sequence

Six workstreams. The first three make validation possible. The last three are
the concepts. They overlap, but the order of emphasis is as listed.

**1. Performance and compatibility, driven by games and browsers.**

Finish the active 60 fps goal on the Dell. Keep bringing up games and browsers,
chosen for what they exercise. A new title earns its place by adding syscalls,
ABI paths or load patterns that the existing set does not cover; the
compatibility census (`work/compat-census` in `HANDOVER.md`) can make that
choice explicit. Seccomp and PID and network namespaces
(`chromium-sandbox.md`) belong here, as compatibility work: they let Chromium,
Electron and Steam run the way upstream ships them, with no `--no-sandbox`.
*Measure:* displayed frames per second against a Linux run of the same
workload on the same machine; census gaps closed per workload added; number
of per-app launch workarounds.

**2. A more capable hardware target with a discrete GPU.**

There is an untracked draft, `docs/discrete-gpu-bringup-plan.md`, in the
legacy checkout `/data/Programming/Codex/Brook`. It is not in main yet and
should be committed there. It proposes
short bare-metal sessions on the existing desktop (ASUS Z370, RTX 3080),
booting from USB with internal disk writes disabled, proving platform boot
and the debug connection first, then a first accelerated backend, then game
comparisons against Linux on the same box. Broadwell stays as the integrated
regression target.

I agree with its order and its caution. Two things to reconcile:

- **The smoke test is worth doing on that desktop whatever card comes next.**
  It exercises what the Dell never did: PCIe topology, 64-bit BARs, DMA
  remapping beyond one integrated device, more cores, and a NIC with no Brook
  driver.
- **The RTX 3080 is probably the hardest of the three candidate cards for a
  first accelerated backend.** From a web check today (news coverage, not Mesa
  documentation): open NVIDIA support runs through nouveau's kernel interface,
  and usable performance on RTX 30 depends on NVIDIA's large signed GSP
  firmware. An Arc A380 would keep the Mesa Iris and ANV drivers and the
  i915-style kernel interface Brook already implements for Broadwell, though
  it too needs firmware (GuC). An RX 6600 means a new kernel interface, backed
  by fully open source. All three need firmware blobs. My position: decide the
  first accelerated card after the smoke test, with a written cost comparison,
  and expect an Arc card in the same desktop to be the shortest route to "can
  game". This is a judgement from partial information.

*Measure:* the draft's own criteria, plus one Steam title with matched
settings and displayed frame pacing against Linux.

**3. Interfaces, tests, then Rust.**

The codebase already leans this way. `syscall.cpp` has been split by
responsibility as a pure move (`docs/agent/syscall-split-plan.md`); 182
issues are logged for a fix pass that "adds test coverage first"
(`docs/agent/REFACTOR_ISSUES.md`); policy logic lives in headers and `.inc`
fragments that host tests include directly; and guest matrices already use
Linux as the oracle (`docs/exec-permissions.md`: 672 cases). The proposal
extends that practice. It does not replace it.

*Defining the interfaces.* Three tiers, each with a different kind of test:

| Tier | Interface | Form | Tests that survive a rewrite |
| --- | --- | --- | --- |
| External | The Linux syscall ABI | Already defined, by Linux | Guest workload matrices run on Linux and on Brook. Language-neutral. |
| Module | Kernel to driver (the versioned module ABI) | Already a C-level boundary | Per-driver conformance fixtures against a fake kernel side. |
| Internal | Between subsystems: VFS backends, block and network devices, scheduler policy, memory, user-copy, the syscall handler table | To be written: one C-ABI header per boundary, using plain structs and opaque handles | Contract tests against the header, with no knowledge of the implementation. |

Each internal header states what C++ leaves implicit and Rust will demand: who
allocates and frees, what may sleep, which locks are held on entry. Writing
that down will surface bugs before any Rust exists.

*Measuring coverage.* Three numbers per subsystem:

- line and branch coverage of host tests, from Clang's source-based coverage,
  which the existing LLVM toolchain supports and which rustc shares;
- the fraction of interface functions with at least one contract test;
- the fraction of syscall handlers exercised by a guest matrix with a Linux
  oracle.

In-kernel coverage on a running guest is harder. Speculation: an instrumented
build could export its counters through the bridge.

*Converting the tests to Rust, before any code.* Write the contract tests in
Rust on the host, calling the existing C++ implementation through the C-ABI
header (generated bindings). Nothing in the kernel changes yet. This proves
the headers are sufficient, since a test that needs to reach around one has
found a hole in it. It also brings property testing and fuzzing to the C++
code immediately, on the same LLVM.

*Porting, one subsystem at a time.* A Rust implementation of one interface is
built as a `no_std` static library and linked into the same kernel. The same
Rust tests run against both implementations with the same inputs. A
difference is a bug in one of them, and often in the original. Suggested
order, from least to most entangled:

1. Pure policy headers that are already host-tested. Small, and they prove the
   toolchain, the panic strategy and the allocator hookup.
2. Parsers of untrusted input: ELF loading, on-disk filesystem structures,
   partition tables, netlink and network packets, user-memory copying. This
   is where memory safety pays most.
3. One whole leaf subsystem behind the VFS interface, such as tmpfs or FAT.
4. Syscall handlers, a family at a time. After the split, the dispatch table
   can point at a Rust function per entry.
5. Core memory management and the scheduler, last, or never if the earlier
   stages show diminishing returns.

*Gates and risks.* Do not port a subsystem until every interface function has
a contract test and its branch coverage passes a threshold Ian sets. Port only
subsystems that are quiet; one being reworked for performance is a moving
target. Track `unsafe` per crate, because agents translating C++ will
otherwise produce C++ in Rust syntax. Where an interface shares mutable
structures across the language boundary, narrow the interface first. New
drivers are a tempting place to start in Rust; I would not write the discrete
GPU backend in Rust unless stage 1 is already done, to avoid stacking two
unknowns.

*Measure:* the three coverage numbers; subsystems with a complete interface
card; subsystems ported; open High and Medium entries in the refactor log.

**4. Compartments, the headline concept.**

The claim to test: most Linux apps run confined with no change to the app and
no visible friction. The mechanism has five parts.

- **A compartment object in the kernel.** It has a non-reused identity. A
  trusted launcher creates it; every process the app starts inherits it; there
  is no call to leave it.
- **A constructed filesystem view.** Brook already has mount namespaces, bind
  mounts, tmpfs and `pivot_root` from the Steam and bubblewrap work (the
  `mount-*`, `tmpfs-*` and `pivot-root` checkpoints). The launcher uses the
  same machinery on the app's behalf: the store read-only, a private directory
  mounted as the home, private `/tmp` and runtime directories, a minimal
  `/dev`, and a filtered `/proc`. The app sees uid 1000 and an ordinary
  system.
- **A scoped process view.** Signals, ptrace and `/proc` stop at the
  compartment boundary.
- **Per-compartment endpoints.** Its own Wayland socket, audio socket and
  session bus, so the system knows which app is speaking by which socket it
  used. Its own Xwayland if it needs X11.
- **Documents bound in at their real paths.** This is where owning the kernel
  beats a shim. When the owner picks `Documents/report.odt`, Brook binds that
  one file into the compartment at the same path the owner sees. Flatpak's
  document portal exposes an opaque path instead, which confuses recent-file
  lists and relative links. Grants can cover a folder and can persist.

What each kind of app needs:

| App class | What it needs to be seamless |
| --- | --- |
| Games, native and Proton | Nothing. Saves and settings live in the private home. Expected to be the easiest class, and the first to demonstrate. |
| Browsers | A granted Downloads folder; the system picker for uploads; nested namespaces and seccomp so the browser's own sandbox runs inside the compartment. |
| GTK and Qt apps using their toolkit's standard file chooser | A manifest setting that switches the toolkit to the portal chooser. |
| Apps with their own file dialogs (GIMP, parts of LibreOffice, Java, Wine apps) | Launch-with-file from the Files app, drag-and-drop, and persistent folder grants. The dialog shows the private home with granted items in place. |
| Electron apps and editors | The portal chooser, plus project-folder grants. |
| Steam | Treated as a nested platform. Downloaded code runs inside Steam's compartment, and its own bubblewrap nests within it. |
| Terminals and developer tools | A workshop compartment with folder grants (section 4.4). |
| Apps that talk to each other (password manager and browser, "open with", link handlers) | A declared, brokered connection per pair. This is the class most likely to need manual work. |

Which toolkits honour the portal switch in practice is from memory and is the
first thing to measure.

Framed for the agreed direction: this is built now as an architecture and a
user-experience concept. It will already stop apps reading each other's data
and make uninstall complete. It is not presented as a defence against a
determined attacker until workstream 3 has hardened what enforces it.
*Measure:* a hostile-app fixture suite kept as a regression test; of a fixed
app list, how many run with no folder grant beyond what the owner chose; frame
rate and startup time inside and outside a compartment.

**5. A native xdg-desktop-portal, once performance allows.**

Fork the portal front end so that it speaks to Brook's broker directly,
instead of running the stock service with a backend shim. Apps see the
standard D-Bus interface; behind it there is no FUSE document store and no
separate backend process, because the kernel binds granted files itself. Start
with FileChooser, OpenURI, Notification, Settings and Screenshot. Broker-side
versioning of files granted for writing follows. (From memory: the upstream
project is LGPL. A user-space fork under that licence sits fine beside an MIT
kernel, but check before starting.)
*Measure:* for each app, open, edit and save through the picker with no
folder grant; picker latency against the same app on Linux.

**6. Thought experiments with small prototypes.**

The anti-cheat study in section 4.5, with its toy server and cheat fixtures,
after compartments exist. The agent interface: a compartment-scoped grant for
an agent in place of machine-wide debug authority, and the accessibility
ingestion proposed in `vm-bridge-next.md`. Multi-user is a non-goal for now
and is not excluded: if it arrives, a user is an outer boundary that contains
compartments, and nothing in the compartment design should assume there is
only one.

### Risks that remain

- **Confinement may not be seamless for enough apps.** Flatpak's experience is
  that many need broad grants. Brook's advantage is real-path binding and
  control of the kernel side. Whether that is enough is the experiment.
- **The compatibility tail does not end.** Linux adds interfaces every
  release. Workload-driven coverage keeps the effort honest, and it stays a
  permanent cost.
- **A discrete GPU driver is the largest single piece of work proposed here.**
  It could absorb the project for months, as Broadwell did.
- **The Rust path competes for the same hours as everything else,** and a
  half-finished port leaves two languages and one set of bugs.
- **Per-app launch recipes held in handover notes and scripts** are
  fragmentation leaking in. They should move into manifests as each app is
  placed in a compartment.
- **Android** remains the worst cost-to-value ecosystem on x86 (section 4.3).
- **A native syscall API** remains the most attractive work with the fewest
  users (section 2). Build native interfaces where system components need
  them.
- **Security wording.** Until the kernel is hardened, compartments should be
  described by what they do, not as protection. "Impossible to get a virus"
  should stay retired; section 4.2 gives the claim that can eventually be
  defended.

### How to tell whether it is working

For the platform: a demanding game and a browser run on the desktop at frame
rates within a stated factor of Linux on the same hardware, and coverage
numbers exist for every subsystem.

For the headline concept: the hostile-app suite passes; most of a fixed app
list runs confined with no grant the owner did not choose; and Ian, using it,
does not reach for a way to turn it off. If more than about half the apps
need broad grants, or the confinement is correct and Ian disables it anyway,
the concept has failed as designed and the finding is worth writing up. On a
concept platform a clear negative result is a result.

The thresholds here are proposals to make the tests falsifiable. Ian should
set them before starting, not after.
