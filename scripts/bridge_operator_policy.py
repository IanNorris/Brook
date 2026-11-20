"""Canonical public-key list shared by the kernel build and FAT image."""

import hashlib
import json
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


def verify_receiver_evidence(verdict_path, verdict, manifest, operator_public_key):
    """Bind the exact clean pair and admitted operator to all four receivers."""
    attempt = next((item for item in reversed(verdict.get("attempts", []))
                    if item.get("passed") is True), {})
    run = attempt.get("output", "")
    if (not re.fullmatch(r"run-[1-9][0-9]*", run) or
            attempt.get("bridge_v3_evidence") != "bridge-v3-qa.json"):
        raise ValueError("The exact v3 pair needs a passing --bridge-v3-qa verdict")
    directory = verdict_path.parent / run
    path = directory / "bridge-v3-qa.json"
    if directory.is_symlink() or path.is_symlink() or not path.is_file():
        raise ValueError("Missing guarded v3 receiver evidence")
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != attempt.get("bridge_v3_evidence_sha256"):
        raise ValueError("Guarded v3 evidence changed after the check")
    evidence = json.loads(raw)
    policy = manifest["bridge_v3"]
    if (evidence.get("package_kernel_sha256") != manifest["kernel_sha256"] or
            evidence.get("package_initrd_sha256") != manifest["initrd_sha256"] or
            evidence.get("operator_public_key") != operator_public_key.hex() or
            evidence.get("policy_sha256") != policy["policy_sha256"] or
            evidence.get("packaged_helpers_sha256") != policy["helpers_sha256"]):
        raise ValueError("Guarded v3 evidence belongs to a different pair or operator")
    if evidence.get("receivers") != {
        "command": True,
        "bulk": True,
        "update": {"helper": "/boot/HWUPSYS.ELF", "authenticated_end": True,
                   "safe_destination_refusal": 11},
        "store": {"helper": "/boot/HWSTORE.ELF", "authenticated_end": True,
                  "safe_destination_refusal": 23},
    }:
        raise ValueError("Guarded v3 evidence must cover command, bulk, update and store")
