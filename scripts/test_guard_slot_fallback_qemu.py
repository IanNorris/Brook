"""Offline checks for the guarded A/B start journal in the disposable SATA fixture."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import uuid
import zlib

from hardware_update_bootstat import SECTOR, inspect_sectors
from hardware_update_protocol import record
from test_panic_fat_qemu import verify_record
from test_recovery_data_dispatch import metadata_hash
from test_recovery_menu_boot import boot

ESP_OFFSET = 2048 * 512
ESP_BYTES = 131072 * 512
DATA_OFFSET = 133120 * 512
DATA_BYTES = 126976 * 512
GENERATIONS = (41, 42)
RESET_OPTION = 'name=opt/brook/power,string=q35-reset'
CLEAN_RESET = 'AHCI_DIRECT_SESSION_CLEAN'
HEALTH_WINDOW_MS = 60000
NATIVE_CLIENT = '/boot/GDFRAME.ELF'
NATIVE_FRAMES = 'GUARD_NATIVE_FRAMES_PASS'
RECOVERY_USB = 'Recovery payload invalid or missing; use a recovery USB.'
SLOT_CASES = ('fallback', 'confirmed', 'exhausted', 'profile-rejected', 'profile-repaired',
              'profile-both-repaired', 'profile-retry-interrupted', 'early-restart', 'checked-early-restart', 'post-init-panic',
              'zero-journal', 'emergency', 'legacy-loader')


def read_fat(media, name):
    return subprocess.check_output(['mtype', '-i', media, '::/' + name])


def write_fat(media, name, source):
    subprocess.run(['mcopy', '-o', '-i', media, str(source), '::/' + name], check=True)


def expect_legacy_bootstat(disk, state):
    with disk.open('rb') as raw:
        def read(lbas):
            return [os.pread(raw.fileno(), SECTOR, lba * SECTOR) for lba in lbas]

        actual = inspect_sectors(read, disk.stat().st_size // SECTOR)['state']
    if actual != state:
        raise RuntimeError(f'archived RECOVERY expected BOOTSTAT {state}, found {actual}')


def seed_health(identity, selectors, prior_strike):
    if not prior_strike:
        return bytes(1024)
    sector = bytearray(512)
    struct.pack_into('<8sQ32s', sector, 0, b'BRKSLT01', 1, hashlib.sha256(identity.encode()).digest())
    struct.pack_into('<Q32sII', sector, 96, GENERATIONS[1], hashlib.sha256(selectors[1]).digest(), 1, 0)
    struct.pack_into('<I', sector, 508, zlib.crc32(sector[:508]))
    return sector + bytes(512)


def read_health(media, identity, selectors, sequence, slots, *, prior_slots=None):
    raw = read_fat(media, 'RECOVERY/BOOTSTAT.BIN')
    if len(raw) != 1024:
        raise RuntimeError('incorrect boot journal allocation')
    disk_hash = hashlib.sha256(identity.encode()).digest()
    digests = [hashlib.sha256(selector).digest() for selector in selectors]
    snapshots = {}
    for offset in (0, 512):
        sector = raw[offset:offset + 512]
        if not any(sector):
            continue
        magic, seq, disk = struct.unpack_from('<8sQ32s', sector)
        if (magic != b'BRKSLT01' or not seq or seq in snapshots or disk != disk_hash or
                zlib.crc32(sector[:508]) != struct.unpack_from('<I', sector, 508)[0]):
            raise RuntimeError('invalid boot journal sector')
        snapshots[seq] = []
        for slot, expected_generation in enumerate(GENERATIONS):
            generation, digest, starts, rejected = struct.unpack_from('<Q32sII', sector, 48 + slot * 48)
            if (generation, digest) not in ((0, bytes(32)), (expected_generation, digests[slot])):
                raise RuntimeError(f'journal does not bind slot {slot} to a verified selector')
            if starts > 2 or rejected not in (0, 1) or (not generation and starts) or (rejected and not starts):
                raise RuntimeError(f'invalid journal state for slot {slot}')
            snapshots[seq].append((generation, starts, rejected))
    if max(snapshots, default=0) != sequence or snapshots.get(sequence) != slots:
        raise RuntimeError(f'boot journal mismatch at sequence {sequence}: {snapshots}')
    if prior_slots is not None and snapshots.get(sequence - 1) != prior_slots:
        raise RuntimeError(f'prior charged state missing at sequence {sequence - 1}: {snapshots}')
    return dict(sequence=sequence, slots=[dict(slot=i, generation=g, starts=n, profile_rejected=bool(r))
                                          for i, (g, n, r) in enumerate(slots)])


def extract(disk, path, offset, size):
    with disk.open('rb') as source, path.open('wb') as dest:
        source.seek(offset)
        image = source.read(size)
        if len(image) != size:
            raise RuntimeError('disposable disk lost a partition')
        dest.write(image)


def check_volumes(disk, work, output, expect_clean, *, expect_repair=False):
    fat, data = work / 'esp.img', work / 'data.img'
    extract(disk, fat, ESP_OFFSET, ESP_BYTES)
    extract(disk, data, DATA_OFFSET, DATA_BYTES)
    with output.open('wb') as log:
        subprocess.run(['fsck.fat', '-n', str(fat)], check=True, stdout=log, stderr=subprocess.STDOUT)
        check = subprocess.run(['e2fsck', '-f', '-n', str(data)], stdout=log, stderr=subprocess.STDOUT)
        # A crash can leave pending allocation metadata. Only the synthetic
        # panic step can report uncorrected errors because it must be followed
        # by the checked recovery boot and a clean, successful e2fsck.
        if check.returncode and not (expect_repair and check.returncode == 4):
            check.check_returncode()
    with data.open('rb') as image:
        image.seek(1024 + 58)
        clean = image.read(2) == b'\x01\0'
        if clean != expect_clean:
            raise RuntimeError(f'data-volume header clean={clean}, expected {expect_clean}')
    return clean, check.returncode


def attach_desktop(work, port):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/vm_bridge'))
    from hardware import Hardware
    bridge = Hardware(work / 'registry')
    deadline = time.monotonic() + 20
    while True:
        try:
            session = bridge.attach('127.0.0.1', port)['session']
            break
        except (OSError, RuntimeError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(.2)
    return bridge, session


def present_native_frames(work, port, output):
    bridge, session = attach_desktop(work, port)
    result = bridge.guest_run(session, uuid.uuid4().hex, [NATIVE_CLIENT], cwd='/tmp', uid=1000, wait_s=10)
    output.write_text(json.dumps(result, indent=2) + '\n')
    if (result.get('state') != 'completed' or result.get('exit', {}).get('code') != 0 or
            result.get('stdout', {}).get('text', '').strip() != NATIVE_FRAMES):
        raise RuntimeError('guarded native window failed to submit distinct frames')


def finish_checked_shutdown(work, port, evidence, restart=False):
    serial = work / 'serial.log'
    deadline = time.monotonic() + 8
    while 'GUARD_DESKTOP_SERVICE_PASS' not in serial.read_text(errors='replace'):
        if time.monotonic() >= deadline:
            raise RuntimeError('guarded desktop never ran the fixture service')
        time.sleep(.05)
    bridge, session = attach_desktop(work, port)
    doctor = bridge.doctor(session)
    if not doctor.get('identity_matches') or not doctor['capabilities'].get('hardware_power'):
        raise RuntimeError('guarded desktop is not ready for checked shutdown')
    request = uuid.uuid4().hex
    state = bridge.power(session, request, 'restart' if restart else 'shutdown')
    deadline = time.monotonic() + 60
    while state.get('state') != 'ready':
        if state.get('state') == 'failed' or time.monotonic() >= deadline:
            raise RuntimeError(f'checked shutdown did not reach ready: {state}')
        time.sleep(.2)
        state = bridge.power(session, request, 'status')
    final = bridge.doctor(session)['guest']['writable_desktop']
    if not final.get('clean_finalized') or any(final.get(k) for k in ('dirty_session', 'state_uncertain', 'final_error')):
        raise RuntimeError(f'checked shutdown did not finalize cleanly: {final}')
    evidence.write_text(json.dumps({'shutdown': state, 'final_desktop': final}, indent=2) + '\n')
    if restart:
        bridge.power(session, request, 'commit')


def edit_profile_index(disk, work, restore=None):
    # This required shared file sits outside any boot slot.
    data = work / 'data.img'
    extract(disk, data, DATA_OFFSET, DATA_BYTES)
    command = f'write {restore} /index/packages.idx' if restore else 'rm /index/packages.idx'
    subprocess.run(['debugfs', '-w', '-R', command, str(data)], check=True, stdout=subprocess.DEVNULL)
    with disk.open('r+b') as dest, data.open('rb') as source:
        dest.seek(DATA_OFFSET)
        shutil.copyfileobj(source, dest)


def poke_desktop(work):
    # Exercise kernel-only cursor redraws in the negative desktop case.
    with socket.socket(socket.AF_UNIX) as monitor:
        monitor.settimeout(1)
        monitor.connect(str(work / 'monitor.sock'))
        for dx in (32, -16, 32):
            monitor.sendall(f'mouse_move {dx} 8\n'.encode())
            time.sleep(.15)


def post_init_panic(kernel, work):
    symbols = subprocess.check_output(['nm', str(kernel)], text=True)

    def address(name):
        matches = [int(line.split()[0], 16) for line in symbols.splitlines() if line.split()[-1:] == [name]]
        if len(matches) != 1:
            raise RuntimeError('missing post-INIT panic symbol ' + name)
        return matches[0]

    present_count = address('_ZN5brook29CompositorDesktopPresentCountEv')
    panic = address('KernelPanic')
    scratch = address('_ZN5brookL8g_kbdBufE')
    gdb = work / 'post-init-panic.gdb'
    # The first sample is taken on admission; the second follows successful INIT.RC.
    gdb.write_text('set pagination off\nset confirm off\nset architecture i386:x86-64\n'
                   f'target remote {work / "gdb.sock"}\nhbreak *{present_count:#x}\nignore 1 1\ncontinue\n'
                   f'set {{unsigned long long}}{scratch:#x}=0x0054534554424447\n'
                   f'set $rdi={scratch:#x}\nset $rip={panic:#x}\ndelete breakpoints\ndetach\nquit\n')
    return gdb


def interrupt_profile_completion(kernel, work):
    symbols = subprocess.check_output(['nm', '-C', str(kernel)], text=True)
    pattern = r'^([0-9a-f]+) T brook::FatFsPrivateRecoveryTestComplete\([^\n]*\)$'
    matches = re.findall(pattern, symbols, re.MULTILINE)
    if len(matches) != 1:
        raise RuntimeError('missing guard completion breakpoint')
    # Exit the disposable QEMU at entry, before any Clean record can be written.
    gdb = work / 'interrupt-profile-completion.gdb'
    gdb.write_text('set pagination off\nset confirm off\nset architecture i386:x86-64\n'
                   f'target remote {work / "gdb.sock"}\nhbreak *0x{matches[0]}\ncontinue\n'
                   'printf "GUARD_QEMU_CUT_BEFORE_CLEAN\\n"\n'
                   'python\ntry:\n    gdb.execute("monitor quit")\n'
                   'except gdb.error:\n    pass\nend\n')
    return gdb


def copy_boot_logs(work, prefix):
    for kind in ('serial', 'qemu'):
        artifact = work / (kind + '.log')
        if artifact.is_file():
            shutil.copyfile(artifact, prefix.with_name(prefix.name + f'-{kind}.log'))


def boot_matrix_step(disk, firmware, step, marker, prefix, **kwargs):
    def disk_digest():
        with disk.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').digest()

    before = disk_digest()
    for attempt in range(1, 4):
        try:
            boot(disk, firmware, step, False, marker, 110, True, **kwargs)
            return
        except RuntimeError as error:
            serial_path = step / 'serial.log'
            serial = serial_path.read_bytes() if serial_path.exists() else b''
            # OVMF occasionally stops with only ANSI screen controls before
            # boot selection. Retry only when the disk is still byte-identical.
            firmware_stall = (
                str(error).startswith('timed out waiting for ') and serial and
                re.fullmatch(rb'(?:\x1b\[[0-9;=]*[A-Za-z]|\s)+', serial) and
                disk_digest() == before
            )
            if not firmware_stall or attempt == 3:
                raise
            copy_boot_logs(step, prefix.with_name(f'{prefix.name}-firmware-stall-{attempt}'))
            print(f'{prefix.name}: OVMF stalled before boot selection, retrying', flush=True)


def verify_emergency_gate(a, disk, work, guards):
    for name in ('f5', 'dirty-disk'):
        check = work / name
        check.mkdir()
        clone = check / 'disk.raw'
        shutil.copyfile(disk, clone)
        if name == 'dirty-disk':
            with clone.open('r+b') as stream:
                stream.seek(DATA_OFFSET + 1024 + 58)
                stream.write(b'\0\0')
        before = hashlib.sha256(clone.read_bytes()).digest()
        try:
            boot(clone, a.firmware, check, name == 'f5', RECOVERY_USB, 60, True, sata_snapshot=False,
                 sata_serial='BROOK-RECOVERY-QA', forbidden='Brook guard emergency generation',
                 failure_marker='*** KERNEL PANIC ***', hold_after_marker=1)
        finally:
            copy_boot_logs(check, a.output / f'emergency-{name}')
        serial = (check / 'serial.log').read_text(errors='replace')
        if ('Recovery required; normal boot blocked.' in serial) != (name == 'dirty-disk'):
            raise RuntimeError(f'{name}: incorrect independent disk guard verdict')
        if (hashlib.sha256(clone.read_bytes()).digest() != before or
                [read_fat(str(clone) + '@@1048576', f'RECOVERY/GUARD{j}.BIN') for j in range(2)] != guards):
            raise RuntimeError(f'{name}: refusing emergency selection changed the disk')


def run_matrix(a, base_disk, normal, work, guards, metadata, panic_journal):
    client = work / 'GDFRAME.ELF'
    source = Path(__file__).with_name('workloads') / 'guard_present_frames.c'
    subprocess.run(['cc', '-nostdlib', '-static', '-fno-stack-protector',
                    '-Wall', '-Wextra', '-Werror', '-O2', str(source), '-o', str(client)], check=True)
    framed = work / 'framed.img'
    shutil.copyfile(normal, framed)
    write_fat(str(framed), NATIVE_CLIENT.removeprefix('/'), client)

    bad = work / 'bad.img'
    shutil.copyfile(normal, bad)
    subprocess.run(['mdel', '-i', str(bad), '::/BOOT/INIT.RC'], check=True)
    kernel = a.desktop_pair / 'KERNEL.ELF'
    kernel_bytes = kernel.read_bytes()
    verdict = {}
    blank = (0, 0, 0)
    a0, a1, a2 = ((41, count, 0) for count in range(3))
    b0, b1, b2 = ((42, count, 0) for count in range(3))
    failed_a_profile, failed_b_profile = (41, 1, 1), (42, 1, 1)
    bad_b_first = (1, 1, 'failure', [blank, b1])
    bad_b_second = (1, 2, 'failure', [blank, b2])
    recovery = (None, None, 'recovery')
    repair = (None, None, 'repair')
    both_rejected = [
        (1, 1, 'profile', [blank, failed_b_profile]), (*repair, [blank, failed_b_profile]),
        (0, 1, 'profile', [failed_a_profile, failed_b_profile]),
        (None, None, 'repair-malformed-profile', [failed_a_profile, failed_b_profile]),
        (*recovery, [failed_a_profile, failed_b_profile]),
    ]
    scenarios = (
        ('fallback', (framed, bad), False, [bad_b_first, bad_b_second, (0, 1, 'success', [a0, b2])]),
        ('confirmed', (normal, framed), True, [(1, 2, 'success', [blank, b0])]),
        ('exhausted', (bad, bad), False, [bad_b_first, bad_b_second, (0, 1, 'failure', [a1, b2]),
                                         (0, 2, 'failure', [a2, b2]), (*recovery, [a2, b2])]),
        ('profile-rejected', (normal, normal), False, [(1, 1, 'profile', [blank, failed_b_profile]),
                                                       (*repair, [blank, failed_b_profile]),
                                                       (0, 1, 'profile', [failed_a_profile, failed_b_profile]),
                                                       (*repair, [failed_a_profile, failed_b_profile]),
                                                       (*recovery, [failed_a_profile, failed_b_profile])]),
        ('profile-repaired', (framed, normal), False, [(1, 1, 'profile', [blank, failed_b_profile]),
                                                       (*repair, [blank, failed_b_profile]),
                                                       (0, 1, 'success', [a0, b2])]),
        ('profile-both-repaired', (framed, normal), False, [
            *both_rejected, (None, None, 'repair-profile', [a1, b2]), (0, 2, 'success', [a0, b2])]),
        ('profile-retry-interrupted', (framed, normal), False, [
            *both_rejected, (None, None, 'cut-profile-retry', [a1, b2]),
            (None, None, 'resume-profile', [a1, b2]), (0, 2, 'success', [a0, b2])]),
        ('early-restart', (normal, normal), False, [(1, 1, 'early-restart', [blank, b1])]),
        ('checked-early-restart', (normal, framed), True, [(1, 2, 'checked-early-restart', [blank, b1])]),
        ('post-init-panic', (normal, normal), False, [(1, 1, 'panic', [blank, b1]), (*repair, [blank, b1])]),
        ('zero-journal', (normal, normal), False, [(1, None, 'empty-journal', None)]),
        ('emergency', (normal, normal), False, [(1, None, 'emergency', None)]),
        ('legacy-loader', (normal, normal), True, [(1, None, 'legacy-loader', [blank, b1])]),
    )
    for name, images, seed, starts in scenarios:
        if a.slot_fallback_case:
            if name not in a.slot_fallback_case:
                continue
        elif name in ('legacy-loader', 'profile-both-repaired', 'profile-retry-interrupted'):
            # These cases require an explicitly selected loader or recovery pair.
            continue
        scenario = work / name
        scenario.mkdir()
        disk = scenario / 'disk.raw'
        shutil.copyfile(base_disk, disk)
        media = str(disk) + '@@1048576'
        if name.startswith('profile-'):
            edit_profile_index(disk, scenario)
        for directory in ('UPDATES', 'UPDATES/A', 'UPDATES/B'):
            subprocess.run(['mmd', '-i', media, '::/' + directory], check=True)
        selectors = []
        for slot, (letter, initrd) in enumerate(zip('AB', images)):
            selector = record(GENERATIONS[slot], slot, kernel_bytes, initrd.read_bytes())
            selectors.append(selector)
            stored = scenario / f'SELECT{slot}.BIN'
            stored.write_bytes(selector)
            for entry, source in ((f'{letter}/KERNEL.ELF', kernel), (f'{letter}/INITRD.IMG', initrd),
                                  (stored.name, stored)):
                write_fat(media, 'UPDATES/' + entry, source)
        cfg = scenario / 'BROOK.CFG'
        config = read_fat(media, 'BROOK.CFG')
        if any(line.startswith(b'UPDATE_SLOTS=') for line in config.splitlines()):
            raise RuntimeError('pinned fixture unexpectedly enables update slots')
        cfg.write_bytes(config.rstrip(b'\r\n') + b'\nUPDATE_SLOTS=1\n')
        write_fat(media, 'BROOK.CFG', cfg)
        health = scenario / 'BOOTSTAT.BIN'
        raw_health = seed_health(a.identity, selectors, seed)
        if name == 'confirmed':
            # Simulate a completely torn write-target sector beside the valid strike.
            raw_health = raw_health[:512] + bytes([0xa5])*512
        if name == 'zero-journal':
            raw_health = b''
        elif name == 'emergency':
            raw_health = bytes(2048)
            subprocess.run(['mdel', '-i', media, '::/RECOVERY/SELECT.BIN'], check=True)
        if name == 'zero-journal':
            expect_legacy_bootstat(disk, 'absent')
        health.write_bytes(raw_health)
        write_fat(media, 'RECOVERY/BOOTSTAT.BIN', health)
        if name == 'zero-journal':
            expect_legacy_bootstat(disk, 'empty')
        if name == 'emergency':
            verify_emergency_gate(a, disk, scenario, guards)

        expected_guards = guards
        expected_panic = panic_journal
        sequence = int(seed)
        verdict[name] = []
        for i, (slot, count, result, expected) in enumerate(starts, 1):
            step = scenario / f'b{i}'
            step.mkdir()
            profile_retry = result in ('repair-profile', 'cut-profile-retry')
            if (name == 'profile-repaired' and result == 'success') or profile_retry:
                edit_profile_index(disk, scenario, restore=work / 'placeholder')
            if profile_retry:
                # The operator restored the missing file offline. Force the
                # next boot through guarded repair before it can retry a slot.
                with disk.open('r+b') as volume:
                    volume.seek(DATA_OFFSET + 1024 + 58)
                    volume.write(b'\0\0')
            before_health = read_fat(media, 'RECOVERY/BOOTSTAT.BIN')
            key = f'generation={GENERATIONS[slot]} slot={slot}' if slot is not None else ''
            start = f'GUARD_SLOT_START status=0 {key} starts={count}' if count is not None else None
            health_wait = f'GUARD_SLOT_HEALTH_WAIT {key} window_ms={HEALTH_WINDOW_MS}'
            early_restart = result in ('early-restart', 'checked-early-restart')
            qualifying_frames = result in ('success', 'checked-early-restart')
            repairing = result in ('repair', 'repair-profile', 'repair-malformed-profile', 'resume-profile')
            reset = repairing or result in ('failure', 'panic', 'cut-profile-retry') or early_restart
            evidence = 'GUARD_COMPLETE_RESTART' if repairing else CLEAN_RESET
            failure_marker = '*** KERNEL PANIC ***'
            gdb_script = on_marker = on_intermediate = intermediate = port = None
            prefix = a.output / f'{name}-{i}'
            if qualifying_frames or early_restart or result in ('empty-journal', 'emergency', 'legacy-loader'):
                with socket.socket() as probe:
                    probe.bind(('127.0.0.1', 0))
                    port = probe.getsockname()[1]
                shutdown = prefix.with_name(prefix.name + '-shutdown.json')
                frame_evidence = prefix.with_name(prefix.name + '-native-frames.json')

                def finish():
                    if result == 'checked-early-restart':
                        present_native_frames(step, port, frame_evidence)
                    elif result == 'early-restart':
                        poke_desktop(step)
                    finish_checked_shutdown(step, port, shutdown, restart=early_restart)

                on_marker = finish
            if result == 'failure':
                marker = f'GUARD_SLOT_RESTART requested=1 {key} stage=script status=-1'
                sequence += 1
            elif result == 'success':
                marker = f'GUARD_SLOT_CONFIRMED status=0 {key}'
                intermediate = health_wait
                on_intermediate = lambda: present_native_frames(step, port, frame_evidence)
                sequence += 2
            elif early_restart:
                marker = health_wait
                sequence += 2 if qualifying_frames else 1
            elif result == 'panic':
                marker = 'PANIC_DISK_CAPTURE_OK'
                evidence = 'PANIC_RECOVERY_RESET_WAIT'
                failure_marker = 'GUARD_DESKTOP_HANDOFF_FAILED'
                gdb_script = post_init_panic(kernel, step)
                sequence += 1
            elif result == 'profile':
                marker = 'GUARD_DESKTOP_HANDOFF_FAILED status=-5 shared_profile_rejected=1'
                sequence += 2
            elif result == 'cut-profile-retry':
                marker = 'GUARD_EXT2_VERIFIED_PENDING'
                evidence = 'GUARD_SLOT_PROFILE_RETRY status=0'
                gdb_script = interrupt_profile_completion(a.recovery_pair / 'KERNEL.ELF', step)
                sequence += 1
            elif repairing:
                marker = 'GUARD_COMPLETE_RESTART'
                if profile_retry:
                    sequence += 1
            elif result == 'recovery':
                marker = 'GUARD_AUTO_NO_REPAIR'
            else:
                marker = 'GUARD_DESKTOP_HANDOFF_SCRIPT status=0'
            try:
                boot_matrix_step(
                    disk, a.firmware, step, marker, prefix, sata_snapshot=False,
                    sata_serial='BROOK-RECOVERY-QA', extra_fw_cfg=[RESET_OPTION], expect_reset=reset,
                    reset_evidence_marker=evidence, on_marker=on_marker, network_port=port,
                    failure_marker=failure_marker, hold_after_marker=1 if not reset else 0,
                    intermediate_marker=intermediate, on_intermediate=on_intermediate,
                    gdb_script=gdb_script)
            finally:
                copy_boot_logs(step, prefix)
            serial = (step/'serial.log').read_text(errors='replace')
            slot_starts = re.findall(r'GUARD_SLOT_START [^\r\n]+', serial)
            if slot_starts != ([start] if start else []):
                raise RuntimeError(f'{name}-{i}: wrong guarded start: {slot_starts}')
            fallback = 'Brook guard fallback generation 41 slot 0 verified'
            if (fallback in serial) != (slot == 0):
                raise RuntimeError(f'{name}-{i}: unexpected firmware fallback')
            if result == 'failure' and ('GUARD_DESKTOP_HANDOFF_SCRIPT status=-1' not in serial or
                                        'GUARD_SLOT_CONFIRMED' in serial):
                raise RuntimeError(f'{name}-{i}: did not fail at the startup-script checkpoint')
            if result == 'success':
                measured = re.search(r'GUARD_SLOT_CONFIRMED status=0 ' + key + r' healthy_ms=(\d+) presents=(\d+)', serial)
                if (not measured or int(measured[1]) < HEALTH_WINDOW_MS or int(measured[2]) < 2 or
                        'GUARD_DESKTOP_HANDOFF_SCRIPT status=0' not in serial or CLEAN_RESET not in serial):
                    raise RuntimeError(f'{name}-{i}: confirmation lacked checked sync or positive desktop health')
            if early_restart or result == 'panic':
                if ('GUARD_DESKTOP_HANDOFF_SCRIPT status=0' not in serial or health_wait not in serial or
                        'GUARD_SLOT_CONFIRMED' in serial):
                    raise RuntimeError(f'{name}-{i}: the post-INIT interruption did not retain its strike')
            withdrawal = re.findall(r'GUARD_SLOT_CHECKED_SHUTDOWN [^\r\n]+', serial)
            withdrawal_presents = None
            if result == 'checked-early-restart':
                pattern = rf'GUARD_SLOT_CHECKED_SHUTDOWN status=0 {key} presents=(\d+)'
                measured = re.fullmatch(pattern, withdrawal[0]) if len(withdrawal) == 1 else None
                if not measured or int(measured[1]) < 2:
                    raise RuntimeError(f'{name}-{i}: checked restart did not withdraw one qualified start: {withdrawal}')
                withdrawal_presents = int(measured[1])
            elif early_restart and withdrawal:
                raise RuntimeError(f'{name}-{i}: kernel-only redraws withdrew a start: {withdrawal}')
            if result == 'panic':
                packet = next((line[10:] for line in serial.splitlines() if line.startswith('PANIC_HEX:')), None)
                if not packet or 'GUARD_DESKTOP_PANIC_RESET armed=1' not in serial:
                    raise RuntimeError('the post-INIT panic did not produce a recovery packet')
                expected_panic = read_fat(media, 'PANIC.BIN')
                verify_record(expected_panic, 0, 1, bytes.fromhex(packet))
            if result == 'profile' and (f'GUARD_SLOT_PROFILE_REJECTED status=0 {key}' not in serial or
                                        'GUARD_SLOT_RESTART' in serial or 'GUARD_DESKTOP_HANDOFF_SCRIPT' in serial):
                raise RuntimeError(f'{name}-{i}: shared profile rejection entered a slot-specific retry')
            if result == 'profile':
                diagnostics = re.findall(r'^ahci direct: handoff failed [^\r\n]+', serial, re.M)
                expected_diagnostic = ('ahci direct: handoff failed step=profile-admission status=-2 '
                                       'path=/nix/index/packages.idx check=stat cleanup_status=0')
                if diagnostics != [expected_diagnostic]:
                    raise RuntimeError(f'{name}-{i}: missing or incorrect admission diagnostic: {diagnostics}')
            if result in ('recovery', 'repair', 'repair-malformed-profile', 'resume-profile') and (
                    'Brook SSD recovery payload verified' not in serial or
                                                      'GUARD_SLOT_CONFIRMED' in serial or
                                                      read_fat(media, 'RECOVERY/BOOTSTAT.BIN') != before_health):
                raise RuntimeError(f'{name}-{i}: recovery-only boot changed the slot journal')
            if result == 'empty-journal' and f'GUARD_SLOT_JOURNAL status=1 {key}' not in serial:
                raise RuntimeError('empty legacy journal was not reported as absent')
            if result == 'emergency' and ('Brook guard emergency generation 42 slot 1 verified' not in serial or
                                          'GUARD_SLOT_UNTRACKED reason=emergency-loader' not in serial):
                raise RuntimeError('verified slot did not run untracked in the emergency path')
            if result == 'legacy-loader' and ('Brook update generation 42 verified' not in serial or
                                               'GUARD_SLOT_UNTRACKED reason=no-loader-key' not in serial):
                raise RuntimeError('previous loader could not start the verified slot untracked')
            if result in ('empty-journal', 'emergency', 'legacy-loader') and (
                    read_fat(media, 'RECOVERY/BOOTSTAT.BIN') != before_health or CLEAN_RESET not in serial):
                raise RuntimeError(f'{name}-{i}: untracked desktop charged a slot or failed checked sync')
            guarded_records = [read_fat(media, f'RECOVERY/GUARD{j}.BIN') for j in range(2)]
            if result == 'cut-profile-retry':
                if ('GUARD_QEMU_CUT_BEFORE_CLEAN' not in (step / 'qemu.log').read_text(errors='replace') or
                        'Brook SSD recovery payload verified' not in serial or
                        'GUARD_ATTEMPT_NUMBER 1' not in serial or 'GUARD_COMPLETE_RESULT' in serial):
                    raise RuntimeError('recovery did not stop after requalification and before guard completion')
                pending = bytearray(expected_guards[0])
                generation = struct.unpack_from('<Q', pending, 8)[0]
                struct.pack_into('<QII', pending, 8, generation + 1, 2, 1)
                struct.pack_into('<I', pending, 124, zlib.crc32(pending[:124]))
                expected_guards = [pending, pending]
            if result == 'resume-profile' and (
                    'Recovery required; normal boot blocked.' not in serial or
                    'GUARD_PENDING_RESULT outcome=2' not in serial or 'GUARD_ATTEMPT_NUMBER 2' not in serial or
                    'GUARD_SLOT_PROFILE_RETRY' in serial):
                raise RuntimeError('recovery failed to finish a pending guard without requalifying the slot again')
            if repairing:
                needs_check = 'GUARD_AUTO_NEEDS_CHECK' in serial
                if (needs_check != (result != 'resume-profile') or 'GUARD_COMPLETE_RESULT outcome=2' not in serial or
                        guarded_records[0] != guarded_records[1]):
                    raise RuntimeError('shared profile recovery did not complete the guarded data repair')
                clean_guard = guarded_records[0]
                if (len(clean_guard) != 128 or struct.unpack_from('<II', clean_guard, 16) != (1, 0) or
                        zlib.crc32(clean_guard[:124]) != struct.unpack_from('<I', clean_guard, 124)[0]):
                    raise RuntimeError('guarded profile recovery did not publish a clean guard')
                expected_guards = guarded_records
                if result == 'repair-profile' and (
                        'GUARD_SLOT_PROFILE_RECHECK accepted=1' not in serial or
                        'GUARD_SLOT_PROFILE_RETRY status=0' not in serial or
                        serial.index('GUARD_SLOT_PROFILE_RETRY status=0') > serial.index('GUARD_COMPLETE_RESULT')):
                    raise RuntimeError('checked repair did not reopen the verified older slot')
                if result == 'repair-malformed-profile' and ('GUARD_SLOT_PROFILE_RECHECK accepted=0' not in serial or
                                                              'GUARD_SLOT_PROFILE_RETRY' in serial):
                    raise RuntimeError('checked repair unexpectedly reopened a slot with a malformed profile')
            if expected is None:
                report = dict(sequence=None, slots=[])
            else:
                prior = [blank, b2] if result == 'checked-early-restart' else None
                report = read_health(media, a.identity, selectors, sequence, expected, prior_slots=prior)
            if (metadata_hash(disk) != metadata or read_fat(media, 'PANIC.BIN') != expected_panic or
                    guarded_records != expected_guards or
                    [read_fat(media, f'UPDATES/SELECT{j}.BIN') for j in range(2)] != selectors):
                raise RuntimeError(f'{name}-{i}: shared guarded boot data changed')
            clean, fsck_exit = check_volumes(
                disk, scenario, prefix.with_name(prefix.name + '-fsck.log'),
                expect_clean=result not in ('profile', 'panic'), expect_repair=result == 'panic')
            verdict[name].append(dict(result=result, generation=GENERATIONS[slot] if slot is not None else None,
                                      slot=slot, starts=count, fat_clean=True, ext2_clean=clean,
                                      ext2_fsck_exit=fsck_exit,
                                      checked_shutdown_presents=withdrawal_presents,
                                      **report))
            (a.output/'slot-matrix.json').write_text(json.dumps(verdict, indent=2) + '\n')
    print('GUARD_SLOT_FALLBACK_MATRIX_PASS', flush=True)
