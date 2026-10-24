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
The exact library is retained from the checked offline Nix disk. Its matching
instruction at ELF offset `0x1397` is `xshmfence_trigger+7`, an atomic
`lock cmpxchg` through RDI. The next diagnosis must distinguish a removed or
unwritable four-byte fence mapping from an invalid application-side lifetime;
the stack alone does not decide which occurred.
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

## Fence crash narrowed to a shortened device mapping

A subsequent fresh run instruments mapping calls and fence triggers in Xwayland.
The private diagnostic passes a Linux smoke test for four-byte fence mapping,
trigger, protection changes, same-address remap, unmap and address reuse. It
reports tracking loss explicitly. Trigger records are written before entering
libxshmfence; their placeholder result is not evidence of a successful return.
Brook's current `/proc/self/maps` omits mmap regions, so absence there is not
used as evidence of an unmapped fence.

The native failure has a decisive sequence: fence mapping 9 at `0x2862c000`
is created read/write and triggered several times. A later successful
`munmap(0x28456000, 1996800)` covers that fence, without an explicit fence-unmap
call. The next trigger faults at precisely that address. The kernel page-table
walk confirms the page is absent. This distinguishes the observed failure from
a write-protection fault.

The standalone `gpu_mapping_extent_check.py --brook-guest` reproduces the
underlying reservation failure without Steam. For an 800-by-600 texture with a
1,996,800-byte client allocation, the old kernel substitutes 1,920,000 bytes.
It then silently shortens a successful mmap to that backing size. An immediately
following anonymous mapping lies inside the original requested extent. A later
munmap using the original length can therefore remove unrelated mappings.
The explicit-stride control retains its size, but oversized device mappings are
silently shortened there too.

The repair preserves the larger client allocation, retaining the existing
base-level minimum for placeholder sizes and checking arithmetic before
narrowing. Device mappings reject requests larger than the page-rounded backing
instead of returning success for a shorter range. A host layout regression and
the native extent probe cover padded allocation, explicit stride, placeholder
size, mapping separation, tail access and oversize rejection. The fresh repaired
native kernel passes all 15 extent-probe checks; all 104 host tests and both
compiled entry/scheduler audits pass. The isolated GLX readback still passes,
but the red stripe remains, so this fix does not resolve all presentation
corruption. In the repaired Steam retry, Xwayland survives moving the transport
dialog into view and selecting **Continue Anyway**. The final diagnostic has
11 fence maps, 11 fence unmaps, 27 trigger entries, and no tracking-loss entry.
Steam still reports startup error `0x3000` and does not reach login. Thus the
mapping repair resolves the demonstrated overlapping-unmap hazard, without
claiming that Steam startup or all Xwayland rendering is fixed.

The repaired run drains every tracked group, restores its wrapper, and passes
heap integrity, checked sync, final serial acceptance, original QEMU-owner exit
and all four offline filesystem checks. The final source also releases the
framebuffer reference on its new oversized-mapping error branch; that small
cleanup is reviewed separately from the first native candidate.

The failed run's owned jobs drain and its wrapper is restored. Heap and checked
sync pass, QEMU's original owner returns zero, and all four offline filesystem
checks pass. The controller's final serial acceptance nevertheless fails on an
`exit_group` timeout for thread 124; this remains separate failure evidence.

## Next transport diagnosis

Astra's source review identifies a concrete INET receive gap: the `recvmsg`
fallback calls the UDP datagram receiver even for TCP. A small localhost control
passes on Linux with eight correct bytes, while Brook returns one incorrect
byte for both a single receive buffer and two scatter buffers. This is a
verified compatibility defect, not yet an attribution of Steam's startup error.
Connected Unix-socket peer-name queries are another verified gap: socketpair,
client and accepted endpoints return `ENOTCONN` on Brook, while the same Linux
control returns their proper names.

The attempted broader Steam network trace is disabled after collection. Its
output interleaves syscall entry and return text between threads, so even a
line with one prefix can contain another call's return. It is unsuitable for
reliable per-call correlation. The next application probe needs bounded,
atomic records of socket kind, endpoint category, operation, flags, byte counts
and error, without exporting payloads. Local and remote transport must be
distinguished before choosing which compatibility defect to pursue.

## Native socket repairs

A bounded userspace probe confirms both gaps in the real client. On the old
kernel, a successful Unix connect is immediately followed by `ENOTCONN` from
`getpeername`. Separately, accepted IPv4 TCP descriptors reach `recvmsg` and
the UDP decoder returns bogus stream lengths. The probe records only numeric
socket metadata, never message contents.

Unix endpoints now retain a snapshot of their peer's bound address at connect
and accept. Unnamed peers report the address family alone, and names survive
unlink, listener close and peer close without retaining the peer object.
TCP `recvmsg` stages a stream read and scatters it after releasing the socket
lock. It honors nonblocking reads and rejects unsupported flags explicitly.
The receive loop rechecks readiness under its lock, so competing readers do
not turn a stolen readiness indication into a premature timeout. UDP decoding
also rejects stream sockets defensively.

The same final candidate passes 21 native Unix-name cases and 17 TCP/UDP cases,
including invalid-pointer behavior, name truncation and lifetime, scatter
reads, EOF, competing blocking readers, and a complete 128 KiB transfer.
Both controls pass on Linux. All 104 host tests and the compiled entry and
scheduler audits pass; the production kernel builds with all 13 module imports
resolved. The candidate kernel SHA-256 is
`d9522fe5a9e89435d4739c41688a74aff6e04a84c77601805f9fa126dde0c898`.

In the repaired Steam retry, the sampled main process records 554 successful
Unix peer-name queries. Accepted loopback TCP reads now return 544-byte chunks
where the prior decoder returned 1,024 bytes. Steam reaches its large shared
pool handoff, but still displays **Unexpected Transport Error**; login is not
verified. One persistent-memory allocator warning remains in this run, without
establishing its cause. The isolated red-bar presentation issue also remains
open. These repairs resolve demonstrated socket defects, not Steam acceptance.
