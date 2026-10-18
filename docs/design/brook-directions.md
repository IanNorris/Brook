# Brook beyond compatibility

Status: future design exploration, requested 18 October 2026. This does not
replace the current reliability, application and performance work. The proposals
below are hypotheses and research questions, not implemented guarantees or a
decision to rewrite the kernel, desktop or compatibility layers.

The central question: what could a personal computer become if its owner had one
coherent environment, applications had independent authority, and both people
and agents could operate it through understandable, observable actions?

## One owner, many application identities

Explore making the application or workspace the main isolation principal while
keeping a simple personal environment for its human owner. A distinct Unix UID
per app could be a useful first prototype; it is not the complete model.

Separate these identities explicitly:

- The person who owns the machine and their preferences and documents.
- An installed application, its updates, private state and declared capabilities.
- A running instance or sub-environment, including project-specific permissions.
- Helpers, extensions and agents acting with explicitly delegated authority.

Questions include stable identities across updates/reinstallation, per-project
instances, package provenance, migrations and backup/restore. Avoid tying a
permanent app identity to a reused numeric PID or UID. Preserve the possibility
of shared machines and multiple people without forcing that complexity into the
ordinary single-owner experience.

Prototype private application homes and explicit document handoff. Opening a
document should not require giving an app the owner's entire home directory.
Define how saving, replacing, exporting, directory access and collaborative
editing work, including revocation and applications that retain open handles.
Cross-application workflows must remain convenient enough that isolation survives
normal use. Browser profiles and existing documents must migrate without loss.

## Memory isolation and deliberate sharing

Define what “total app memory isolation” means before claiming it. Investigate
address spaces, process inspection/debugging, signals, inherited descriptors,
shared mappings, device access, GPU resources and privileged services together.
Specify which components are trusted and which observations are outside scope.

Treat shared memory as an explicit exception with a purpose, owner, access
rights, bounds, lifetime and crash/revocation behavior. Work through compositor
surfaces, audio queues, clipboard contents and high-throughput IPC as concrete
examples. Compare copying, read-only sharing and writable sharing with measured
latency and memory costs. App extensions and child processes need an explicit
trust/delegation model rather than automatically acquiring every parent right.

Keep an intentional owner-controlled development/debugging mode. Investigate how
to make its authority visible and scoped without weakening ordinary app
boundaries or making Brook unpleasant to develop on.

## Anti-cheat under an attestable trust model

Explore this conditionally: assume a relying party trusts and can verify the
boot key, and ordinary users cannot load arbitrary kernel drivers into that
attested environment. What guarantees could Brook then offer a game?

Research the exact statement being attested, update/revocation policy, approved
debugging states, device/firmware trust, DMA and GPU access, virtualization,
screen/input observation and external devices. Distinguish protection of game
memory from broader definitions of cheating. Do not equate isolated memory or
verified boot with proof that all gameplay is legitimate.

Explore an opt-in protected game environment with a small documented interface
instead of bespoke third-party kernel extensions. Establish what cooperation
game/platform vendors would need to provide. Consider owner control, privacy,
offline use, accessibility tools, streaming and the experience when attestation
is unavailable. This is a threat-model and feasibility study, not a promise of
anti-cheat compatibility or mandatory remote attestation for the desktop.

## Agentic computing as a native interface

Design apps so their meaningful state and actions are available semantically:
documents, selections, commands, progress, errors, cancellation and results.
Explore a shared action model for UI, accessibility, scripting and agents, with
stable object identities, versioned schemas and discoverable capabilities.

Commands should have observable completion and explicit effects. Investigate
caller IDs, retry behavior, event streams, transactions/undo and previews for
actions that warrant them. Agents should receive delegated application/workspace
authority, not implicitly become the machine owner. Human takeover and inspection
should be normal interactions.

Use the [VM bridge specification](../vm-bridge-next.md) as an existing
testbed: semantic actions and efficient artifacts can inform the design without
turning the VM's debugging privileges into the native app permission model.
Visual interaction remains useful, especially for compatibility apps; it should
not be the only way a native app exposes a button or long-running operation.

## Multiple application ecosystems, one desktop

Explore a common desktop contract across these execution environments:

| Environment | Design questions |
| --- | --- |
| Native Brook | What can the native API offer that compatibility apps cannot express directly? |
| Linux | Combine syscall/ABI compatibility with Wayland desktop integration; keep these two seams explicit. |
| Windows through Proton | How do file access, graphics, input, process identity and desktop lifecycle map through the compatibility stack? |
| Android | Investigate runtime/system-service requirements, graphics/input integration, package distribution and application/service compatibility. |

Unify document opening, icons, launch feedback, audio, scaling, accessibility,
settings and window/task behavior. Preserve provenance and meaningful permission
differences. A compatibility adapter should not acquire unrestricted authority
merely to make path translation convenient. Identify which native contracts can
be supplied by adapters and which require app changes; do not promise identical
coverage for every ecosystem.

## What makes an application native to Brook?

Draft a small native app contract, then demonstrate it with a useful editor or
file tool. Candidate properties include:

- Private persistent state and explicit, portable document formats.
- Scoped access to documents, services and shared objects.
- Semantic actions and state, usable by humans, accessibility tools and agents.
- Asynchronous work with cancellation, clear errors and resource/progress reporting.
- Consistent rendering, text, input, theme and scale behavior.
- Versioned contracts that can evolve without requiring app and OS updates together.

Investigate whether native apps should be organized around durable documents or
tasks rather than a process/window lifetime. Keep familiar files and ordinary
programming languages usable; a new object model needs clear interoperability,
export and recovery stories before becoming a dependency for user data.

## Revisit OS/API choices with evidence

Investigate Linux, Windows and macOS tradeoffs without assuming that historical
constraints were mistakes or that a new API is automatically faster.

| Area | Question to test in Brook |
| --- | --- |
| I/O and synchronization | Where do batching, asynchronous completion, cancellation and explicit ownership reduce measured overhead? |
| Names and authority | Which global names or ambient permissions can become scoped handles without breaking ordinary workflows? |
| Memory and data movement | Which copies or shared mappings dominate real workloads, and what lifetime/error contract would improve them? |
| ABI evolution | Where should stable binary compatibility live, and how can native interfaces negotiate versions and capabilities? |
| Failure semantics | Can incomplete operations, cancellation and unsupported features be made explicit rather than returning misleading success? |
| Services and startup | Which services need to be resident, and what can start on demand with dependable readiness and completion? |
| Compatibility state | How can manifests, adapters, per-app environments and migrations avoid permanent system-wide workarounds? |

For each proposed change, record the historical constraint, current workload
evidence, alternative design, compatibility cost and a benchmark or failure test.
Keep the Linux-facing ABI adapter separate from decisions about the native API.
Include old applications, crash recovery and update/rollback in compatibility
tests rather than evaluating only a freshly built example.

## Revisit desktop conventions

Question whether windows, taskbars, application launchers, folders and modal
dialogs are always the right organizing units. Explore task/document workspaces,
searchable actions, resumable activities, visible background work and surfaces
that can be operated equally well by a person or agent.

Use concrete tasks to compare concepts: open/edit/share a document across apps,
recover a failed operation, manage a long job, switch projects, and find where an
app is saving data. Test keyboard/mouse use, accessibility, discoverability,
scaling and visual density. Preserve a conventional desktop option while testing
alternatives. Novel appearance alone is not evidence of a better interaction.

## Follow-up deliverables

When current workload reliability permits, produce:

1. A short principal/delegation model and threat model, including sharing exceptions.
2. A native app/API sketch with one end-to-end document workflow.
3. A compatibility matrix for Linux, Proton and a possible Android environment.
4. Two or three desktop interaction concepts evaluated against the same tasks.
5. A small prototype: an isolated native editor and a Linux app exchanging an
   explicitly selected document, with semantic automation and observable cancellation.

Acceptance should include useful isolation tests, denied access with meaningful
errors, retained documents/settings across updates, crash recovery and measured
costs. Any anti-cheat proposal needs its own conditional guarantees and feasibility
review. Choose implementation work after reviewing these results; this backlog
does not authorize starting a new platform rewrite ahead of the active goals.
