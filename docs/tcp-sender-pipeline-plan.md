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
