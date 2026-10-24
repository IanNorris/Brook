# Steam PRIME context repair

Steam login remains incomplete. Review X fixes a reproducible cross-context
VirGL resource error and passes rendered-pixel/lifetime regressions. A later
Xwayland fence-memory crash and Steam transport error remain separate leads.

## Change

PRIME import previously installed a GEM alias without attaching its resource to
the destination GPU context. The independent GLX client could pass its own red
pixel readback while causing Xwayland's context to reject that foreign resource
and blacken an already-working xeyes window.

The driver now records legacy resource attachments by creator and importer
context incarnation. Canonical GEM/PRIME/VMA references retain the resource and
creator; the association registry takes no extra resource references. Repeated
and self imports are deduplicated. A lazy importing context is initialized
before attachment. Host-blob/Venus cross-context imports are explicitly rejected
by this bounded path, rather than certified by an unsuitable VirGL fence.
The appended driver operation requires module ABI 51 and matched modules.

The existing GPU mutex serializes publication, shared use and retirement.
Initial import completes earlier creator work. Subsequent submissions and
transfers fence directly sharing contexts before use and the submitting
context after use. This also covers legacy submissions and later producer
writes without relying on a requested output fence FD. GL EXECBUFFER snapshots
and validates its entire BO list before GPU publication; duplicate entries
cause no additional reservation or ownership acquisition.

Final canonical-resource retirement fences/detaches recorded importers before
ordinary backing retirement. Destination destruction detaches its imports
before its context ID can recycle. Each confirmed detach is recorded separately.
GEM close/reimport of a still-live resource retains its existing association;
there is no queued old detach that could invalidate a newly published alias.
Association metadata disappears on resource retirement or destination teardown,
without keeping backing alive until Xwayland exits.

Partial submission/transfer errors attempt completion and mark sharing state
unusable. Ambiguous attach, fence or detach retains metadata/backing for
quarantine instead of freeing potentially live DMA storage. This is a
conservative correctness path, not an asynchronous-throughput optimization or
complete implementation of Linux DRM scheduling/security semantics.

## Validation

All 103 host tests pass. The new test compiles the production association,
submission and transfer bodies with fault-injected device operations. It checks
identity/deduplication, context incarnation reuse, importer-first/resource-first
detach, allocation/publication failure, partial detach progress, partial command
submission, completion attempts, and post-submit/transfer fence failures.
Astra's read-only review finds no blocking lifetime or lock-order gap.
Compiled entry-state and scheduler mutation audits pass in their supported scope.

Fresh review X boots the matched 13 ABI-51 modules and exact kernel
`3235c61d2c2dcb06c0b67ba16b65fd3069f546bf151bf4824ce2023983bda84f`.
The isolated xeyes/GLX sequence no longer blackens xeyes or emits the previous
illegal-resource/CREATE_OBJECT errors. The eyes remain visible and update after
the GLX client exits. A red stripe remains in the X11 image, including after an
explicit full repaint; presentation is not declared fully correct.

`gpu_prime_render_check.py` checks all 16 pixels of a shared 4-by-4 texture.
Both runs pass initial producer render, later producer render after import,
importer render, close/reimport render, and importer rendering after the source
GEM, source render FD and PRIME descriptor close. No output fence FD is requested.
A counter-bracketed repeat allocates one 4 KiB backing mapping and releases one
4 KiB mapping, with zero quarantines or failed retirements. Existing native
3,000-import/256-close-race and exported-mapping lifetime regressions also pass.

## Steam retry and remaining evidence

Steam acquires its large shared pools, but the initial 200-by-200 browser window
remains unmapped. Later it draws a readable **Unexpected Transport Error** dialog.
Attempting to move that dialog into the X screen is followed by Xwayland SIGSEGV
inside libxshmfence at address `0x28427000`, then abort signal 6. Its stack is
preserved. This is not a usable login or proof that the fence mapping was valid.
The new CEF segment contains no fixed allocator-corruption or EnsureConnected
timeout category in this run; absence alone does not prove shared IPC correctness.

Steam's main exits 1 after display loss. Its exact eight-thread orphan network
service is incarnation-checked and killed before the tracked group drains.
The initially attempted main-stop command fails explicitly because the main has
already exited; this failure remains recorded. The pristine wrapper is restored.
All groups drain, heap and checked sync pass, the original VM owner exits zero,
and final serial acceptance and all four offline filesystem checks pass.

Private controllers, binary capsule and failure evidence remain under
`build/workloads/steam-prime-import-20261024` and review X. No proprietary
payload, raw Steam log, disk image or authentication data is committed.
