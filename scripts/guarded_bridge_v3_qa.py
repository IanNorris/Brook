"""Exercise the four packaged v3 receivers in a disposable guest."""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
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
INTEL_NIC_DEVICE_IDS = {"e1000": 0x100E, "e1000-82545em": 0x100F, "e1000e": 0x10D3}
NIC_PASS_MARKER = "GUARDED_BRIDGE_V3_NIC_PASS"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def make_nic_disk(output, recovery, manifest):
    """Start with an empty GPT and use only volumes named by the QEMU fixture."""
    require_keyless_image(recovery)
    identity = bytes.fromhex(manifest["guard_identity"])
    if len(identity) != 48 or (manifest["data_first"], manifest["data_sectors"]) != (133120, 126976):
        raise ValueError("Unexpected guarded QEMU disk identity")
    volume = subprocess.check_output(
        ["mtype", "-i", recovery, "::/ETC/ahci-boot-volume"], text=True
    ).splitlines()
    if (len(volume) != 4 or volume[:2] != ["BRKESP1", identity[:16].hex()] or
            volume[3] != "2048,131072"):
        raise ValueError("Unexpected guarded QEMU boot volume")
    disk_guid = uuid.UUID(bytes_le=identity[:16])
    esp_guid = uuid.UUID(bytes_le=bytes.fromhex(volume[2]))
    ext2_guid = uuid.UUID(bytes_le=identity[16:32])
    ext2_uuid = uuid.UUID(bytes=identity[32:])
    esp = output.with_suffix(".fat")
    data = output.with_suffix(".ext2")
    for path, sectors in ((output, 262144), (esp, 131072), (data, 126976)):
        with path.open("xb") as stream:
            stream.truncate(sectors * 512)
    layout = f"""label: gpt
label-id: {disk_guid}
unit: sectors
first-lba: 2048
last-lba: 262110

start=2048,size=131072,type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B,uuid={esp_guid}
start=133120,size=126976,type=0FC63DAF-8483-4772-8E79-3D69D8477DE4,uuid={ext2_guid}
"""
    subprocess.run(["sfdisk", output], input=layout, text=True, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["mkfs.fat", "-F", "32", "-s", "1", esp], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096", "-I", "128", "-N", "1024",
                    "-m", "0", "-O", "none,filetype,sparse_super,large_file",
                    "-U", str(ext2_uuid), data], check=True)
    for directory in ("EFI", "EFI/BOOT", "KERNEL", "RECOVERY"):
        subprocess.run(["mmd", "-i", esp, "::/" + directory], check=True)
    subprocess.run(["mcopy", "-i", esp, "-", "::/BROOK.CFG"], check=True,
                   input=b"TARGET=KERNEL\\BROOK.ELF\nRECOVERY_MENU=1\nDISPLAY_WIDTH=1024\nDISPLAY_HEIGHT=768\n")
    require_keyless_image(esp)
    with output.open("r+b") as disk:
        for start, partition in ((2048, esp), (133120, data)):
            disk.seek(start * 512)
            with partition.open("rb") as source:
                shutil.copyfileobj(source, disk)
            partition.unlink()
    return digest(output)


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


def check_operator_refusal(port):
    from nacl.signing import SigningKey
    from bridge_v3 import BridgeV3Refused, COMMAND, ZERO_ID, open_bridge_v3

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        try:
            open_bridge_v3(connection, SigningKey.generate(), COMMAND, 0, ZERO_ID,
                           uuid.uuid4().bytes)
        except BridgeV3Refused:
            return
    raise RuntimeError("Guarded v3 listener admitted an unknown operator")


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


def run_nic_smoke(bridge, output, session, boot_id, bulk_port, model, module_sha256):
    """Verify the attached NIC, a separate guest job and a nonblank frame."""
    from hardware_bulk_workflow import BulkRun
    from hardware_update_workflow import UpdateRun

    doctor = bridge.doctor(session)
    if (doctor.get("probe_stage") != "verified" or doctor.get("reachable") is not True or
            doctor.get("identity_matches") is not True or doctor["guest_boot_id"] != boot_id or
            not doctor["capabilities"]["command_jobs"] or not doctor["capabilities"]["frame_capture"]):
        raise RuntimeError("Intel NIC did not support a verified v3 doctor, jobs and frame capture")
    (output / "intel-nic-doctor.json").write_text(json.dumps(doctor, indent=2) + "\n")
    cursor = 0
    network = []
    root_bridges = []
    while True:
        page = bridge.command(session, f"bridge hardware pci {cursor} 8")
        if page.get("pci_inventory_api") != 1 or page.get("truncated") is not False:
            raise RuntimeError("Intel NIC PCI inventory is unavailable")
        network.extend(device for device in page["devices"] if device["class"] == 2)
        root_bridges.extend(device for device in page["devices"]
                            if device["class"] == 6 and device["subclass"] == 4 and device["bus"] == 0)
        new_cursor = page["next_cursor"]
        if new_cursor == page["total"]:
            break
        if not cursor < new_cursor < page["total"] or page["total"] > 256:
            raise RuntimeError("Invalid QEMU PCI inventory pagination")
        cursor = new_cursor
    matching = [device for device in network if device["vendor_id"] == 0x8086 and
                device["device_id"] == INTEL_NIC_DEVICE_IDS[model]]
    if len(network) != 1 or len(matching) != 1:
        raise RuntimeError(f"Expected only PCI NIC {model}; got {network!r}")
    nic = matching[0]
    if not nic["bus"] or len(root_bridges) != 1:
        raise RuntimeError(f"Expected {model} behind a PCI bridge; got bus {nic['bus']} and {root_bridges!r}")
    print(f"INTEL_NIC_PCI {model} {nic['bus']:02x}:{nic['device']:02x}.{nic['function']} secondary_bus={nic['bus']}",
          flush=True)

    # This is distinct from the transfer worker and runs under the desktop uid.
    transaction = uuid.uuid4().hex
    bridge.guest_run(session, transaction, ["/boot/HWSTATUS.ELF"], cwd="/tmp", uid=1000, wait_s=2)
    job = UpdateRun(bridge, output).wait_job(session, transaction,
                                            lambda result: result.get("state") == "completed")
    payload = doctor.get("guest", {}).get("payload_id")
    job_lines = job.get("stdout", {}).get("text", "").splitlines()
    if (not payload or job.get("session") != session or job.get("guest_boot_id") != boot_id or
            job.get("operation_id") != transaction or job.get("exit", {}).get("code") != 0 or
            f"HARDWARE_PAYLOAD_ID {payload}" not in job_lines or
            not any(line.startswith("HARDWARE_NETWORK_READY ") for line in job_lines)):
        raise RuntimeError("Authenticated Intel NIC guest job did not complete with the expected output")
    (output / "intel-nic-guest-job.json").write_text(json.dumps(job, indent=2) + "\n")

    frame = BulkRun(bridge, output / "intel-nic-frame").execute(
        session, None, "framez", port=bulk_port, keep_raw=True
    )
    transfer = frame["transfer"]
    raw = Path(transfer["raw_artifact_path"])
    pixels = raw.read_bytes()
    pixel_count = frame["frame"]["width"] * frame["frame"]["height"]
    if (frame["state"] != "completed" or frame["guest_boot_id"] != boot_id or
            len(pixels) != pixel_count * 4 or raw.stat().st_size != transfer["bytes"] or
            digest(raw) != transfer["sha256"] or digest(Path(frame["artifact_path"])) != frame["artifact_sha256"]):
        raise RuntimeError("Intel NIC frame data differs from its authenticated capture receipt")
    sample_step = max(1, pixel_count // 4096)
    colours = len({pixels[i*4:i*4+3] for i in range(0, pixel_count, sample_step)})
    if colours < 2:
        raise RuntimeError("Intel NIC frame contains only a single colour")
    return {
        "qemu_nic_model": model, "pci": nic, "pci_bridge": root_bridges[0],
        "module_sha256": module_sha256,
        "auth_mode": "v3", "boot_id": boot_id, "doctor_verified": True,
        "doctor_receipt": "intel-nic-doctor.json",
        "guest_job": "intel-nic-guest-job.json",
        "frame_receipt": "intel-nic-frame/receipt.json",
        "frame_png": "intel-nic-frame/screenshot.png",
        "frame_png_sha256": frame["artifact_sha256"], "frame_bytes": len(pixels),
        "frame_raw_sha256": transfer["sha256"], "frame_sample_colours": colours,
        "frame": frame["frame"],
    }


def run(output, registry, ports, seed, expected, nic_model=None, module_sha256=None):
    command_port, bulk_port, update_port, store_port = ports
    sys.path.insert(0, str(ROOT / "tools/vm_bridge"))
    from guest_transport import exchange, exchange_v3
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
    check_operator_refusal(command_port)

    session = attachment["session"]
    with ThreadPoolExecutor(max_workers=2) as pool:
        concurrent_caps = [
            pool.submit(exchange_v3, "127.0.0.1", command_port, ["bridge hardware caps"], 6, seed)
            for _ in range(2)
        ]
        if any(request.result()[0].get("boot_id") != attachment["guest_boot_id"] for request in concurrent_caps):
            raise RuntimeError("Concurrent v3 commands did not report the attached boot")
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
        "unknown_operator_refused_before_challenge": True,
        "concurrent_authenticated_commands": True,
        "bytes": expected_bytes,
        "sha256": expected_sha,
        "compressed_chunks": decoder.call_count,
        "put_receipt": "bulk-put/receipt.json",
        "getz_receipt": "bulk-getz/receipt.json",
        "job_receipts_match_binary": True,
    }
    if nic_model:
        evidence["intel_nic"] = run_nic_smoke(bridge, output, session, attachment["guest_boot_id"],
                                              bulk_port, nic_model, module_sha256)
        print(NIC_PASS_MARKER, nic_model, flush=True)
    (output / "bridge-v3-qa.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(PASS_MARKER, flush=True)
