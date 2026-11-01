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
