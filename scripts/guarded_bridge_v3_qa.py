"""Exercise the packaged v3 command and bulk receivers in a disposable guest."""

import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
from unittest.mock import patch
import uuid
import zlib

from bridge_keyless_image import require_keyless_image
from bridge_operator_policy import validate_operator_policy

ROOT = Path(__file__).resolve().parents[1]
HELPERS = ("HWBULK.ELF", "HWBULKZ.ELF", "HWUPDATE.ELF", "HWUPSYS.ELF", "HWSTORE.ELF", "HWSTAGE.ELF")
CHUNK_BYTES = 65536
PASS_MARKER = "GUARDED_BRIDGE_V3_RECEIVERS_PASS"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(original, fixture, expected, seed):
    """Check the executable receiver bytes and admitted public key before boot."""
    from bridge_v3 import read_operator_key

    if digest(original) != expected["source_initrd_sha256"]:
        raise RuntimeError("Guarded v3 fixture source differs from the checked package")
    operator = read_operator_key(seed)
    require_keyless_image(original)
    require_keyless_image(fixture)
    if any(operator.encode() in image.read_bytes() for image in (original, fixture)):
        raise RuntimeError("Guarded v3 image contains the synthetic operator seed")
    policy_path = "::/ETC/bridge-v3-operators"
    paths = (policy_path, *(f"::/BOOT/{name}" for name in HELPERS))
    for path in paths:
        source, packed = (
            subprocess.check_output(["mtype", "-i", image, path]) for image in (original, fixture)
        )
        if source != packed:
            raise RuntimeError(f"Guarded v3 fixture changed {path}")
        expected_sha = (expected["policy_sha256"] if path == policy_path
                        else expected["helpers_sha256"][path.rsplit("/", 1)[-1]])
        if hashlib.sha256(packed).hexdigest() != expected_sha:
            raise RuntimeError(f"Guarded v3 fixture differs from the package manifest for {path}")
        if path == policy_path:
            validate_operator_policy(packed)
            public = bytes(operator.verify_key).hex().encode("ascii")
            if public not in packed.splitlines():
                raise ValueError("Synthetic operator seed does not match the pair's public policy")

    return {**expected, "boot_fixture_initrd_sha256": digest(fixture)}


def refuse_plaintext(port):
    # This is a read-only command that is nevertheless outside public caps.
    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.sendall(b"@42 bridge hardware logs 0 1\n")
        if connection.recv(1):
            raise RuntimeError("Guarded v3 listener replied to a plaintext non-caps command")


def check_maintenance_receiver(bridge, output, session, boot_id, port, seed, store):
    from binary_bridge import binary_stream, finish_request, require_response_end
    from bridge_v3 import STORE, UPDATE, read_operator_key
    from hardware_update_protocol import line
    from hardware_update_workflow import UpdateRun

    # The ordinary 128 MiB guarded disk has no preallocated A/B payload slots
    # or mounted USB. Prove that the packaged production receiver authenticates,
    # processes the request and returns the exact encrypted destination refusal.
    role = "store" if store else "update"
    helper = "/boot/HWSTORE.ELF" if store else "/boot/HWUPSYS.ELF"
    tx = uuid.uuid4().hex
    args = [helper, boot_id, tx]
    if not store:
        args.append("--require-slot-aware")
    bridge.guest_run(session, tx, args, cwd="/tmp", uid=0, wait_s=2)
    run = UpdateRun(bridge, output)
    marker = "HWSTORE_LISTENING" if store else "HWUPDATE_LISTENING"
    run.wait_job(session, tx, lambda row: marker in row.get("stdout", {}).get("text", ""))
    with socket.create_connection(("127.0.0.1", port), timeout=20) as sock:
        stream, reserved = binary_stream(sock, read_operator_key(seed), STORE if store else UPDATE,
                                         1, boot_id, tx)
        magic = b"BRSTORE1" if store else b"BRUPLOAD"
        stream.sendall(magic + reserved + boot_id.encode() + tx.encode())
        expected = {"state": "failed", "code": 23, "published": False} if store else {"state": "failed", "code": 11}
        if store:
            if line(stream) != {"state": "ready", "store_api": 1}:
                raise RuntimeError("Unexpected v3 store readiness")
            plan = b"BRKSTORE1 16777216 01234567-89ab-cdef-0123-456789abcdef"
            stream.sendall(struct.pack("<I", len(plan)) + bytes(32) + plan + b"BEGIN001")
        finish_request(stream)
        reply = line(stream)
        require_response_end(stream)
    job = run.wait_job(session, tx, lambda row: row.get("state") == "completed")
    unchanged = "image_created=0" if store else "storage_modified=0"
    if (reply != expected or job.get("exit", {}).get("code") != expected["code"] or
            unchanged not in job.get("stderr", {}).get("text", "")):
        raise RuntimeError(f"Unexpected v3 {role} destination result: {reply}")
    return {"helper": helper, "authenticated_end": True, "safe_destination_refusal": reply["code"]}


def run(output, registry, ports, seed, expected):
    command_port, bulk_port, update_port, store_port = ports
    sys.path.insert(0, str(ROOT / "tools/vm_bridge"))
    from guest_transport import exchange
    from hardware import Hardware
    from hardware_bulk_workflow import BulkRun
    from hardware_update_workflow import verify_v3_boot
    from bridge_v3 import read_operator_key

    bridge = Hardware(registry)
    deadline = time.monotonic() + 15
    while True:
        try:
            attachment = bridge.attach("127.0.0.1", command_port, auth_mode="v3", operator_key_file=seed)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.2)

    caps = attachment["guest"]
    measurements = verify_v3_boot(caps, expected, expected["kernel_sha256"],
                                  expected["boot_fixture_initrd_sha256"])
    if (caps.get("auth_ready") is not True or caps.get("recovery_only") is True or
            caps.get("bulk_helper_api") != 1 or attachment.get("auth_mode") != "v3"):
        raise RuntimeError("Guarded desktop did not admit the packaged v3 bulk path")
    if measurements["initrd_sha256_source"] != "bootloader_ram_image":
        raise RuntimeError("Guarded desktop did not measure the booted initrd")
    if (measurements["kernel_sha256"] is None and
            measurements["kernel_sha256_source"] != "unavailable_loader_elf_digest"):
        raise RuntimeError("Guarded desktop did not report the missing loader kernel digest")
    public, = exchange("127.0.0.1", command_port, ["bridge hardware caps"], timeout=3)
    protected_fields = measurements.keys() - {"build_id"}
    if (public.get("boot_id") != attachment["guest_boot_id"] or
            public.get("auth_version") != 3 or protected_fields & public.keys()):
        raise RuntimeError("Public boot caps leaked protected evidence or described another boot")
    refuse_plaintext(command_port)

    session = attachment["session"]
    logs = bridge.command(session, "bridge hardware logs 0 1")
    if "lines" not in logs:
        raise RuntimeError("Authenticated log command was refused after guarded handoff")

    source = output / "bulk-source.bin"
    source.write_bytes(bytes(range(64)) * (2 * CHUNK_BYTES // 64) + b"brook v3 compressed tail\n" * 31)
    expected_bytes = source.stat().st_size
    expected_sha = digest(source)
    upload = BulkRun(bridge, output / "bulk-put").execute(session, None, "put", source=source, port=bulk_port)
    # Only the production getz decoder calls this for compressed chunks. Allow
    # the real zlib decoder to process each chunk and count them independently.
    with patch.object(zlib, "decompressobj", wraps=zlib.decompressobj) as decoder:
        download = BulkRun(bridge, output / "bulk-getz").execute(
            session, None, "getz", artifact_id=upload["transaction_id"], port=bulk_port
        )
    if decoder.call_count != (expected_bytes + CHUNK_BYTES - 1) // CHUNK_BYTES:
        raise RuntimeError("The compressed receiver did not send the expected DEFLATE chunks")
    for receipt in (upload, download):
        transfer = receipt["transfer"]
        if (receipt["state"] != "completed" or transfer["bytes"] != expected_bytes or
                transfer["sha256"] != expected_sha):
            raise RuntimeError("Packaged bulk receiver returned an unexpected extent or hash")
    if digest(Path(download["artifact_path"])) != expected_sha:
        raise RuntimeError("The bulk round-trip changed the bytes")

    update = check_maintenance_receiver(bridge, output, session, attachment["guest_boot_id"],
                                        update_port, seed, False)
    store = check_maintenance_receiver(bridge, output, session, attachment["guest_boot_id"],
                                       store_port, seed, True)
    evidence = {
        "operator_public_key": bytes(read_operator_key(seed).verify_key).hex(),
        "policy_sha256": expected["policy_sha256"],
        "receivers": {"command": True, "bulk": True, "update": update, "store": store},
        "boot_id": attachment["guest_boot_id"],
        "measurements": measurements,
        "package_kernel_sha256": expected["kernel_sha256"],
        "package_initrd_sha256": expected["source_initrd_sha256"],
        "packaged_helpers_sha256": {name: expected["helpers_sha256"][name] for name in HELPERS},
        "protected_command": "bridge hardware logs 0 1",
        "public_caps_protected_fields_absent": True,
        "plaintext_non_caps_refused": True,
        "bytes": expected_bytes,
        "sha256": expected_sha,
        "compressed_chunks": decoder.call_count,
        "put_receipt": "bulk-put/receipt.json",
        "getz_receipt": "bulk-getz/receipt.json",
        "job_receipts_match_binary": True,
    }
    (output / "bridge-v3-qa.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(PASS_MARKER, flush=True)
