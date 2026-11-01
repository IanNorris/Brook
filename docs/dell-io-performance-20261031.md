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
