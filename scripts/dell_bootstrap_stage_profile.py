#!/usr/bin/env python3
"""Bootstrap the signed profile installer on an existing Dell NETWORK image."""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import uuid
import zipfile

from stage_closure import NIX, ROOT, STORE, closure_info

sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/vm_bridge")]
from hardware import Hardware
from hardware_bulk_workflow import BulkRun
from hardware_update_workflow import UpdateRun
from dell_recovery_lock import inhibit_recovery

SHARED = Path("/data/Programming/Brook/shared/dell")
INSTALLER = ROOT / "tools/nix-install"
GUEST_MARKER = "STAGE_BOOTSTRAP "


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def guest_program(out):
    with zipfile.ZipFile(out, "x", compression=zipfile.ZIP_STORED) as program:
        program.write(INSTALLER / "stage_bootstrap.py", "__main__.py")
        program.write(INSTALLER / "stage_nar.py", "stage_nar.py")


def manifest_for(helper, installer, public_key):
    closure = closure_info([str(helper)])
    for entry in closure.values():
        entry["hexNarHash"] = base64.b64decode(entry["narHash"].removeprefix("sha256-")).hex()
    return {"format": 1, "helper": helper.name, "closure": closure,
            "installerSha256": sha(installer), "keySha256": sha(public_key)}


def build_installer(out):
    helper_path = subprocess.check_output([
        "nix-build", "--no-out-link", "-E",
        'import ./nix/closure-stage.nix { pkgs = import ./nix/nixpkgs.nix {}; }'
    ], cwd=ROOT, text=True).strip()
    compiler = os.environ.get("MUSL_CC")
    if not compiler:
        raise ValueError("Run from the Brook development shell so MUSL_CC is set")
    subprocess.run([compiler, "-static", "-O2", "-Wall", "-Wextra",
                    INSTALLER / "nix-install.c", "-o", out], check=True)
    return Path(helper_path)


def runtime_archive(out, manifest, missing, installer, public_key):
    names = list(manifest["closure"])
    expected = sum(manifest["closure"][names[i]]["narSize"] for i in missing)
    if expected > (992 << 20):
        raise ValueError("Missing bootstrap closure exceeds bulk-transfer limit")
    with tarfile.open(out, "x", format=tarfile.PAX_FORMAT) as archive, tempfile.TemporaryDirectory(
            dir=out.parent, prefix=".bootstrap-nar-") as staging:
        archive.add(installer, arcname="nix-install")
        archive.add(public_key, arcname="stage-plan.pub")
        nar = Path(staging) / "store.nar"
        for index in missing:
            name = names[index]
            with nar.open("wb") as stream:
                subprocess.run([*NIX, "store", "dump-path", str(STORE / name)],
                               stdout=stream, check=True)
            entry = manifest["closure"][name]
            if nar.stat().st_size != entry["narSize"] or sha(nar) != entry["hexNarHash"]:
                raise ValueError(f"Local NAR does not match the plan for {name}")
            archive.add(nar, arcname=name + ".nar")


def transfer(bridge, session, key, directory, source):
    receipt = BulkRun(bridge, directory).execute(session, key, "put", source=source)
    result = receipt["transfer"]
    if result["sha256"] != sha(source):
        raise ValueError(f"Bulk verification failed for {source.name}")
    return result["guest_path"]


def guest_action(bridge, journal, args, payload, action, identity, *options):
    job = uuid.uuid4().hex
    journal.save(state=f"{action}_dispatched", **{f"{action}_job_id": job})
    bridge.guest_run(args.session, job, [args.guest_python, payload, action, identity, *options],
                     cwd="/tmp", uid=0, wait_s=2)
    result = journal.wait_job(args.session, job, lambda status: status.get("state") == "completed",
                              timeout=args.timeout)
    markers = [json.loads(line.removeprefix(GUEST_MARKER))
               for line in result.get("stdout", {}).get("text", "").splitlines()
               if line.startswith(GUEST_MARKER)]
    journal.save(**{f"{action}_job": result})
    if result.get("exit", {}).get("code") != 0 or len(markers) != 1:
        raise RuntimeError(f"Bootstrap {action} failed. Inspect guest job {job}")
    journal.save(state=f"{action}_verified", **{action: markers[0]})
    return markers[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", required=True)
    parser.add_argument("--shared", type=Path, default=SHARED, help="private Dell staging directory")
    parser.add_argument("--key-file", type=Path, help="defaults to SHARED/hardware-update-key")
    parser.add_argument("--public-key", type=Path, help="defaults to SHARED/stage-plan.pub")
    parser.add_argument("--guest-python", default="/nix/profile/bin/python3")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=900)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true", help="check legacy layout and reusable store paths")
    mode.add_argument("--rollback-receipt", type=Path,
                      help="undo the bootstrap named in this receipt while its legacy generation is active")
    args = parser.parse_args()
    if not args.guest_python.startswith("/nix/") or not 1 <= args.timeout <= 3600:
        parser.error("Expected an absolute Nix Python path and a timeout in 1..3600 seconds")

    key = args.key_file or args.shared / "hardware-update-key"
    public_key = args.public_key or args.shared / "stage-plan.pub"
    bridge = Hardware()
    target = bridge.load_session(args.session)
    endpoint = [target["host"], target["port"]]
    journal = UpdateRun(bridge, args.output)
    journal.output.mkdir(mode=0o700, parents=True)
    try:
        program = args.output / "bootstrap.pyz"
        guest_program(program)
        journal.save(state="prepared", session=args.session, endpoint=endpoint,
                     guest_boot_id=target["guest_boot_id"])
        with inhibit_recovery(*endpoint):
            remote_program = transfer(bridge, args.session, key, args.output / "script", program)
            if args.rollback_receipt:
                original = json.loads(args.rollback_receipt.read_text())
                if original.get("endpoint") != endpoint:
                    raise ValueError("Rollback receipt was recorded for a different device")
                identity = original["bootstrap_id"]
                journal.save(bootstrap_id=identity)
                report = guest_action(bridge, journal, args, remote_program, "rollback", identity)
            else:
                installer = args.output / "nix-install"
                helper = build_installer(installer)
                der = subprocess.check_output(["openssl", "pkey", "-pubin", "-in",
                                               public_key, "-outform", "DER"])
                ed25519_prefix = bytes.fromhex("302a300506032b6570032100")
                if len(der) != 44 or not der.startswith(ed25519_prefix):
                    raise ValueError("Expected an Ed25519 public plan key")
                manifest = manifest_for(helper, installer, public_key)
                document = args.output / "bootstrap.json"
                document.write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n")
                manifest = json.loads(document.read_text())
                identity = sha(document)
                journal.save(bootstrap_id=identity)
                remote_manifest = transfer(bridge, args.session, key, args.output / "manifest", document)
                report = guest_action(bridge, journal, args, remote_program, "check", identity, remote_manifest)
                if not args.dry_run and report["phase"] != "complete":
                    archive = args.output / "runtime.tar"
                    runtime_archive(archive, manifest, report["missing"], installer, public_key)
                    remote_runtime = transfer(bridge, args.session, key, args.output / "runtime", archive)
                    report = guest_action(bridge, journal, args, remote_program, "apply", identity,
                                          remote_manifest, remote_runtime,
                                          json.dumps(report, separators=(",", ":")))
        print(json.dumps(report, indent=2))
        print(journal.output / "receipt.json")
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        journal.save(state="needs_inspection", error=str(error))
        parser.exit(1, f"{error}\nSee {journal.output / 'receipt.json'}\n")


if __name__ == "__main__":
    main()
