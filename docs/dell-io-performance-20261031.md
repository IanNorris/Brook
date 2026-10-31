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

A random 8 MiB sample from the previously received portion of the SSD store
compressed to 35.4% with zlib level 1 on the host, at about 66 MB/s of host
CPU time. This is not a full-image or Dell CPU measurement. Optional bounded
chunk compression is useful for tools, screenshots and bulk data, with
integrity checked over the uncompressed bytes; it should be benchmarked
separately from the underlying transport. The portable installer should run
on the laptop against the identity-bound SSD rather than requiring a host to
write a physically connected laptop drive.
