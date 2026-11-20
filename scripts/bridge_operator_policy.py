"""Canonical public-key list shared by the kernel build and FAT image."""

import re

PUBLIC_KEY = re.compile(rb"[0-9a-f]{64}")


def validate_operator_policy(data):
    if not data:
        return ()
    if len(data) not in (65, 130) or not data.endswith(b"\n"):
        raise ValueError("bridge operator policy needs at most two LF-terminated public keys")
    records = data[:-1].split(b"\n")
    if (
        len(records) != len(set(records))
        or any(not PUBLIC_KEY.fullmatch(key) or key == b"0" * 64 for key in records)
    ):
        raise ValueError("bridge operator policy needs distinct, nonzero lowercase Ed25519 public keys")
    return tuple(bytes.fromhex(key.decode("ascii")) for key in records)
