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

An alternating same-boot repeat (three runs each, 4 MiB random RAM source)
produced unpaced times of 8.515, 6.194 and 7.790 seconds; interleaved runs
with a requested 16 MB/s cap took 0.339, 1.038 and 0.345 seconds. Each guest
receipt reported the expected SHA-256. These are helper binary-phase times,
not direct wire-speed measurements. Evidence:
`build/hardware/net-alternating-20261101-results.json` and the six associated
receipts. The current Dell kernel was unchanged during this comparison.

`scripts/benchmark_hardware_network.py` now runs that comparison with a caller-
provided stable random file, alternating unpaced and paced uploads and a
verified download. It retains boot identity and each transaction receipt. A
two-round physical smoke completed with unpaced times 4.889 and 8.369 seconds
and paced times 0.583 and 0.342 seconds; the download verified the same hash
(`network-benchmark-script-20261101`). Timings include the helper, and this
tool does not identify the packet-loss location or replace network-device
counters. It takes a bounded private source snapshot, alternates mode order,
and records the boot ID, build ID and RX depth when available in retained
logs. Use the same source and settings when comparing boot candidates. The
previously booted Dell image predates the RX-depth log and records null rather
than inferring a value. Five rounds of 8 MiB leave up to 80 MiB of upload
artifacts in guest RAM until reboot; the script does not remove them.

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

An SSD-specific depth-eight candidate was prepared from the accepted physical
initrd with a matched new kernel and xHCI module, an explicit marker and a
distinct payload ID. The Dell's identity-bound system updater received the
pair in staging-only mode and reported `storage_modified=false`
(`dell-ecm-rx8-staging-20261101/receipt.json`). At that point it had not been
activated or booted. The previous SSD generation and root recovery remained unchanged. The
two-slot selector chooses the newest hash-valid generation; it does not
automatically roll back a kernel that hangs, so recovery availability must be
established before activation.

The SSD-specific RX8 candidate was subsequently activated as generation 9 in
slot A; generation 8 remains in the other slot and root recovery was not
changed. The boot-pinned updater observed a new boot and the expected payload
ID (`dell-ecm-rx8-physical-update-20261101/receipt.json`). Ethernet and the
persistent store came up, the RX8 marker read back from the running initrd,
and the existing SSD persistence fixture passed. The QEMU-tested candidate
contained the RX8 module and marker; the physical kernel log did not expose
the driver's serial-only depth message, so this is indirect rather than an
independent runtime observation of the configured depth.

On that boot, three unpaced 4 MiB RAM uploads took 2.276, 3.516 and 2.356
seconds, and paced uploads took 0.605, 1.556 and 0.345 seconds. A verified
download took 3.635 seconds (`network-benchmark-rx8-physical-20261101`). The
earlier unpaced control was 6.194–8.515 seconds, but used a different kernel
and boot, so this is encouraging, not an isolated RX-depth result. Preserve
generation 8; installing a second candidate in its slot would overwrite the
known-good fallback. A checked warm restart subsequently returned on a new
boot ID, with networking and the persistent store ready; the pre-existing
SSD persistence fixture passed again. This does not establish cold-boot
reliability or behavior under device failure.
A further unpaced 4 MiB upload took 3.578 seconds; host `ss` sampled over
874 KiB retransmitted before all bytes were acknowledged
(`network-rx8-host-ss-20261101.txt`). Significant loss or delayed delivery
therefore remains; this does not locate it within the adapter, driver, network
stack or link.

The update workflow also supports the same opt-in Linux pacing cap, with an
explicit USB or system helper target; activation and restart remain separate
options. On the Dell, the 3.96 MB kernel and 32 MiB initrd passed a paced
staging-only transfer in 3.13 seconds for connection, authentication,
transfer and verification together. The guest reported no storage change
(`dell-ecm-rx8-paced-staging-20261101/receipt.json`). No boot selection was
changed. Workflow tests cover invalid pacing, error handling, and the fact
that paced staging alone cannot activate or restart.

While that physical comparison was pending, the five existing Dell audio
prerequisite host tests were rebuilt and passed in the pinned Nix shell:
firmware snapshot, I2C identity policy and probe, codec ID transaction, and
the simulated DesignWare polling adapter. These exercise validation and mocks;
they do not access the physical codec or provide audio playback.

The real `KMutexTryLock` implementation is now exercised in the cooperative
scheduler lifetime harness for the ECM dispatch pattern: a recursive poll and
a competing caller fail without disturbing ownership, a simulated kill is
deferred while the dispatcher holds the lock, and a later caller acquires it
after unlock and the cancellation boundary. The scheduler and kill are mocked;
this is not a physical process-termination test inside `NetReceive`.

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

The USB ECM driver now exposes an on-demand `bridge network` snapshot, with
receive completions, dropped packets, queue occupancy and boot-wide high-water
mark, active receive depth, transmit completions and rejected busy sends.
The counters are synchronized under the controller lock; reading them does not
continuously log to disk. `benchmark_hardware_network.py --driver-snapshot`
records snapshots around each upload. The measurement requests themselves use
the same network and can perturb small transfers. A high-water value is since
boot, not specific to a single transfer. These counters cannot detect packets
lost before USB completion or prove the cause of a TCP retransmission.
The `NetIf` layout change increments the kernel module ABI to 58; any image
using this kernel must package freshly rebuilt modules rather than retain ABI
57 modules from an older initrd.

Both receive depths passed the private QEMU USB Ethernet acceptance with the
snapshot enabled, including traffic and USB storage/HID checks. This diagnostic
kernel has not yet been installed on the Dell; preserve the currently accepted
SSD recovery slot when planning its first physical comparison.

A maintenance receiver prototype can promote an older, separately verified
slot by publishing a newer selector for its unchanged payload. It requires an
authenticated, boot-bound transaction, exact expected payload identity, and an
explicit activation; it leaves the older selector and both payloads untouched.
The subsequent ordinary update then targets the other slot. Private host tests
cover distinct slot contents, mismatched identity, same-slot and invalid
records, generation overflow, and simulated torn-selector and sync failures.
An uncertain write retains the maintenance lock. The intact selector gives a
fallback if the new record is damaged; it does not guarantee recovery from
arbitrary FAT or device failure. A disposable USB QEMU image also completed an
update, booted the new image, promoted its earlier image, and booted that image
on a third firmware boot; non-slot files and the FAT check passed. The prototype
was then used on the Dell: a staged-only verification matched the known slot B
payload, and generation 10 promotion was confirmed by both receiver and job.
The checked reboot was committed, but the Dell did not return on its previous
network address; this does not establish which payload booted or why it is
offline. Inspect the console before attempting another mutation. A lost
activation reply must be inspected using the same transaction rather than
retried automatically.

The TCP receive path and delayed-ACK timer previously cleared pending ACK
state before submitting the packet to the NIC. A busy USB transmit ring can
return `-EAGAIN`, leaving no scheduled ACK until later traffic or a much slower
window update. These two paths now requeue a failed ACK under the socket lock,
preserve newer pending work, and take a fresh cumulative ACK/window snapshot
at the next timer tick. Closed sockets discard pending ACK work. A boot-wide
`tcp_ack_submit_failures` counter is available through `bridge network`.
The separate `SockRecv` window-update path also schedules a retry on rejection,
without turning an already successful read into an error. Its unlocked send
tail holds a short socket borrow; the caller must still own the socket during
the receive operation. The socket-lifetime host test exercises the actual
receive and timer code with an injected busy NIC, repeated failures, concurrent
state advancement, window reopening, socket retirement and a closed socket.
The counter covers these paths, not every possible TCP send failure. This does
not establish that ACK rejection was the Dell's dominant throughput bottleneck;
physical measurement is still required.

The prepared `ssd-ecm-window-ack-20261101` kernel and initrd booted together
from a disposable read-only QEMU USB disk. The guest reported the expected
payload ID, brought up its debug network and reported eight active ECM receive
slots. After QEMU stopped, both files extracted from the boot disk compared
byte-for-byte with the prepared payload. This validates packaging and a QEMU
boot; it does not test the Dell SSD, hardware-specific behavior, or physical
throughput. The Dell had not returned to its old address after the previous
checked restart, so the candidate was not installed there.

After the Dell returned online, the candidate was first staged without writes;
the updater selected slot A, preserving the verified recovery image in slot B.
Generation 11 was then activated and booted with the expected payload identity
and writable store. Three unpaced 4 MiB RAM uploads took 2.73, 3.77 and 3.01 s;
three uploads paced at 16 MB/s took 0.34, 0.61 and 1.24 s. Unpaced runs added
4, 11 and 9 to both USB `tx_busy` and TCP ACK submission failures; paced runs
added none. All transfers passed their digest checks, and the USB snapshot
reported no software RX drops. These observations identify local ACK rejection
under burst load but do not prove it explains all retransmissions or the
remaining delay. They are binary-transfer timings, not browser benchmarks.

An opt-in `/etc/usb-net-tx16` experiment raises ECM transmit capacity from
eight to sixteen 2 KiB DMA slots; the default remains eight. Host tests cover
slot saturation, ordered two-TD/ZLP completions, duplicate/unknown/error
handling and pinned ownership. Existing transfer-ring wrap tests and QEMU
TCP/UDP, exact-maxpacket, USB storage and HID coexistence tests passed at
TX16; the default TX8 path also passed in QEMU. These tests do not prove the
physical controller's ordering or demonstrate a throughput improvement. The
experiment was not yet deployed to the Dell at that point.

The TX16 candidate was subsequently activated in slot A and booted on the
Dell, with the verified recovery payload retained in slot B. Its marker and
payload ID were confirmed. Unpaced 4 MiB transfers took 2.49, 2.98 and 3.05 s;
paced transfers took 0.346, 0.335 and 0.346 s. All digests matched and both
`tx_busy` and the covered TCP ACK failure counter stayed at zero. TX8's
unpaced times were 2.73, 3.77 and 3.01 s and added 24 failures across the three
runs. TX16 therefore removed the measured local queue-full failures in this
small sample, but did not produce a clear unpaced throughput improvement.
An additional unpaced TX16 upload took 1.57 s; host `ss` sampled 399,215 bytes
retransmitted before completion. That independent transfer and the variability
in timings prevent treating the two configurations as a precise causal
comparison. Host pacing is a diagnostic aid, not a general fix for browser
traffic from arbitrary servers.

A separate opt-in boot marker, `/etc/tcp-recv-window-16k`, caps the TCP
receive window at 16 KiB before window scaling. SYN fields remain unscaled;
zero windows and actual receive capacity are retained. This is a burst-control
experiment, not a claim of loss-free TCP or a bandwidth guarantee. The cap is
fixed before network initialization and may reduce performance over paths with
higher RTT. Host TCP tests and QEMU USB networking passed with and without the
cap; the QEMU packet capture checked the effective negotiated window after
scaling. They did not establish non-shrinking-window correctness or exercise a
full zero-window/reopen scenario.

The capped candidate was then activated in slot A as generation 15, leaving
the verified recovery image in slot B. The Dell reported the expected payload,
RX depth eight and the 16,384-byte cap. Three unpaced 4 MiB uploads took
0.280, 0.282 and 0.277 s; paced runs took 0.345, 0.346 and 0.348 s. Every
transfer passed its digest check, and the observed USB queue-full and covered
ACK-failure counters remained zero. One 4 MiB download took 3.63 s. These
are local RAM-transfer binary-phase times: the unpaced improvement over the
earlier TX16 run is strong evidence of burst sensitivity on this path, not a
general browser or high-RTT speed claim. Host retransmission sampling and
alternating the same build across boots remain outstanding. After the pause,
the Dell was no longer reachable at its prior address, so this boot has not
yet been reverified or cleanly shut down.

After the Dell returned, another unpaced capped 4 MiB upload took 0.335 s.
Host `ss` samples showed a 16,384-byte send window and no retransmissions in
the observed samples; the final sample preceded socket closure, so it is not
an end-to-end proof of zero retransmissions. The user's browser speed test
reported 112 Mbps download and 0.34 Mbps upload; possible overlap with the
earlier local transfer was not established. A subsequent isolated Dell-to-host
4 MiB RAM transfer took 3.61 s with a matching digest. Brook's sender waits
for each segment's ACK, consistent with a significant outbound limit but not
proof of the cause of the browser result.

Two 4 MiB HTTPS downloads from Cloudflare's speed-test endpoint with the cap
enabled took 3.74 and 4.37 s. The latter reported 19 ms TCP connection time;
16 KiB per 19 ms is only about 0.86 MB/s, consistent with a window-limited
path but not proof of the bottleneck or a controlled uncapped comparison.
The endpoint and route differ from the user's speed test. Do not make the
fixed cap the general default based on local throughput: it can substantially
limit higher-RTT connections. Investigate where uncapped bursts are lost and
compare the same path before adopting any general policy.

The uncapped diagnostic candidate adds boot-wide USB queue-credit-pressure and
zero-outstanding transitions, dispatch try-lock misses, and TCP out-of-order
admission/rejection and clipping counters. USB counts are sampled under the
controller lock. Outstanding counts software-owned transfers whose completion
events have not yet been consumed; pending events can mean the device
has fewer requests armed than this value suggests. The pressure count measures
transitions, not time or packets lost. TCP counters are global and sampled
independently; overlap includes duplicate retransmissions, and clipping is
recorded before the state-machine decision. Host tests exercise credit
transitions and actual OOO-full/clipping paths; QEMU USB acceptance passed.
These counters cannot by themselves prove where an individual packet was lost.
The snapshot layout requires module ABI 60 and freshly rebuilt modules.
This diagnostic kernel was installed on the Dell for the comparison below.

As a prerequisite for a future bounded multi-segment sender, incoming TCP
window updates now reject invalid ACK bounds, reset/no-ACK packets, stale
ordering and sequence positions outside the pre-processing receive estimate.
Active and passive handshakes initialize unscaled SYN windows; accepted window
changes wake a registered waiter. Host tests exercise actual `HandleTcp`
dispatch, handshakes, scaling, zero-window, wraparound, trimming and wakeups;
QEMU USB Ethernet acceptance passed. Socket layout changes require module ABI
59 and a fresh set of modules. This does not implement pipelining, complete TCP
segment validation or advertised-right-edge tracking. The diagnostic build
includes these changes. The ordering rules follow [RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html).

With the receive-window cap disabled, three alternating 4 MiB RAM uploads
with RX depth 8 and TX depth 16 took 2.267, 3.463 and 2.794 s unpaced;
each had respectively 282, 463 and 292 new OOO-full rejections. Paced runs
took 0.346, 0.342 and 0.337 s, without new OOO arrivals. All digests matched.
There were no new TX-busy or covered ACK-submission failures. Two USB receive
backpressure transitions occurred during the third unpaced transfer and no
zero-outstanding transitions were observed; neither fact rules out loss at
the adapter or in pending completion events. The OOO results identify loss
amplification in Brook, but do not locate the original missing segment.

An experimental 64-slot queue (up from 16), plus immediate retirement of
fully covered retained entries, passed host reassembly tests and an exact
candidate QEMU boot. Its eager data allocation rises from 24,000 to 96,000
bytes per stream socket, with additional static per-socket metadata, and the
existing drain algorithm has quadratic worst-case scanning cost. It remains
an experiment pending physical measurement. It was installed as generation
19 in slot A, while the independent recovery payload remains in slot B. The
Dell was unreachable after its checked restart, then returned after the user
intervened; the console failure was not captured.

On the 64-slot candidate, three comparable uncapped unpaced uploads took
0.393, 0.204 and 0.450 s, with respectively 159, 233 and 219 OOO arrivals,
all stored and no OOO-full rejections. Paced uploads took 0.346, 0.336 and
0.343 s; the first two had 43 OOO arrivals each, the last had none. Hashes
matched. These samples strongly support reducing OOO loss amplification on
this local path; they neither identify the original loss nor establish WAN or
outbound throughput. Two USB backpressure transitions occurred during the
three unpaced transfers; no zero-outstanding transitions were observed.
Host tests cover full-size and reversed wraparound reassembly, stale and
partially covered entries, shared receive-budget accounting and cleanup.

The same benchmark measured 3.627 s for the reverse 4 MiB transfer; another
isolated transfer took 3.626 s. Host TCP samples show 1,200-byte receive
segments with roughly one outgoing ACK per incoming segment and an open host
receive window. Brook's sender waits for an ACK and yields after each segment.
An opt-in build skipping that yield, while keeping stop-and-wait, measured
3.629, 3.633 and 3.678 s, all with matching digests. This excludes a material
yield benefit in these samples; sender pipelining still needs independent
correctness work around windows, retransmission, concurrent writes and close.
The 0.34 Mbps browser upload has not been reproduced in an isolated WAN test.
The diagnostic switch was removed from source, and the Dell was restored to
the measured 64-slot build as SSD generation 23; recovery remains in slot B.

## Two-packet sender comparison

The same kernel and settings were booted twice on the Dell, with only the
`/etc/tcp-two-packet-flight` marker changed. Both used RX depth 8, TX depth
16, the 1,200-byte egress ceiling and no receive-window cap. With two packets,
three 4 MiB Dell-to-host RAM transfers took 1.829, 1.877 and 1.876 s; with the
single-packet control they took 3.633, 3.637 and 3.628 s. All returned the
same SHA-256 digest; the observed TX-busy, RX-drop and covered ACK-failure
counters did not increase. These are binary-phase wall times including the
protocol and digest work, not wire-rate measurements.

Three subsequent 100,000-byte HTTPS uploads to the [Cloudflare speed-test
upload endpoint](https://github.com/cloudflare/speedtest#api-reference) took
1.508, 1.536 and 1.538 s with two packets, versus 2.964, 2.791 and 2.740 s
with one. All returned HTTP 200. Curl's total time includes connection and TLS
setup; reported connection times ranged 34–74 ms. The tests were consecutive
across boots, not randomized, and cannot separate network variability from
the sender effect. The measurements are consistent with RTT dominating a
small flight, but are not directly comparable to the browser speed test.

The experimental flight remains boot-opt-in; its limits are documented in
`docs/tcp-sender-pipeline-plan.md`. After the comparison, the Dell is on the
same-kernel single-packet control as SSD generation 27; the independently
verified recovery payload remains in slot B. Physical audio remains absent.

The separately built fixed-four experiment was then booted as SSD generation
29, retaining the verified recovery payload in slot B. Three 4 MiB local
Dell-to-host RAM transfers took 0.981, 0.936 and 0.936 s with matching digests;
observed TX-busy, RX-drop and covered ACK-failure counters did not rise.
Three 100,000-byte Cloudflare uploads took 1.003, 0.960 and 0.922 s, all HTTP
200; connection times were 69, 35 and 40 ms. Total time includes setup.
The four-packet kernel differs from the earlier same-kernel one/two controls,
so these sequential samples suggest scaling but do not isolate the marker.
Neither fixed flight is suitable as a general congestion policy yet. The Dell
was left on the opt-in experimental generation 29, and the separate recovery
slot was not overwritten.

The opt-in adaptive candidate (generation 31) was subsequently tested with
the same local payload. Three 4 MiB RAM transfers took 0.488, 0.538 and
0.491 s, with matching SHA-256 digests. TX-busy, RX-drop and covered
ACK-submission-failure counters did not increase during these samples. Three
100,000-byte Cloudflare HTTPS uploads took 0.670, 0.562 and 0.504 s, each
returning HTTP 200; connection times were 63, 38 and 30 ms. The local times
include the binary protocol and digest, and HTTPS totals include setup. These
consecutive tests used a different kernel from the fixed-flight tests and are
not a controlled fairness or loss assessment. They do not establish the
maximum available Internet throughput. The candidate is opt-in and the
independently verified recovery payload remains in slot B.

For a longer sample, three 1 MiB uploads on that adaptive boot took 2.097,
2.486 and 2.232 s, each HTTP 200 (roughly 3.4–4.0 Mbps including setup).
The same payload uploaded immediately afterward from the host over IPv4 took
0.208, 0.167 and 0.177 s, also HTTP 200. Cloudflare endpoints and host and
guest networking may differ, and these measurements are sequential rather
than randomized; this is a useful control, not a strict link benchmark.
During the Dell runs the observed RX-drop, TX-busy and ACK-submission-failure
counters did not increase. The data does not distinguish the eight-MSS cap,
per-send-call draining, ACK latency, scheduling or an unmeasured bottleneck.

An opt-in instrumented adaptive build (generation 33) repeated the 1 MiB
HTTPS upload in 2.007, 1.922 and 1.928 s, all HTTP 200. The aggregate probe
includes bridge and job traffic as well as curl. Across the three intervals it
recorded 127–128 slot-capacity waits and 129–130 congestion-limit waits per
run, with a boot-wide observed maximum of 9,600 outstanding bytes. Each run
had 64 calls larger than eight MSS and 275–291 call-drain wait observations;
no retry attempts, local-busy or peer-window-wait observations were counted.
Waits are observations rather than durations and the maximum is not a
per-run delta. Of 981–985 eligible retired-slot samples per interval, aggregate
latest-ACK latency was 12,193–12,274 guest ticks and aggregate delay from the
latest ACK to sender observation was 2–14 ticks. Those are not individual RTT
measurements; multiple ACKs can be combined before the sender runs. Together
the counters support testing a larger bounded flight, while also showing why
per-call drain behavior deserves attention. The diagnostic build is opt-in,
with the verified recovery payload retained in slot B.

The separate 16-slot experiment was compared with the eight-slot mode on
the same kernel, with matching 1,200-byte egress, RX/TX depths and probe
settings. Three 4 MiB local RAM transfers took 0.491, 0.490 and 0.536 s at
eight slots, versus 0.345, 0.348 and 0.296 s at sixteen, with matching hashes.
Three 1 MiB HTTPS uploads took 2.110, 2.425 and 1.936 s at eight slots, versus
1.250, 1.094 and 1.072 s at sixteen; each returned HTTP 200. The probe's
boot-wide observed maximum rose from 9,600 to 19,200 bytes. Across the WAN
intervals, slot-capacity wait observations fell from 121–136 to zero, while
call-drain observations remained 258–283. No retry attempts, local busy or
peer-window waits were observed in these intervals. The counters include
bridge/job activity and count observations rather than elapsed time. These
sequential runs are not randomized and do not establish fairness or general
loss behavior. The Dell was left on the opt-in generation 37 sixteen-slot
candidate; its separately verified recovery payload remains in slot B.

## Write-size comparison

On a later muted boot using the same 16-slot, RX-8/TX-16, 1,200-byte egress
configuration, a static guest sender transferred the same deterministic 4 MiB
to a host LAN sink using four requested `send()` sizes. The 12 runs were
interleaved across sizes; each received exactly 4 MiB with the expected SHA-256,
and the sender reported no partial writes:

| Send size | Host receipt time, seconds (three runs) |
| --- | --- |
| 4 KiB | 1.024, 1.025, 1.022 |
| 16 KiB | 0.258, 0.279, 0.271 |
| 64 KiB | 0.284, 0.267, 0.276 |
| 256 KiB | 0.245, 0.244, 0.246 |

The sink measures from its ready byte to the final payload byte, excluding the
end marker, acknowledgement and job setup; this is not a wire-only metric.
Each 4 KiB run had 1,089–1,092 additional call-drain wait observations, versus
339–351 at 16 KiB and 73–74 at 256 KiB. Those counters include incidental
bridge traffic and count observations, not elapsed time. There were no new
recorded retries or RX drops, but 64 and 256 KiB runs had one to three
additional TX-busy events each. This supports a local write-boundary effect;
it does not quantify WAN performance, ACK-delay effects, loss behavior, or
fairness. `scripts/workloads/tcp_send_chunks.c` and `tcp_chunk_sink.py` are
the bounded reproducible test pair. A first probe delivered a hash-matching
payload but could not complete because TCP `shutdown(SHUT_WR)` is unsupported;
the successful runs use an explicit end marker instead. No TCP semantics were
changed by the measurement. Recovery generation 104 was selected afterward.

## Vectored-write comparison on the recovery kernel

On the later generation 112 recovery boot, the same benchmark sent four
4,096-byte vectors per call, alternating `writev`, `sendmsg`, `sendmsg`,
`writev`. Each transfer delivered the same 4 MiB with the expected hash and
no partial returns. Host receipt times were 4.079 and 4.082 seconds for
`writev`, and 3.574 and 3.581 seconds for `sendmsg`. The host timer includes
protocol and scheduling overhead; the runs are not independent WAN results.
Brook currently drains each `writev` vector separately, whereas its `sendmsg`
path combines these vectors. This makes the gap a useful lead, not proof that
all of it is caused by draining. The recovery kernel uses the older single-
packet sender; the result must not be projected onto the experimental
adaptive flight or workloads that use separate writes. Profiling real
applications' vector sizes is needed before prioritizing a kernel change.

The comparison uses `tcp_send_chunks.c` with `4096 writev 4` or
`4096 sendmsg 4`; its existing single-send mode remains available. The host
loopback smoke check also passed all three modes with matching hashes.
