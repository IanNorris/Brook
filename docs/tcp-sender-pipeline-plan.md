# Bounded TCP sender work

## Next: socket-owned send queue

`/etc/tcp-async-send` enables an initial per-connection worker experiment;
the default sender remains synchronous. A job owns a 32 KiB queue, a 16 KiB
batch and a socket reference, with at most eight active jobs. Producers copy
into owned storage before returning; each worker drains batches with the
existing ACK-synchronous sender, and drops its reference after detach. A full
queue can return an accepted prefix; an empty queue releases its worker.
Connection epochs reject stale publication after reconnect. Peer reset now
latches `ECONNRESET` for one `SO_ERROR` read. When the worker pool is full,
unattached sockets report non-writable. Poll and epoll register independent,
generation-tagged process waiters, which are notified when a permit becomes
available. The table is bounded to 64 waiters; exhaustion uses the existing
50/100 ms timed rescans. Readiness does not reserve a permit, so `EAGAIN` can
still occur if another sender takes it first. Internet half-close is still
unsupported, and this mode is not ready to become the default.

Hosted tests cover allocation and thread-creation failure, pool saturation,
close before drain, queue backpressure, copy fault after an accepted prefix,
timeout after partial delivery, RST and one-shot error consumption, reconnect
during allocation and after old-job detach, plus one stalled connection while
another progresses. Disposable QEMU tests covered data integrity, concurrent
writers, user-copy faults, one dropped packet and close with a 16 KiB accepted
write. A separate QEMU loopback test fills the receive side and sender queue,
starts an unfinished blocking write, closes the descriptor, and verifies the
two distinct accepted byte ranges followed by EOF after draining. The test
does not prove exactly where the writer was descheduled when close occurred;
internet half-close and prolonged physical-device backpressure remain open.
Hosted tests also exercise pool waiter token reuse, saturation, concurrent
unregister/notification and permit reconsumption. A disposable QEMU test
filled eight loopback receive queues and observed concurrent poll and epoll
calls regain writable readiness when a receiver drained. Their timed rescans
mean this guest test alone does not prove notification-driven wakeup.

In one matched QEMU comparison with 10 ms packet buffering and one dropped
packet, the host received a 1 MiB stream issued as 1,024 separate 1 KiB writes
in 10.33 seconds synchronously and 4.79 seconds asynchronously; another async
run took 5.70 seconds. Timing starts after the first receive and ends on the
last; this is not syscall latency or a Dell/WAN measurement. The matched
run's median 32 KiB receive-tail time was 140.6 ms versus 140.0 ms.

On the physical Dell, matching speaker-muted kernel/initrd candidates differed
only by the async marker. Three host-timed 4 MiB LAN transfers using 1 KiB
`send()` calls took 4.14–4.17 seconds synchronously and 1.39–1.44 seconds
asynchronously; all six hashes matched. These were grouped runs, not an
alternating long-duration trial, and do not establish WAN or browser upload
performance. Two simultaneous async 4 MiB transfers also matched their hashes.
A separate 256 KiB test closed immediately after accepting all writes; the
host received the expected bytes and then EOF. The known-good recovery slot
was preserved and booted afterward. Audio output was disabled throughout.

The faster LAN result did not carry over to a subsequent matched HTTPS test
against the previously used Cloudflare speed-test upload endpoint. Three
1 MiB `curl` uploads on the Dell completed in 1.12–1.16 seconds synchronously
and 2.28–2.43 seconds asynchronously, all reporting HTTP 200. The grouped
runs used the same random payload and kernel, with the async boot flag as the
intended difference; WAN and server conditions were not controlled or
alternated. Aggregate flight counters showed 11–18 local busy observations in
the control and 54–62 in async runs, with zero retransmission attempts. The
sum of sampled ACK-to-resume ticks was also higher in async runs, but samples
can overlap and must not be subtracted from wall time. Transmit contention and
the shared socket waiter are separate leads; these counters do not establish
either as the cause. Keep async disabled by default.

A further default-off experiment shortened only the asynchronous sender's
local `EAGAIN` retry interval from 10 ms to 1 ms. Host tests covered reservation
continuity, retransmission priority and the deadline under persistent local
backpressure; an isolated USB-Ethernet QEMU test passed with delay and one
dropped packet. On the Dell, the same three HTTPS uploads took 2.34–2.50
seconds, with 36–41 aggregate local-busy observations per run. The earlier
10 ms runs took 2.28–2.43 seconds with 54–62 busy observations. These grouped
WAN tests are not an alternating or controlled network comparison, but provide
no evidence that the shorter retry improves this workload. The experimental
option was discarded. ACK waiter ownership was a separate lead.

An opt-in aggregate diagnostic subsequently confirmed that waiter replacement
occurs on the Dell during the same 1 MiB HTTPS upload. Three runs took
2.35–2.45 seconds and recorded 68–69 sender-waiter displacements and 97–171
advancing ACKs after displacement in each run. Counts include concurrent
connections, and the displacement flag stays set for the wait interval; they
do not establish the current waiter at every ACK or quantify the delay caused.
A host test verifies that an application poller can replace a sender waiter
and receive the ACK wake instead. A delayed, lossy QEMU run also exercised the
new counters. The next candidate should give the worker an independent waiter
and compare it under the same workloads; the diagnostic itself changes no
wake behavior.

An opt-in `/etc/tcp-worker-waiter` candidate gives the asynchronous flight
sender a separate, reference-owned waiter. TCP state changes wake both that
waiter and the application waiter. The option requires `/etc/tcp-async-send`
and affects the flight sender; legacy single-packet sending retains its
existing waiter. Host tests exercise an actual worker and competing poller
with diagnostics disabled, ACK delivery, cleanup and stale-worker handling.
Delayed and lossy USB Ethernet QEMU acceptance passed. Astra reviewed the
locking and lifetime approach; the feature remains experimental and off by
default.

On the Dell, three 1 MiB HTTPS uploads with the candidate took 1.17–1.29
seconds, compared with 2.35–2.45 seconds for the earlier shared-waiter runs.
The candidate recorded zero displaced-worker ACKs and 15–27 worker wakeups
at or after the wait deadline per run, versus 106–109 in the earlier runs.
These were grouped WAN runs rather than interleaved trials and include other
connections, so they support the hypothesis but do not establish a stable
throughput figure. The known-good boot was restored after the test.

On a later boot of the same muted Dell candidate, three 4 MiB LAN transfers
using 1 KiB writes completed in 1.29–1.36 seconds with matching hashes. Two
simultaneous 4 MiB transfers also matched, each taking about 1.36 seconds.
In a separate close-after-acceptance case the receiver verified 256 KiB and
EOF. These are bounded checks, not a complete half-close or prolonged
backpressure test. The recovery selection was restored and booted afterward.

The variable-length LAN workload then sent two 32 MiB streams using 1 KiB
writes with the same dedicated-waiter candidate. Each took about 10.60 seconds
(25.3 decimal Mbps), and both receiver hashes matched. This is a sustained
single-stream LAN result, not a WAN measurement or a link-capacity benchmark.
An initial attempt was invalid because its temporary driver treated a queued
guest job as complete and closed the receiver; the same job was observed to
finish with connection refused before the corrected runs started. The checked
recovery boot was restored afterward.

On a subsequent muted boot, four 32 MiB LAN uploads on the same candidate
used 1 KiB, 16 KiB, 64 KiB and 1 KiB writes in that order. Receiver times
were 10.592, 10.752, 10.762 and 10.634 seconds, respectively; every hash
matched. The guest reported 32,768, 2,060, 1,031 and 32,768 send calls.
Aggregate flight counters remained near 3,145–3,150 batches per run. This
single ordered comparison suggests application write count is not the main
limit for this workload, but does not identify the transport bottleneck or
exclude a different application pattern. Recovery was restored afterward.

The flight state remains local to `SockSendImpl`, so every call drains its
unacknowledged data before returning. To let applications hand data off sooner,
the next experiment needs a bounded queue owned by the socket, with a separate
TX worker and the synchronous sender retained as the default. Queue capacity
and congestion-controlled flight capacity are separate: enqueued data does not
reserve TCP sequence space until the worker is ready to submit it. Producers
must copy user data into owned storage before publishing it and return only
the accepted prefix on a later copy fault, signal, or full queue. Nonblocking
writers need `EAGAIN` when no space remains, and write readiness must reflect
queue space and terminal errors rather than merely `Established` state.

The worker must make bounded progress per socket, retain bytes through ACK or
terminal failure, and use an independent wakeup with a timed fallback; the
existing socket waiter is shared with readers and pollers. The NIC callback can
currently block, so an explicitly nonblocking transmit contract is needed
before claiming that one stalled connection cannot hold up a shared worker.
An owner reference must keep the socket alive while accepted data remains.
However, dropping the final owner invokes `SockFinalize`, whose FIN wait can
block for up to 500 ms. The shared worker must defer that release to a bounded
cleanup mechanism or use an incremental finalizer; it must not perform this
wait while serving other sockets. Timeout, reset, close, and slot reuse need
explicit tests, and the existing unsupported Internet `shutdown` semantics
remain unchanged until queue draining and half-close are implemented together.

`TcpSendQueue` provides the bounded byte-storage primitive for that work. It
distinguishes published bytes from the portion copied into flight storage and
only frees a newly acknowledged prefix. The caller must serialize it, publish
flight and TCP sequence reservations together, and validate ACKs against
submitted bytes; the queue alone does not track submissions, errors, workers,
or socket lifetime. Copies into and out of the ring must not overlap it.
Capacity-one, wrap, partial retirement, backpressure and 100,000 randomized
operations against a reference FIFO pass in the host tests. This primitive is
also exercised by the opt-in `/etc/tcp-queue-stage` experiment: the existing
call-local flight stages each copied chunk through a bounded ring, reserves it
in both structures under the socket lock and retires it on validated ACK
progress. This intentionally duplicates flight storage and retains synchronous
completion; it is not a socket-owned queue or a throughput improvement. Hosted
real-sender tests exercise allocation failure, wrap across a larger transfer,
retry payload integrity, partial ACKs, loss, rejected submissions, sequence
wrap and copy faults. The USB Ethernet QEMU test passed with delay, dropped
traffic and concurrent writers. On early terminal return `Finish` may update
its acknowledged count without retiring the call-local ring; safe destruction
there does not establish persistent-queue semantics.

The existing flight step is now a separate pump with explicit retained state;
the caller supplies the observation time and deadline. Its step does not own
a syscall process or user buffer. A `WaitLocked` result transfers the socket
lock to the caller so it can install a waiter before releasing it; all other
results leave the lock released. The caller still owns the state and executes
the ACK-synchronous loop. This preserves the wakeup ordering, but does not
make the pump safe for a shared worker: state mutation needs exclusive
ownership, transmission may block, and completion accounting is call-based.
Hosted sender tests and the USB Ethernet QEMU loss/concurrency test pass with
the extracted step. The actual-code lifetime tests also check that an additional owner
survives application close, acknowledges data before emitting FIN with the
next sequence number, and permits clean socket-slot reuse. This tests a
necessary ownership primitive; it does not demonstrate a queued sender.

The Dell's 4 MiB RAM download to the host takes about 3.6 seconds. Brook's
current sender waits for each 1,200-byte segment to be acknowledged before
sending the next; removing its explicit post-ACK yield did not improve the
measurement. This is a local-path result, not a diagnosis of every WAN upload.

The experimental `/etc/tcp-try-transmit` marker requires the existing async
neighbor and TCP flight markers. It selects an optional immediate driver
callback for both neighbor requests and off-host TCP data. Missing support
fails rather than falling back to a blocking callback. xHCI ECM tries the
event-ring lock once: contention returns `EAGAIN` before publishing anything;
success copies the frame into a driver-owned DMA slot. The ordinary callback
is unchanged. This extends `NetIf` and requires kernel/module ABI 63.
The test remains synchronous and disabled by default. Loopback, local delivery
and TCP control packets still use their existing paths. Bounded software work
does not guarantee bounded MMIO latency, and interface lifetime/configuration
still needs attention before a shared worker uses this interface.

Hosted tests exercise ticket-lock contention, queued tickets and wraparound;
the actual ECM driver path is tested with fake DMA/ring/doorbell memory for
contention, invalid input, controller failure, full slots, success and a ZLP
across ring wrap. The wrap test injects completions directly; it does not test
hardware consumer ordering or completion-event dispatch. QEMU USB Ethernet
passed with the marker, loss and concurrent writers, and also in the default
configuration. An opt-in dedicated kernel-thread probe also passed in QEMU
for successful and busy try-lock operations with IF initially set and clear,
checking that both paths restore the original state. This verifies local
interrupt-state behavior in that VM, not inter-CPU fairness or hardware
latency. This candidate has not been tested on the physical Dell and makes
no throughput claim.

Before adding a two-segment flight, establish single-writer ownership and
sequence reservation using the actual socket lifetime machinery. A persistent
per-slot sleeping send lock must survive payload resets. A sender needs a
borrow and a closing-state recheck after acquiring that lock. Final close must
coordinate FIN with it and release it before waiting for outstanding borrows;
otherwise a queued writer can deadlock close. ACK/RX processing must never
take the send lock. Cancellation protection must cover both unlock and borrow
release, including after lock handoff. `KMutexLock` does not lock outside
process context, so such a path needs explicit treatment. Long waits must not
indefinitely defer termination or close.

The flight must retain immutable sender-owned payload until acknowledged,
reserve sequence space under the socket lock, honor the latest ordered peer
window and allow cumulative and partial ACK progress. A rejected local
submission must retry the same range, not reserve a new one. Timeouts must not
abandon unacknowledged bytes on a live connection. Retransmit the oldest
unacknowledged suffix with bounded backoff and reduce flight on loss. Zero
window, reopening and concurrent receive must make bounded progress despite
the existing single poll-waiter slot. These are prerequisites, not a complete
congestion-control design.

Tests should exercise real send, close and receive code: simultaneous writers,
close and cancellation across lock handoff, slot reuse, synchronous loopback
ACKs, two packets before the first ACK, partial/cumulative ACKs, EAGAIN on
either submission, loss/retransmission, sequence wrap, window shrink and
reopening, RST, FIN and full-duplex waits. Use the real mutex and cooperative
scheduler for the lock-lifetime cases. QEMU packet capture and exact echo
verify the flight before the same isolated physical benchmark. Keep receive
depth, receive-window policy and other knobs fixed for comparison.

As a first safety checkpoint, an ACK timeout now atomically closes the local
stream instead of allowing later writes to proceed beyond data whose
retransmission copy has been discarded. An ACK observed at the deadline wins;
earlier fully acknowledged chunks retain their existing partial-write return.
Buffered incoming data remains readable. The local abort uses the existing
reset indication, so later reads report `ECONNRESET`, while `SO_ERROR` reports
`ETIMEDOUT` once. The abort does not currently transmit an RST; the peer may
retain its half of the connection until its own timeout. Socket lifetime and
concurrent send serialization remain separate work. Host tests exercise the
actual send and timeout paths, and QEMU USB Ethernet acceptance passed.

The sender now retains a temporary socket owner reference and uses a
slot-persistent send mutex. A close cannot finalize the stream until active
and queued sends drop their references; the gate is released before the last
reference, with an outer cancellation region covering both operations. Gate
acquisition polls at 10 ms intervals and the entire call is bounded to 30
seconds. Contended nonblocking acquisition returns `EAGAIN` before reserving
sequence space. Uncontended nonblocking sends still use the existing ACK wait.
Cancellation during an outstanding ACK wait can be deferred until the bounded
deadline. Only sleepable process-context callers are supported; the current
process check is not an IRQ-context detector.

Host tests exercise the actual KMutex and actual send/close paths with modeled
scheduler transitions, including concurrent writers, a queued sender at close,
contended nonblocking acquisition and pending signals. QEMU USB Ethernet tests
add guest pthread writer integrity and concurrent close stress; the latter does
not by itself prove precise overlap. Real scheduler termination during
contention, the send pipeline, and physical performance remain unverified.

TCP sends now snapshot each MSS into bounded kernel storage before reserving
its sequence range, then reuse that snapshot for retries. Direct user buffers
are copied through the fault-aware user-copy path; trusted kernel buffers used
by `writev` and other internal callers remain explicitly separate. TCP
multi-iovec `sendmsg` snapshots its vector and payload and uses the retained
descriptor. QEMU tests cover `send`, `writev`, multi-iovec `sendmsg`, a
cross-page first-chunk fault, a later-chunk fault with partial progress, and
continued stream integrity. A hosted retry test mutates the source after the
first send and confirms that the retransmission keeps the original byte.
UDP sendmsg identity and generic writev metadata races are not addressed.

An experimental, boot-opt-in two-packet flight is enabled by
`/etc/tcp-two-packet-flight`; the default remains the single-packet sender.
It retains both payloads locally, limits new reservations to peer-window
credit, retries local `EAGAIN` without reserving a new range, and retransmits
the oldest missing suffix with a one-packet limit after RTO. It aborts rather
than abandoning outstanding data on interruption or deadline, and drains
previously submitted data after a later user-copy fault. It rechecks waiters
periodically because receive and send share one waiter slot. A zero window
waits only until the bounded operation deadline; TCP persist probing is not
implemented. An unsubmitted slot is retried only when its entire length fits
the reopened window. A fixed two-packet limit is not congestion control.

Hosted actual-code tests cover cumulative and partial ACKs, wraparound,
local submission failures, rejected-unsent ACKs, shrinking/reopening windows,
loss and oldest-suffix retransmission, copy faults and interruption with data
outstanding. QEMU USB Ethernet acceptance exercises concurrent writers,
close stress and user-copy faults, and its packet capture confirms successive
payloads before their ACK. Physical throughput and wider network behavior are
not yet verified; keep the known-good SSD recovery slot available.

A separate `/etc/tcp-four-packet-flight` marker selects an experimental
four-entry version of the same queue; conflicting markers select two. It is
not a congestion-control policy or a default. Host tests exercise every
local-EAGAIN position, ring reuse, a short last packet and recovery from a
lost middle packet. Isolated QEMU tests with a 10 ms periodic network buffer
captured maximum contiguous flights of 1, 2 and 4 for the corresponding modes
while preserving the tested bytes. Capture is placed on the guest side of
the delay; the parser proves an observed chain before covering ACKs, not a
general upper bound or a throughput result. The buffer batches rather than
guaranteeing per-packet delay. Persistent congestion state, real-network
loss behavior and physical four-packet performance remain to be tested.

The Dell's sixteen-slot run still observed many call-tail drains. Removing
them would require a persistent bounded per-socket queue: an application may
only return successfully after its bytes are owned by the kernel, and those
bytes must remain available for retransmission until ACK or terminal failure.
Queue capacity, nonblocking and poll readiness, asynchronous error reporting,
last-close/FIN ordering, worker lifetime and global memory limits need to be
designed together. The current call-local flight cannot safely return early.
First moving retained flight state into a worker while keeping synchronous
returns would permit testing lifetime and loss handling before opting in to
buffered returns. These changes are not implemented by the flight experiment.

Astra's follow-up review identified a specific close constraint: a callback
borrow cannot keep queued writes ahead of FIN, because `SockFinalize` reserves
FIN before it drains callback borrows. A nonempty asynchronous queue would
need an owner reference held until ACK retirement or terminal abort; dropping
that reference must happen outside locks in process context. An idle queue
must not keep the socket alive indefinitely. The transport worker also needs
bounded per-connection pumping, separate wakeup state, queue-aware writable
readiness and a latched asynchronous error. `shutdown(SHUT_WR)` requires its
own FIN ordering audit. The first implementation checkpoint should extract a
caller-driven bounded pump while retaining synchronous returns, followed by
worker-driven synchronous testing; neither checkpoint alone improves the
per-call drain. Tests must include queue-reference versus last-close races,
slot reuse, stalled peers, worker shutdown, readiness and failure after bytes
have been accepted from an application.

The flight's slots, allocation and progress counters now have an explicit
call-local owner. Allocation is one-shot and rejects unsupported flight
limits. The sender remains synchronous; no persistent queue, worker or
application-visible buffering is introduced by this extraction. The hosted
socket-lifetime suite passed with `BROOK_HOST_TEST` enabled, including the
writev boundary and fault cases; the QEMU USB Ethernet suite passed with the
16-slot experiment, delayed traffic, one controlled drop, concurrent writers,
close stress and user-copy faults. Astra reviewed the extraction. This is a
preparatory step toward the bounded pump, not a throughput result.

The caller-driven flight loop now separates a single transport action from
the outer wait loop, while keeping the same synchronous completion and
call-local lifetime. Each action stages or submits at most one packet, but it
is not bounded in wall time: user-memory copying and neighbor resolution can
sleep. It also still captures caller-owned context. Before a shared worker can
drive it, those blocking operations and all captured lifetimes must be
separated. The hosted socket-lifetime suite and the delayed, loss-injected
16-slot QEMU USB Ethernet suite passed; Astra reviewed the control-flow
extraction. This does not yet change upload performance.

User-source copying is now a separate action carried out by the same syscall
caller, outside the flight action and socket lock. Sequence reservation still
follows the copy and rechecks current connection state, peer window and
congestion credit. Hosted tests inject a window shrink during copying and
cover both partial credit and a zero window followed by reopening; the delayed,
loss-injected QEMU USB Ethernet suite also passed. This separation depends on
single-caller ownership of the flight's tail and source offset. Before a
concurrent worker is introduced, a pending copy needs an explicit owned slot
and generation-checked completion, and neighbor resolution must cease blocking
the shared transport path. No asynchronous writes are enabled.

IPv4 frame construction and submission for an already selected interface and
MAC are now separated from route selection and blocking ARP resolution. The
existing caller still performs ARP first, so this does not yet provide
nonblocking resolution or alter traffic behavior. The helper requires a live
interface and retains the existing synchronous driver transmit contract; it
is not certified as a shared-worker operation. The review also identified an
existing length-addition overflow in both NIC and loopback frame builders;
both now reject payloads exceeding the Ethernet frame budget before adding
header sizes. Hosted boundary checks cover 1,480, 1,481 and `UINT32_MAX`;
the USB Ethernet QEMU loss and concurrency suite passed with normal traffic.
Actual ARP miss scheduling, interface identity, retry and driver-lifetime
behavior still need their own design and tests.

The hosted socket suite now calls the actual resolved-frame builder with a
capturing NIC. It checks Ethernet addresses, IPv4 fields and checksum, a
maximum-sized payload, rejection of oversized and overflowing lengths before
transmit or ID/counter changes, and propagation of a busy NIC result. The
ordinary hosted sender path still uses its separate transmit stub. This does
not establish driver DMA lifetime or nonblocking behavior.

ARP request construction now accepts the already selected interface and
returns its transmit result. The existing blocking resolver keeps its old
interface selection and still ignores that result. Hosted tests check the
actual request frame, null-interface failure and propagation of NIC busy; the
USB Ethernet QEMU loss/concurrency suite passed. This enables a future tracker
to distinguish a submitted request from local backpressure, but does not yet
coordinate asynchronous resolution.

Astra's proposed tracker is a fixed, bounded table of neighbor requests with
generation-checked completion, an absolute deadline and separate retry delays
for a submitted unanswered request versus a busy NIC. It should deduplicate
requests without holding its lock across transmit, never reuse the existing
global ARP reply waiter, and allow unrelated neighbors to progress. The
current ARP cache is keyed only by IP and cannot insert when full, so an
initial asynchronous path needs either safe cache changes or a clearly
restricted single-interface admission and explicit capacity failure. Route
changes, synchronous replies during transmit, table exhaustion and stale
completions require actual-path tests before the tracker drives TCP.

`ArpProbeTracker` implements the bounded request state: it
deduplicates by caller-supplied interface lifetime and IP, separates busy
retries from submitted attempts, imposes an absolute deadline, uses
round-robin dispatch, and rejects stale completion tickets. Its caller must
serialize operations, provide monotonic millisecond ticks and pin interface
lifetime during transmit. Hosted tests exercise those transitions alongside the
actual ARP request-frame builder, including table exhaustion, NIC busy,
expiry and late completion. Astra reviewed the tracker. It was initially
host-only; the opt-in diagnostic integration described below does not drive
TCP or replace the legacy resolver.

The existing ARP cache lookup now exposes a locked three-way result: found,
missing with space, or missing while full. The legacy resolver still treats
both kinds of miss alike. Hosted tests fill the actual cache, verify full
reporting and confirm that an existing entry can still be updated; the USB
Ethernet QEMU loss/concurrency suite passed. Capacity is only a snapshot: a
future asynchronous resolver must recheck after a reply because another
insertion can consume the last slot. This does not yet add cache eviction or
connect the tracker to networking.

The network stack now maintains a configuration generation under its
interface-view lock, advancing it on interface registration and DHCP/static
configuration. A single-interface snapshot includes the generation, selected
interface and copied addressing information; it fails closed for multiple or
unconfigured interfaces and clears its output on failure. Exhaustion disables
new snapshots instead of wrapping. `bridge network` reports the generation, or
zero if no single-interface snapshot is available. Hosted tests cover
change-away-and-back, multiple interfaces and exhaustion; the QEMU USB
Ethernet regression confirmed a nonzero generation after DHCP. Astra reviewed
the configuration-writer inventory and the cache-capacity extraction. This
token does not pin driver storage, serialize transmit with configuration
changes, or make old IP-only ARP entries valid for the current generation.
`NetInit` remains a boot-only initialization.

The tracker retains bounded, expiring resolved MAC values keyed
by the caller's configuration token and IP, separate from the legacy ARP
cache. It accepts a result only for a pending request after submission starts,
rejects invalid unicast encodings, protects a synchronous reply from a late
completion, and evicts resolved values before active requests. Hosted tests
cover separate configuration tokens, expiry, reply timing and earliest-expiry
replacement. This is not packet validation or authenticated ARP: integration
must verify the received frame, selected interface and current generation,
and revalidate before use. A delayed or forged reply on the same configuration
cannot be distinguished cryptographically.

A separate reply validator now checks the Ethernet and ARP format, sender
and target IPs, local destination/target MAC, agreement between sender MAC
fields and a nonzero unicast sender MAC. Its strict diagnostic policy rejects
broadcast-destination replies. The hosted test covers valid padded and
unaligned packets, truncation, malformed fields and mismatched addresses. Astra
reviewed it. These checks are not authentication and cannot prove a matching
reply is fresh.

An isolated diagnostic path now combines the tracker and validator when the
boot marker `/etc/arp-async-probe` is present. `bridge arp-probe <IPv4>`
queries an on-link unicast neighbor and drives at most one due request per
invocation. It accepts a validated reply only against an outstanding request
and a matching single-interface configuration generation; it does not consume
the legacy IP-only cache or drive TCP. QEMU USB Ethernet tests resolved the
virtual gateway, rejected an off-link target, timed out an unanswered target,
and kept the bridge responsive; a second boot verified the command is disabled
without the marker. Hosted actual-path tests cover a synchronous reply during
transmit, NIC busy/retry/expiry and configuration change during transmit.
Astra reviewed the integration. The response is not linearized against a
configuration change after its snapshot, and one request can still be sent
with copied old configuration if that change races the last check. NIC
callbacks are not generally guaranteed nonblocking. Rejected diagnostic
replies still pass through the unchanged legacy ARP-learning path. This is a
bounded diagnostic, not a production resolver or a throughput improvement.

The exact speaker-disabled Dell diagnostic kernel and initrd also booted in
isolated QEMU before physical deployment. On the Dell, the opt-in probe
resolved the local host in two queries; its returned MAC matched the host
interface selected by the host route to the Dell. An off-link address was
rejected. The hardware runtime reported physical speaker output disabled.
The verified recovery payload was selected as generation 118 and booted
afterward. This is one successful local neighbor lookup, not a test of
hardware timeout, configuration races, TCP integration or throughput.

An additional `/etc/tcp-async-neighbor` marker, together with
`/etc/arp-async-probe` and an enabled TCP flight mode, lets the synchronous
TCP data-flight path use this isolated tracker. While a neighbor is pending,
the flight retains its unsent sequence range and retries it; a successful
write still waits for acknowledgement. Control packets use the existing path.
The experiment only admits a single interface, validates its source address,
and resolves the gateway for off-link traffic. Loopback keeps its existing
path. Interface configuration can still change after the final check, and a
device transmit callback can block: this is not a nonblocking sender or a
production neighbor service.

Hosted tests cover routing, pending resolution followed by acknowledged data,
payload preservation, and resolution timeout without data submission. The
USB Ethernet QEMU test passed with delay, packet loss and concurrent writers.
On the Dell, six 4 MiB LAN transfers completed with matching hashes in
0.257–0.285 seconds each, comparable to the prior local test; this does not
show a throughput improvement or establish behavior under arbitrary traffic.
The speaker-disabled candidate was tested and the verified recovery generation
was selected afterward.

An independently opt-in `/etc/tcp-adaptive-flight` mode retains congestion
state per connection, starts at two MSS and grows on newly acknowledged bytes
to at most eight MSS. Retransmission timeout reduces the window to one MSS;
the reduction is applied once until forward ACK progress. It respects the
peer window and rechecks an unsubmitted tail against the reduced allowance.
Conflicting fixed-flight markers take precedence. This is a bounded experiment,
not full TCP congestion control: idle restart and zero-window persist are not
implemented. Socket layout changed, so kernel and modules must use ABI 62.

Hosted tests cover window growth, ACK grouping, loss, delayed submission and
connection reuse. An actual-sender host test also drops the second packet of a
four-packet adaptive flight, verifies retransmission of the missing range and
completion of the retained later bytes, and observes the reduced window. The
USB Ethernet QEMU acceptance passed with a delayed
packet queue and observed eight contiguous packets before their covering ACK;
this does not establish behavior under arbitrary real-network congestion.
An optional private QEMU packet relay also drops one guest-to-host TCP payload
and confirms that a subsequent packet has the same flow, sequence, length and
payload hash. The delayed-ACK USB Ethernet run passed this loss check along
with the 256 KiB echo, concurrent-send, close-stress and user-copy checks. Its
packet capture records the guest's transmission attempt, not proof that the
dropped packet reached the backend; the echo proves the test stream survived.
This single controlled drop does not measure general loss handling or fairness.

For investigation only, `/etc/tcp-send-probe` enables aggregate counters
queried with `bridge tcp send-probe`. They include send-call size buckets,
submitted and acknowledged bytes, maximum observed outstanding bytes,
retransmission attempts, local busy returns and wait observations for slot,
congestion and peer-window limits or call-tail draining. Counts include all
flight-enabled connections, including bridge traffic. Wait counters count
observations, not elapsed time, and independently read fields are not an
atomic snapshot. ACK timing samples exclude retransmitted slots and measure
to the latest advancing ACK observed when the sender resumes; multiple ACKs
can therefore inflate the latency and understate resumption delay. They are
not precise RTT samples. Ticks use the guest clock. Diagnostics are disabled
by default and are not intended as a permanent congestion-control algorithm.
Host tests cover full-flight waits, ACK batching and retransmission exclusion;
the opt-in QEMU loss and concurrent-send run also validates bridge output.

A separate `/etc/tcp-adaptive-flight16` experiment raises the adaptive cap
to 16 MSS while retaining the two-MSS initial window. It allocates a bounded
16-slot flight before reserving sequence space; allocation failure returns
`ENOMEM`. The ordinary eight-slot path keeps its stack allocation. Conflicting
markers retain the earlier selected mode. This does not alter call-tail
draining or implement full TCP congestion control. Host tests cover growth,
ring reuse, allocation failure, loss and EAGAIN, wrap, invalid ACK, user-copy
failure, partial ACK, zero window and interruption. The QEMU USB Ethernet
loss/concurrency test passed and observed a 16-packet flight. That is a
controlled guest result, not evidence of physical throughput or fairness.
The release build's eight-slot send function reserves 12,424 stack bytes;
its caller reserves 1,592, against a 256 KiB kernel stack. These figures are
individual compiler-generated frames, not a complete worst-case stack bound.
