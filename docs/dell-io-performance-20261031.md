# Dell SSD and network performance, 2026-10-31

After repairing the ext2 desktop store, the installed SSD payload booted and
admitted the persistent home. The pre-existing ordinary-user persistence
fixture still verified, including its expected bytes and backing-alias guard.
These observations do not explain the earlier Firefox-associated hang.

On the running Dell, a 4 MiB random payload was transferred via the binary
helper into RAM and back, with digest verification. The binary phase took
6.10 s to upload (0.69 MB/s) and 3.67 s to download (1.14 MB/s). Since
neither transfer used the SSD for its payload, SSD throughput cannot explain
the measured network ceiling. These are single measurements, not sustained
throughput guarantees.

On a subsequent SSD-only boot after controller recovery, another random 4 MiB
RAM-to-RAM sample took 5.037 s host-to-Dell and 3.636 s Dell-to-host in the
binary phase (about 0.83 and 1.15 MB/s respectively). The returned SHA-256
matched. These single measurements include helper and hashing overhead; they
do not isolate TCP or establish steady-state link capacity. Receipts are under
`build/hardware/network-ram-baseline-20261101-new-upload` and
`build/hardware/network-ram-baseline-20261101-new-download`.

Host TCP sampling during a repeated unpaced 4 MiB upload showed hundreds of
retransmissions and over 600 KiB retransmitted before completion, while the
peer's advertised receive window remained open. The same RAM-only upload with
Linux host pacing caps of 4, 8 and 16 MB/s took 1.11, 0.60 and 0.34 seconds;
a repeat at 16 MB/s took 0.59 seconds. A 32 MB/s cap took 1.55 seconds and
the sampled connection showed retransmissions. All transfers verified their
payload hashes. These are short single runs with one repeat, not a measured
link capacity. Pacing reduces burst-related loss in these observations but
does not establish whether loss occurs in the adapter, driver, TCP processing
or another part of the path.

`scripts/hardware_transfer.py` accepts `--pace-bytes-per-second 16000000` for
an explicitly paced Linux host-to-guest upload. It remains opt-in and applies
only to this helper's upload socket; it does not improve general downloads or
browser traffic. Evidence includes `network-window-profile-ss-20261101.txt`
and the `network-pace*m*-20261101` receipts and sampled host socket counters
under `build/hardware/`.

An experimental xHCI ECM receive path keeps up to eight independently owned
DMA requests in flight while reserving space in the 32-frame software queue.
`/etc/usb-net-rx8` opts into depth eight; without it the depth remains one.
Both modes use the new serialized dispatch path and larger DMA allocation.
The final private QEMU candidates for both depths acquired DHCP and resolved
gateway ARP during early boot, passed a hash-verified 4 MiB upload, and shut
down cleanly. An earlier eight-buffer candidate also passed a 4 MiB roundtrip.
Evidence: `ecm-rx1-qemu-20261101`, `ecm-rx8-qemu-v3-20261101`, and their
upload receipts under `build/hardware/`. The first experimental build delayed
DHCP until after scheduler startup; the final candidates correct that.

This is not yet a physical performance result. Before Dell deployment, test
full-credit and malformed completion handling, wrap/interleaving, recursive
dispatch, process termination while dispatching, and disconnect/fatal events
with outstanding buffers. Compare depth one and eight using the same physical
payload, controlled transfers and retransmission counters; pacing results alone
do not prove this queue was the location of the loss.

The receive-credit bookkeeping is now shared with a host test covering full
reservations, queue drain, malformed queue counts, duplicate/unknown completion
tokens and 1,024 cycles of slot reuse. It does not simulate hardware Link TRBs,
interrupt interleavings or a duplicate after a token address has been reused.
The refactored opt-in driver also passed early DHCP, ARP, a 4 MiB verified
upload and clean shutdown in `ecm-rx8-qemu-v4-20261101`.

The shared ECM ring producer now has a deterministic consumer-interleaving
test: over 30 wraps, the consumer follows a newly published Link before the
next Normal TRB is prepared and stops on its old cycle, then observes it only
after publication. It also checks TX immediate publication and a data/ZLP pair
spanning a Link. The depth-eight QEMU TCP/UDP/storage/removal workload passed
again after the extraction (`ecm-rx8-ring-qemu-20261101`). This is not a
concurrent hardware memory-ordering proof or a fatal-completion test.

The production receive-credit validator now has host coverage for fatal and
unknown completion codes, unknown tokens, impossible residue and zero-byte
short completions. Invalid completions leave the outstanding reservations
unchanged; the surrounding driver records failure and stops rearming. This
does not establish that DMA has stopped or test a physical disconnect with
known outstanding requests. The depth-eight QEMU integration workload passed
again after this extraction (`ecm-rx8-completion-qemu-20261101`).

`scripts/test_usb_ecm.py --rx-depth8` now exercises the opt-in path with
TCP and UDP byte checks, exact 64- and 1,024-byte Ethernet frames, an idle
interval, concurrent USB storage, and emulated NIC removal followed by USB
storage and keyboard activity. Both depth eight and the default depth one
passed (`ecm-rx8-fault-qemu-20261101` and `ecm-rx1-fault-qemu-20261101`). This
does not prove the number of requests outstanding at removal, inject fatal
completion codes, or deterministically schedule ring-wrap races.

A read-and-SHA256 pass over the installed 230,212,616-byte Firefox libxul
file took 1.886 s, followed by 1.897 s for a second pass, about 122 MB/s of
file bytes per second including hashing. Cache state was not controlled and
the test does not measure cold SSD throughput or write performance.

The TCP sender currently waits for an acknowledgement after each segment and
explicitly yields before sending the next. This boot also opts into a
1200-byte TCP payload ceiling. A CPU sample taken around a RAM download was
predominantly idle; it does not resolve individual device or scheduler waits.
The sender structure is a concrete limit worth measuring against a bounded,
tested multi-segment sender. Do not infer that a faster sender alone fixes the
entire transfer path: ACK validation, peer-window handling, concurrent writers,
loss recovery and zero-window behavior are prerequisites.

A first prerequisite rejects active-open SYN-ACKs that do not acknowledge the
sent SYN; rejected handshakes also cannot set MSS, scale or the peer window.
Host state-machine tests cover incorrect acknowledgments, wraparound and a
SYN-ACK carrying RST; the kernel builds. This is not a windowed sender and has
not been deployed to the Dell. Receive-path integration tests remain necessary,
as do ordered established-window updates, correct scale negotiation and sender
serialization before using the peer window to permit multiple segments.

The next candidate advertises unscaled windows in SYN segments and disables
both window scales when an accepted active-open SYN-ACK omits scaling. It also
publishes the debug channel's SYN sequence before sending, matching the normal
active-open path. Host tests cover scale absence, bounds and option parsing;
the kernel builds. Actual receive-path and wire tests are still needed before
physical deployment. Ordered updates and sender serialization remain open.

In a private QEMU run of this candidate, an active-open curl request downloaded
4 MiB from a loopback-only host server and exited successfully; the guest then
completed checked shutdown. Packet capture shows the guest SYN advertising a
65,535-byte window and scale 3, a SYN-ACK without a scale option, and the
guest's subsequent ACK advertising an unscaled 65,535-byte window. Evidence:
`build/hardware/tcp-window-qemu-v3-20261101/network.pcap` and `result.json`.
This covers an ordinary no-scale handshake, not injected invalid SYN-ACKs or
the full receive-path matrix; it does not establish any throughput improvement.

A private-QEMU comparison used the same kernel with an experimental boot-only
switch to remove the post-ACK yield. A single 2,056,443-byte roundtrip took
0.168 s upload and 0.303 s download with the existing yield, versus 0.201 s
and 0.385 s without it. Both payload hashes matched. This small, single-run
QEMU sample does not quantify Dell behavior, but provides no evidence to
retain the change; the experiment was discarded.

A random 8 MiB sample from the previously received portion of the SSD store
compressed to 35.4% with zlib level 1 on the host, at about 66 MB/s of host
CPU time. This is not a full-image or Dell CPU measurement. Optional bounded
chunk compression is useful for tools, screenshots and bulk data, with
integrity checked over the uncompressed bytes; it should be benchmarked
separately from the underlying transport. The portable installer should run
on the laptop against the identity-bound SSD rather than requiring a host to
write a physically connected laptop drive.

On 1 November, the user ran Google's speed test in Chromium on the SSD boot;
it reported less than 0.5 Mbps in each direction. This is an external
end-to-end estimate, not a controlled measurement of the Ethernet link alone.
The same session could load and play a YouTube video. A later 2 MiB synthetic,
highly repetitive diagnostic sample transferred from Dell RAM in 1.783 s raw
and 0.086 s with independent zlib chunks; both SHA-256 digests matched. The
host-to-Dell upload took 3.646 s. The compression comparison is a single
best-case diagnostic-data sample, not a browser network improvement or a
claim about throughput for already-compressed data.

On the same date, a Dell curl request for the 19,559-byte local panic-scanner
page completed over HTTPS in 0.059 s (including 0.054 s TLS setup). A single
256 KiB HTTPS download from Cloudflare's speed-test endpoint completed on the
Dell in 0.215 s, approximately 9.8 Mbps including connection time; a separate
host request took 0.399 s. The host and Dell may reach different servers or
use different network paths. These short samples do not measure peak or
sustained bandwidth, but show that the Google browser-test result is not a
universal limit on every download. An attempted request to a temporary host
port timed out before connection, so it yielded no LAN throughput measurement.

A follow-up single 4 MiB request to the same public endpoint completed in
4.677 s on the Dell (about 7.2 Mbps including connection time) and 0.780 s
on the host (about 43 Mbps). This longer sample establishes a gap for this
request, not a controlled attribution: endpoint routing, caching and client
behavior may differ. The Dell measurement is still much higher than the
user's Chromium/Google speed-test estimate.
