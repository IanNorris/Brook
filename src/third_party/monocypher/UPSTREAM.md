# Monocypher 4.0.3

The sources and license come from the official release.

- Archive [monocypher-4.0.3.tar.gz](https://monocypher.org/download/monocypher-4.0.3.tar.gz)
- SHA-256 `8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66`
- Verified against the [published SHA-512 checksum](https://monocypher.org/download/monocypher-4.0.3.tar.gz.sha512)

The Ed25519 sources come from the archive's `src/optional` directory. The library is available under the terms in `LICENCE.md`.

Brook adds an opt-in strict Ed25519 verification entry point for bridge operator
proofs. It uses the existing point decoder and group operations to require
canonical encodings, a non-identity prime-order public key and the uncofactored
verification equation. The existing upstream APIs retain their original
behavior. The RFC 8032 vectors and pinned C2SP Wycheproof test vectors exercise
the bridge's stricter profile.
