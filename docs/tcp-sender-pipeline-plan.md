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
