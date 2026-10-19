#!/usr/bin/env python3
"""Run an isolated prepared/clean guest bootstrap and boot its exact outputs.

Run inside nix-shell. Runtime disks, logs and reports must remain below build/.
"""
import argparse
import functools
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import shutil
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools/vm_bridge"))
import server

# Independent expected inventory for the source revision under acceptance.
# Keep this in step with src/tests/host/CMakeLists.txt; never accept whatever
# count a partial build happens to report.
NATIVE_TEST_COUNT = 57
NATIVE_PASS_MARKER = f"100% tests passed, 0 tests failed out of {NATIVE_TEST_COUNT}"


def sha(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def sdk_nar_bytes(store_path):
    """Measure the exported SDK closure, not an unrelated fixed RAM allowance."""
    pending = [Path(store_path).name]
    seen = set()
    total = 0
    while pending:
        name = pending.pop()
        if name in seen: continue
        if not re.fullmatch(r"[a-z0-9]{32}-[^/]+", name):
            raise ValueError("Invalid SDK closure reference")
        info = ROOT / "build/package-cache" / (name.split("-", 1)[0]+".narinfo")
        fields = dict(line.split(": ", 1) for line in info.read_text().splitlines() if ": " in line)
        if Path(fields["StorePath"]).name != name: raise ValueError("SDK narinfo mismatch")
        size = int(fields["NarSize"])
        if size < 0: raise ValueError("Invalid SDK NAR size")
        total += size
        seen.add(name)
        pending.extend(fields.get("References", "").split())
    return total


def host_environment():
    cpu = Path("/proc/cpuinfo").read_text()
    model = re.search(r"^model name\s*:\s*(.+)$", cpu, re.M)
    quota = Path("/sys/fs/cgroup/cpu.max")
    return {"cpu_model": model[1] if model else None,
            "cpu_count": os.cpu_count(), "affinity": sorted(os.sched_getaffinity(0)),
            "cpu_quota": quota.read_text().strip() if quota.exists() else None,
            "load": os.getloadavg()}


def command(args, log=None, env=None):
    if log:
        with log.open("wb") as f:
            subprocess.run(args, cwd=ROOT, env=env, check=True, stdout=f, stderr=subprocess.STDOUT)
    else:
        subprocess.run(args, cwd=ROOT, env=env, check=True)


def offline_fsck(directory, prefix):
    results = {}
    for name in ("data", "nix", *(["tmp"] if (directory / "tmp.img").exists() else [])):
        log = directory / f"{prefix}-fsck-{name}.log"
        with log.open("wb") as f:
            rc = subprocess.run(["e2fsck", "-fn", str(directory / f"{name}.img")],
                                stdout=f, stderr=subprocess.STDOUT).returncode
        results[name] = {"exit": rc, "log": str(log)}
    log = directory / f"{prefix}-fsck-boot.log"
    with log.open("wb") as f:
        rc = subprocess.run(["fsck.fat", "-n", str(directory/"boot.img")],
                            stdout=f, stderr=subprocess.STDOUT).returncode
    results['boot'] = {'exit': rc, 'log': str(log)}
    if any(v["exit"] for v in results.values()):
        raise RuntimeError(f"Offline fsck failed: {results}")
    return results


def memory_available():
    return int(re.search(r"^MemAvailable:\s+(\d+)", Path("/proc/meminfo").read_text(), re.M)[1])*1024


def wait_launcher(process, directory, session):
    """Persist the owned wrapper's terminal status, separately from QEMU."""
    code = process.wait(timeout=30)
    receipt = {"session": session, "pid": process.pid, "exit_code": code,
               "source": "owned subprocess.Popen.wait",
               "completed_host_monotonic": time.monotonic()}
    write(directory / 'launcher-exit.json', receipt)
    if code != 0:
        raise RuntimeError(f"VM launcher exited {code}; retain disks and launcher receipt")
    return receipt


def check_stopped_serial(data, directory):
    serial = Path(data["serial_log"])
    text = serial.read_text(errors="replace")
    shutil.copy2(serial, directory / "guest-final-serial.log")
    if re.search(r"KERNEL PANIC|KERNEL FAULT|BRO176-MAP(?:LEAK|UNDERFLOW)|SCHED: exit_group timeout|virtio_net: (?:TX timeout|invalid TX completion)|virtio-blk.*(?:timeout|reset)|virtio.*I/O error", text, re.I):
        raise RuntimeError(f"Guest device/panic errors; see {directory / 'guest-final-serial.log'}")


def launch(directory, instance, artifacts=None, cpus=8, memory="8G", gpu="0", composite="cpu", scenario="bootstrap", on_started=None, media_disk=None):
    if media_disk is not None:
        # Large workload payloads exceed QEMU vvfat's default FAT16 capacity.
        # Accept only a private image belonging to this experiment directory.
        media_disk = Path(media_disk).resolve(strict=True)
        media_disk.relative_to(directory.resolve())
        if not media_disk.is_file():
            raise ValueError("Experiment media disk must be a private image file")
    env = dict(os.environ, BROOK_EXPERIMENT_RUN=str(directory),
               BROOK_EXPERIMENT_DISKS=str(directory), BROOK_DISK_IMG=str(directory / "boot.img"),
               BROOK_VM_BRIDGE="1", BROOK_VM_PROVENANCE="agent", BROOK_SMP=str(cpus), BROOK_MEMORY=memory,
               BROOK_AUDIODEV="none", BROOK_GPU=gpu, BROOK_COMPOSITE=composite,
               BROOK_EXPERIMENT_DISPLAY="headless", BROOK_EXPERIMENT_TIMEOUT="14400",
               BROOK_INSTANCE=str(instance))
    temporary = directory / "tmp-host"
    temporary.mkdir(exist_ok=True)
    env["TMPDIR"] = str(temporary)
    # Do not attach unrelated user home/data images or a writable host media tree.
    media = directory / "media"
    media.mkdir(exist_ok=True)
    (media / "BROOK.MNT").write_text("/media")
    env.update(BROOK_HOME_DISK=str(directory / "absent-home.img"),
               BROOK_GPT_DISK=str(directory / "absent-gpt.img"),
               BROOK_DATA_DISK=str(media_disk if media_disk is not None else directory / "absent-extra-data.img"),
               BROOK_USB_TEST_IMG=str(directory / "usb.img"),
               BROOK_MEDIA_DIR=str(media))
    if artifacts:
        env["BROOK_BUILD_DIR"] = str(artifacts)
    log = directory / "launch.log"
    with log.open("wb") as f:
        process = subprocess.Popen([str(ROOT / "scripts/run_experiment.sh"), scenario],
                                   cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT)
    if on_started is not None:
        on_started(process)
    started = time.monotonic()
    while time.monotonic() - started < 180:
        if process.poll() is not None:
            raise RuntimeError(f"VM launcher exited {process.returncode}; see {log}")
        matches = re.findall(r"Brook VM bridge session: ([0-9a-f]+)", log.read_text(errors="replace"))
        if matches:
            session = matches[-1]
            data = server.load_session(session)
            # Registry publication precedes QEMU opening its serial log. Missing
            # output at this point means not ready yet, not a failed VM boot.
            try:
                serial_text = Path(data["serial_log"]).read_text(errors="replace")
            except FileNotFoundError:
                time.sleep(1)
                continue
            if "DEBUG_TCP: listening on port 1234" in serial_text:
                try:
                    server.guest(data, "uptime", timeout=5)
                    return process, session, data, time.monotonic() - started
                except (TimeoutError, OSError):
                    pass
        time.sleep(1)
    raise TimeoutError(f"Guest boot deadline; see {log}")


def guest_scripts(directory, mode, revision, cache_port, sdk, manifest):
    build = '''set -euo pipefail
export CMAKE_BUILD_PARALLEL_LEVEL=8
trap 'rc=$?; echo "BOOTSTRAP_EXIT $rc"' EXIT
stage() {
    local name="$1"; shift
    local started=$SECONDS
    echo "BOOTSTRAP_BEGIN $name"
    cat /proc/blkprobe
    "$@"
    cat /proc/blkprobe
    echo "BOOTSTRAP_END $name $((SECONDS-started))"
}
mkdir -p /data/home /data/tmp /data/bootstrap
cd /data/bootstrap
clone_pinned() {
    git clone --depth 1 --no-checkout https://github.com/IanNorris/Brook.git Brook
    if ! git -C Brook cat-file -e REVISION; then
        git -C Brook fetch --depth 1 origin REVISION
    fi
}
stage clone clone_pinned
cd Brook
stage checkout git checkout --detach REVISION
test "$(git rev-parse HEAD)" = REVISION
stage submodules git submodule update --init --depth 1 -- vendor/uefi-headers src/third_party/fatfs
stage configure cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -G Ninja
stage compile cmake --build build/release --parallel 8
stage modules ./scripts/check-module-symbols.sh build/release/kernel/drivers
stage native-configure cmake -S src/tests/host -B build/release/host_tests -DCMAKE_CXX_COMPILER="$HOST_CXX" -DCMAKE_BUILD_TYPE=Release -DBROOK_ROOT="$PWD" -G Ninja
stage native-compile cmake --build build/release/host_tests --parallel 8
stage native-tests ctest --test-dir build/release/host_tests --output-on-failure --no-tests=error --timeout 180
stage warm-native-clean cmake --build build/release/host_tests --target clean
stage warm-native-compile cmake --build build/release/host_tests --parallel 8
stage warm-native-tests ctest --test-dir build/release/host_tests --output-on-failure --no-tests=error --timeout 180
stage incremental-build cmake --build build/release --parallel 8
test -s build/release/kernel/BROOK.elf
test -s build/release/bootloader/BOOTX64.efi
echo "BOOTSTRAP_ARTIFACTS_OK"
sync
'''.replace("REVISION", shlex.quote(revision))
    (directory / "build.sh").write_text(build)
    start = '''set -euo pipefail
export PATH=/nix/profile/bin:/nix/bin:/boot/BIN
export HOME=/data/home TMPDIR=/data/tmp
mkdir -p "$HOME" "$TMPDIR"
trap 'rc=$?; if [ "$rc" != 0 ]; then echo "BOOTSTRAP_EXIT $rc"; fi' EXIT
'''
    if mode == "clean":
        allowed = "|".join(shlex.quote(path) for path in manifest["closure"])
        start += f'''test ! -e {shlex.quote(sdk["store_path"])}
test ! -e /nix/profile/bin/clang
test ! -e /nix/profile/bin/brook-bootstrap-shell
count=0
for path in /nix/store/*; do
    case "$path" in
        {allowed}) count=$((count+1)) ;;
        *) echo "BOOTSTRAP_UNEXPECTED_STORE_PATH $path"; exit 1 ;;
    esac
done
test "$count" = {len(manifest["closure"])}
echo "BOOTSTRAP_CLEAN_STORE_OK $count"
'''
        start += f'''echo "BOOTSTRAP_BEGIN install"
started=$SECONDS
curl -fsS http://10.0.2.2:{cache_port}/brook-bootstrap.idx -o /data/bootstrap.idx
cat /data/bootstrap.idx /nix/index/packages.idx > /data/packages.idx
cp /data/packages.idx /nix/index/packages.idx
NIX_CACHE_URL=http://10.0.2.2:{cache_port} /nix/bin/nix-install brook-bootstrap
echo "BOOTSTRAP_END install $((SECONDS-started))"
'''
    else:
        start += f'test -d {shlex.quote(sdk["store_path"])}\n'
        start += 'test -x /nix/profile/bin/clang\n'
        start += 'echo "BOOTSTRAP_PREPARED_SDK"\n'
    start += 'exec /nix/profile/bin/brook-bootstrap-shell /boot/BUILD.SH\n'
    (directory / "start.sh").write_text(start)
    for source, target in [("start.sh", "START.SH"), ("build.sh", "BUILD.SH")]:
        command(["mcopy", "-o", "-i", str(directory / "boot.img"), str(directory / source), f"::{target}"])


def validate_finished_build(text):
    plain = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
    if plain.count(NATIVE_PASS_MARKER) != 2 or "BOOTSTRAP_ARTIFACTS_OK" not in plain:
        raise RuntimeError("Guest success missing required test/artifact evidence")


def wait_existing_vm(data, timeout=30):
    # Resume runs cannot waitpid an orphaned launcher's QEMU. Observe its
    # exact PID until exit/zombie; closed QEMU descriptors precede offline IO.
    deadline = time.monotonic()+timeout
    path = Path(f"/proc/{data['pid']}/stat")
    while time.monotonic()<deadline:
        if not path.exists() or path.read_text().rsplit(")",1)[1].split()[0] == "Z": return
        time.sleep(.1)
    raise TimeoutError("Existing QEMU did not exit after stop")


class TrackedCommand:
    """Persist one job identity, byte cursors and complete observed output."""
    def __init__(self, directory, session, argv):
        self.path = directory / "command-evidence.json"
        if self.path.exists():
            self.evidence = json.loads(self.path.read_text())
            request = self.evidence["request"]
            if request["session"] != session or request["argv"] != argv:
                raise RuntimeError("Existing bootstrap command targets another session or argv")
        else:
            # SDK installation and boot verification are explicitly root tests;
            # this does not establish unprivileged bootstrap acceptance.
            self.evidence = {"request": dict(session=session, argv=argv, uid=0,
                request_id=uuid.uuid4().hex, wait_s=1), "stdout": "", "stderr": "",
                "stdout_offset": 0, "stderr_offset": 0, "status": {}}
            write(self.path, self.evidence)

    def poll(self):
        e = self.evidence
        request = e["request"]
        if e["status"].get("stage") != "committed":
            # Same identity recovers an interrupted upload, never a new launch.
            server.guest_run(**request)
        r = server.job_status(request["session"], request["request_id"],
            stdout_offset=e["stdout_offset"], stderr_offset=e["stderr_offset"],
            output_limit=16384, wait_s=0)
        for name in ("stdout", "stderr"):
            stream = r.get(name, {})
            if stream.get("dropped_bytes") or stream.get("gaps"):
                write(self.path.with_suffix(".output-gap.json"), r)
                raise RuntimeError("Bootstrap command output was lost; acceptance is unproven")
            e[name] += stream.get("text", "")
            e[name+"_offset"] = stream.get("next_offset", e[name+"_offset"])
        e["status"] = r
        write(self.path, e)
        if r.get("state") in {"failed", "rejected", "cancelled", "lost"}:
            raise RuntimeError(f"Bootstrap command cannot complete: {r.get('state')}; see {self.path}")
        if r.get("state") == "completed" and (r.get("exit", {}).get("code") != 0
                or r.get("exit", {}).get("signal") is not None):
            raise RuntimeError(f"Bootstrap command exit failure; see {self.path}")
        return e["stdout"]

    def complete(self):
        r = self.evidence["status"]
        return (r.get("state") == "completed" and r.get("exit", {}).get("code") == 0
                and r.get("exit", {}).get("signal") is None
                and r.get("guest", {}).get("live_tasks") == 0
                and all(r.get(name, {}).get("eof") and not r[name].get("read_error")
                        for name in ("stdout", "stderr")))


def monitor(directory, session, data, deadline, job=None):
    started = time.monotonic()
    stages, snapshots, profiles = [], [], []
    active = None
    capture = None
    captured_stages = set()
    profiling_available = True
    last_snapshot = 0
    seen = 0
    serial = Path(data["serial_log"])
    exit_seen = False
    while time.monotonic() - started < deadline:
        serial_text = serial.read_text(errors="replace")
        text = job.poll() if job else serial_text
        if re.search(r'BRO176-MAP(?:LEAK|UNDERFLOW)|SCHED: exit_group timeout', serial_text):
            raise RuntimeError('Guest mapped-page accounting or thread-quiescence failure; VM retained for diagnosis')
        markers = re.findall(r"^BOOTSTRAP_(BEGIN|END|EXIT) ([^\r\n]+)", text, re.M)
        for kind, payload in markers[seen:]:
            now = time.monotonic()
            if kind == "BEGIN":
                active = payload
                stages.append({"name": payload, "host_start": now-started})
                print(f"Stage {payload}", flush=True)
            elif kind == "END":
                name, seconds = payload.split()
                record = next(s for s in reversed(stages) if s["name"] == name)
                record.update(guest_seconds=int(seconds), host_end=now-started)
                active = None
                print(f"Completed {name}: {seconds}s", flush=True)
            elif kind == "EXIT":
                rc = int(payload)
                if rc:
                    raise RuntimeError(f"Guest bootstrap exit {rc}; see command/serial evidence")
                validate_finished_build(text)
                exit_seen = True
        seen = len(markers)
        if exit_seen and (job is None or job.complete()):
            write(directory / "stage-results.json", {"exit": 0, "stages": stages,
                  "snapshots": snapshots, "profiles": profiles})
            return stages, snapshots, profiles
        if job and job.complete() and not exit_seen:
            raise RuntimeError("Bootstrap command exited without required completion marker")
        # A stopped VM with no completion marker cannot make progress. Use
        # the registry's PID/incarnation check, not a stale state field.
        server.load_session(session)
        now = time.monotonic()
        if capture and now >= capture["finish_after"]:
            try:
                result = server.profile(session, action="export", guest_timeout=30)
                profiles.append(dict(stage=capture["stage"], **result))
                print(f"Profile {capture['stage']}: {result['sample_count']} CPU samples", flush=True)
                capture = None
            except RuntimeError as e:
                if "No recorded profile output path" in str(e):
                    snapshots.append({"stage": capture["stage"], "profile_error": str(e),
                                      "profiling_disabled": True})
                    capture = None
                    profiling_available = False
                elif "RUNNING" not in str(e) and "recording/flushing" not in str(e):
                    raise
            except (TimeoutError, OSError) as e:
                # Profiling is ancillary to build acceptance. The guest may
                # still be running; do not restart it or issue another start
                # after unknown completion of the profiling command.
                snapshots.append({"stage": capture["stage"], "profile_error": str(e),
                                  "profiling_disabled": True})
                print(f"Profiling unavailable after export timeout: {e}", flush=True)
                capture = None
                profiling_available = False
        if profiling_available and not capture and active in {"install", "clone", "checkout", "submodules", "compile", "native-compile", "native-tests", "warm-native-compile", "warm-native-tests"} and active not in captured_stages:
            try:
                server.profile(session, action="start", duration_ms=3000, guest_timeout=30)
                capture = {"stage": active, "finish_after": now + 5}
                captured_stages.add(active)
            except (TimeoutError, OSError) as e:
                snapshots.append({"stage": active, "profile_error": str(e), "profiling_disabled": True})
                captured_stages.add(active)
                profiling_available = False
        if now - last_snapshot >= 20:
            observation = {"stage": active, "elapsed": now-started,
                           "host_load": os.getloadavg()}
            for name, query in [("stats", "bridge stats"), ("memory", "mem")]:
                try:
                    observation[name] = server.guest(data, query, timeout=10)
                except (TimeoutError, OSError) as e:
                    observation[name] = {"error": str(e)}
            snapshots.append(observation)
            last_snapshot = now
            write(directory / "progress.json", {"stages": stages, "snapshots": snapshots, "profiles": profiles})
        if server.panic_state(data):
            raise RuntimeError("Guest panic; preserve VM for diagnosis")
        time.sleep(1)
    raise TimeoutError("Bootstrap deadline; VM retained for diagnosis")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=["clean", "prepared"], required=True)
    parser.add_argument("--seed", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--instance", type=int, default=120)
    parser.add_argument("--resume-completed-build", action="store_true", help="resume after a host verifier error, only with completed guest build and two passing suites")
    parser.add_argument("--memory", choices=["4G", "8G"], default="4G")
    parser.add_argument("--host-headroom-gib", type=int, choices=[1,2,4,8], default=4,
                        help="host memory reserve beyond guest/images; default 4 GiB")
    parser.add_argument("--backend", choices=["disk", "ram"], default="disk")
    parser.add_argument("--deadline", type=int, default=7200)
    args = parser.parse_args()
    directory = args.output.resolve()
    if args.resume_completed_build:
        if not directory.is_relative_to(ROOT / "build") or not directory.is_dir(): parser.error("Invalid resume directory")
        metadata = json.loads((directory / "manifest.json").read_text())
        if metadata["mode"] != args.mode or metadata["revision"] != args.revision: parser.error("Resume mode/revision mismatch")
        args.memory = metadata["vm"]["memory"]
        args.backend = metadata.get("backend", "disk")
        args.host_headroom_gib = metadata.get("host_memory_headroom_gib", args.host_headroom_gib)
        stage_data = json.loads((directory / "stage-results.json").read_text())
        if stage_data["exit"] != 0: parser.error("Guest build did not complete successfully")
        session = metadata["session"]
        data = server.load_session(session, live=False)
        receipt = json.loads((directory / "command-evidence.json").read_text())
        validate_finished_build(receipt["stdout"])
        if not TrackedCommand(directory, session, receipt["request"]["argv"]).complete():
            raise RuntimeError("Completed build lacks terminal, drained command evidence")
        ram = Path(metadata["ram_directory"]) if metadata.get("ram_directory") else None
        finalize_acceptance(directory, args, metadata, None, session, data, stage_data["stages"], stage_data["snapshots"], stage_data["profiles"], None, ram)
        return
    if not directory.is_relative_to(ROOT / "build") or directory.exists():
        parser.error("--output must be a new directory under build/")
    if not re.fullmatch(r"[0-9a-f]{40}", args.revision):
        parser.error("--revision must be a full pinned Git SHA")
    manifest = json.loads((args.seed / "manifest.json").read_text())
    if manifest.get("bootstrap_mode") != args.mode:
        parser.error("Seed mode does not match acceptance mode")
    sdk = json.loads((ROOT / "build/package-cache/brook-bootstrap.json").read_text())
    if args.mode == "clean" and sdk["store_path"] in manifest["closure"]:
        parser.error("Clean seed already contains the SDK")
    if args.mode == "clean" and any((Path(manifest["profile"]) / "bin" / name).exists()
                                     for name in ("clang", "clang++", "cmake", "brook-bootstrap-shell")):
        parser.error("Clean seed unexpectedly contains build tools")
    if args.mode == "prepared" and sdk["store_path"] not in manifest["closure"]:
        parser.error("Prepared seed does not contain the exported SDK")
    if args.mode == "prepared" and manifest.get("bootstrap_sdk_modes") != "guest-nar":
        parser.error("Prepared SDK modes must match guest NAR extraction; recreate the seed")
    directory.mkdir(parents=True)
    ram = None
    headroom = args.host_headroom_gib*1024**3
    if memory_available() < int(args.memory[:-1])*1024**3+headroom:
        raise RuntimeError('Insufficient available host memory for guest plus configured headroom')
    if args.backend == "ram":
        guest_capacity = int(args.memory[:-1])*1024**3
        seed_bytes = sum((args.seed / f"{name}.img").stat().st_blocks*512 for name in ("data", "nix"))
        # NAR bytes conservatively cover the SDK even for a clean seed. Reserve
        # another GiB for ext2 rounding/metadata, the boot disk and build outputs.
        image_budget = max(seed_bytes, sdk_nar_bytes(sdk["store_path"])) + 1024**3
        required_memory = guest_capacity + image_budget + headroom
        if memory_available() < required_memory or shutil.disk_usage("/dev/shm").free < image_budget:
            raise RuntimeError(f"RAM bootstrap needs {required_memory/1024**3:.2f} GiB available memory and {image_budget/1024**3:.2f} GiB free tmpfs")
        ram = Path(tempfile.mkdtemp(prefix="brook-bootstrap-", dir="/dev/shm"))
        (ram / "tmp-host").mkdir()
        (directory / "tmp-host").symlink_to(ram / "tmp-host", target_is_directory=True)
        for name in ("data", "nix", "boot"):
            (directory / f"{name}.img").symlink_to(ram / f"{name}.img")
    for name in ("data", "nix"):
        command(["cp", "--reflink=auto", "--sparse=always", str(args.seed / f"{name}.img"), str((directory / f"{name}.img").resolve())])
    env = dict(os.environ, BROOK_DISK_IMG=str(directory / "boot.img"))
    command([str(ROOT / "scripts/create_disk.sh")], directory / "boot-create.log", env)
    command([str(ROOT / "scripts/update_disk.sh")], directory / "boot-update.log", env)
    before = offline_fsck(directory, "before")
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(ROOT / "build/package-cache"))
    cache = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=cache.serve_forever, daemon=True).start()
    guest_scripts(directory, args.mode, args.revision, cache.server_port, sdk, manifest)
    metadata = {"mode": args.mode, "revision": args.revision, "sdk": sdk,
                "backend": args.backend, "ram_directory": str(ram) if ram else None,
                "initial_ram_image_budget": image_budget if ram else None,
                "host_memory_headroom_gib": args.host_headroom_gib,
                "seed": manifest, "before_fsck": before,
                "host_load": os.getloadavg(), "host_cpu_count": os.cpu_count(),
                "host_affinity": sorted(os.sched_getaffinity(0)),
                "vm": {"cpus": 8, "memory": args.memory, "gpu": "software virtio", "audio": "none", "wayland": False, "kernel_compositor": True},
                "cache_note": "Fresh guest filesystem; host page cache uncontrolled. Warm native rebuild follows cold build in same VM."}
    metadata["host_environment"] = host_environment()
    write(directory / "manifest.json", metadata)
    process, session, data, boot_seconds = launch(directory, args.instance, memory=args.memory)
    metadata.update(session=session, boot_seconds=boot_seconds)
    metadata["qemu_argv"] = [p.decode() for p in Path(f"/proc/{data['pid']}/cmdline").read_bytes().split(b"\0") if p]
    write(directory / "manifest.json", metadata)
    print(f"Booted {args.mode}: {session} ({boot_seconds:.2f}s)", flush=True)
    job = TrackedCommand(directory, session, ["/nix/profile/bin/bash", "/boot/START.SH"])
    stages, snapshots, profiles = monitor(directory, session, data, args.deadline, job=job)
    finalize_acceptance(directory, args, metadata, process, session, data, stages, snapshots, profiles, cache, ram)


def finalize_acceptance(directory, args, metadata, process, session, data, stages, snapshots, profiles, cache, ram):
    if process is None and data.get("state") == "stopped":
        if data.get("exit_code") != 0: raise RuntimeError("Resume VM did not exit cleanly")
        receipt = json.loads((directory / "command-evidence.json").read_text())
        validate_finished_build(receipt["stdout"])
        stop = {"already_stopped": True, "exit_code": 0,
                "sync_source": "completed guest build script ends with successful sync; normal VM exit recorded by launcher"}
    else:
        stop = server.control(session, "stop")
        if not stop.get("guest_sync_completed"):
            raise RuntimeError("Guest sync was not acknowledged")
    metadata["initial_shutdown"] = stop
    write(directory / "stop-evidence.json", stop)
    if process:
        metadata['initial_launcher_exit'] = wait_launcher(process, directory, session)
    else:
        wait_existing_vm(data)
        receipt_path = directory / 'launcher-exit.json'
        if not receipt_path.exists():
            raise RuntimeError('Original VM launcher exit receipt missing; stopped disks retained for audit')
        receipt = json.loads(receipt_path.read_text())
        if receipt.get('session') != session or receipt.get('exit_code') != 0:
            raise RuntimeError('Original VM launcher exit is unverified or failed')
        metadata['initial_launcher_exit'] = receipt
    check_stopped_serial(data, directory)
    if cache: cache.shutdown()
    after = offline_fsck(directory, "after")
    outputs = directory / "guest-artifacts"
    outputs.mkdir()
    command(["debugfs", "-R", f"rdump /bootstrap/Brook/build/release {outputs}", str(directory / "data.img")], directory / "extract.log")
    artifacts = outputs / "release"
    required = [artifacts / "kernel/BROOK.elf", artifacts / "bootloader/BOOTX64.efi"]
    modules = sorted((artifacts / "kernel/drivers").glob("*.mod"))
    baremetal = sorted((artifacts / "tests").glob("*.elf"))
    native = sorted(p for p in (artifacts / "host_tests").glob("test_*") if p.is_file() and os.access(p, os.X_OK))
    if len(modules) != 12 or len(baremetal) != 7 or len(native) != NATIVE_TEST_COUNT or any(not p.is_file() or not p.stat().st_size for p in required + modules + baremetal):
        raise RuntimeError(f"Missing artifacts: modules={len(modules)} baremetal={len(baremetal)} native={len(native)}")
    hashes = {str(p.relative_to(artifacts)): sha(p) for p in required + modules + baremetal + native}
    write(directory / "artifact-hashes.json", hashes)
    verification = directory / "verify-boot"
    verification.mkdir()
    if ram:
        guest_capacity = int(args.memory[:-1])*1024**3
        image_bytes = sum((directory / f"{name}.img").stat().st_blocks*512 for name in ("boot", "data", "nix"))
        metadata["verification_image_bytes"] = image_bytes
        if shutil.disk_usage(ram).free < image_bytes+512*1024**2 or memory_available() < image_bytes+guest_capacity+args.host_headroom_gib*1024**3:
            raise RuntimeError("Insufficient RAM for independent verification images plus guest; originals preserved")
        (ram / "verify").mkdir()
        (ram / "verify/tmp-host").mkdir()
        (verification / "tmp-host").symlink_to(ram / "verify/tmp-host", target_is_directory=True)
        for name in ("boot", "data", "nix"):
            (verification / f"{name}.img").symlink_to(ram / "verify" / f"{name}.img")
    for name in ("boot", "data", "nix"):
        command(["cp", "--reflink=auto", "--sparse=always", str(directory / f"{name}.img"), str((verification / f"{name}.img").resolve())])
    for module in modules:
        command(["mcopy", "-o", "-i", str(verification / "boot.img"), str(module), f"::DRIVERS/{module.name.upper()}"])
    command(["mcopy", "-o", "-i", str(verification / "boot.img"), str(artifacts / "kernel/ksym_addr_table.cpp"), "::KSYM_ADDR.TXT"])
    verify_script = verification / "verify.sh"
    verify_script.write_text('''set -euo pipefail
trap 'rc=$?; echo "BOOTSTRAP_VERIFY_EXIT $rc"' EXIT
cd /data/bootstrap/Brook
git fsck --full
ctest --test-dir build/release/host_tests --output-on-failure --no-tests=error --timeout 180
sync
''')
    command(["mcopy", "-o", "-i", str(verification / "boot.img"), str(verify_script), "::VERIFY.SH"])
    verify_process, verify_session, verify_data, verify_seconds = launch(verification, args.instance + 1, artifacts, memory=args.memory)
    if verify_data["kernel_sha256"] != hashes["kernel/BROOK.elf"]:
        raise RuntimeError("Verification VM did not boot the extracted guest kernel")
    replies = {query: server.guest(verify_data, query, timeout=20) for query in ("uptime", "mem", "procs")}
    verify_job = TrackedCommand(verification, verify_session, ["/nix/profile/bin/brook-bootstrap-shell", "/boot/VERIFY.SH"])
    verify_started = time.monotonic()
    while time.monotonic() - verify_started < 600:
        text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", verify_job.poll())
        exit_marker = re.search(r"^BOOTSTRAP_VERIFY_EXIT (\d+)", text, re.M)
        if exit_marker:
            if exit_marker[1] != "0" or NATIVE_PASS_MARKER not in text:
                raise RuntimeError("Tests under guest-built kernel failed")
            if verify_job.complete():
                break
        elif verify_job.complete():
            raise RuntimeError("Verification command exited without required completion marker")
        if server.panic_state(verify_data):
            raise RuntimeError("Guest-built verification kernel panicked")
        time.sleep(1)
    else:
        raise TimeoutError("Guest-built kernel native tests deadline")
    replies["after_tests"] = server.guest(verify_data, "bridge stats", timeout=20)
    verify_native_seconds = time.monotonic() - verify_started
    verify_stop = server.control(verify_session, "stop")
    if not verify_stop.get("guest_sync_completed"):
        raise RuntimeError("Verification guest sync was not acknowledged")
    verify_launcher_exit = wait_launcher(verify_process, verification, verify_session)
    check_stopped_serial(verify_data, verification)
    verify_fsck = offline_fsck(verification, "after")
    # Hosted user-copy probes deliberately fault. Reject unexpected visible
    # records without treating those pinned, successfully recovered tests as
    # application crashes. Serial remains best-effort evidence, not a census.
    from audit_bootstrap_faults import audit_files
    fault_audit = audit_files(artifacts / "host_tests/test_user_copy", [
        (directory / "guest-final-serial.log", directory / "command-evidence.json"),
        (verification / "guest-final-serial.log", verification / "command-evidence.json"),
    ])
    write(directory / "user-fault-audit.json", fault_audit)
    metadata.update(stages=stages, snapshots=snapshots, profiles=profiles,
                    after_fsck=after, artifact_hashes=hashes,
                    verification={"session": verify_session, "boot_seconds": verify_seconds,
                                  "launcher_exit": verify_launcher_exit,
                                  "native_test_seconds": verify_native_seconds,
                                  "replies": replies, "fsck": verify_fsck}, success=True)
    if ram:
        # Both VMs are stopped and synced. Preserve final images on disk once,
        # rather than sending every guest write to the SSD during the run.
        for image_dir in (directory, verification):
            for name in ("boot", "data", "nix"):
                image = image_dir / f"{name}.img"
                saved = image_dir / f"{name}.saved.img"
                command(["cp", "--sparse=always", str(image), str(saved)])
                image.unlink()
                saved.rename(image)
        for image_dir in (directory, verification):
            (image_dir / "tmp-host").unlink()
            (image_dir / "tmp-host").mkdir()
        shutil.rmtree(ram)
    write(directory / "result.json", metadata)
    print(f"PASS: {args.mode} bootstrap, repeated {NATIVE_TEST_COUNT}/{NATIVE_TEST_COUNT} tests, guest-built boot and offline fsck", flush=True)


if __name__ == "__main__":
    main()
