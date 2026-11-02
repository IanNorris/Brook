# Bounded TCP sender work

The Dell's 4 MiB RAM download to the host takes about 3.6 seconds. Brook's
current sender waits for each 1,200-byte segment to be acknowledged before
sending the next; removing its explicit post-ACK yield did not improve the
measurement. This is a local-path result, not a diagnosis of every WAN upload.

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
