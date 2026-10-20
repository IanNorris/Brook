#!/usr/bin/env python3
"""Bounded GL3 demo timing using locally supplied, licensed Quake II data.

No game data is bundled. A short derived demo stays in the fresh private home.
Run in an isolated test VM; log visibility and reader scheduling affect timing.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--home', type=Path, required=True)
    parser.add_argument('--pak-dir', type=Path,
                        default=Path('/data/games/quake2/baseq2'))
    args = parser.parse_args()
    home = args.home
    if not home.is_absolute() or home.exists() or not args.pak_dir.is_absolute():
        raise SystemExit('Use a fresh absolute home and an absolute pak directory')
    # These paths are substituted into the installed, trusted shell wrapper.
    allowed = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._-'
    if any(c not in allowed for c in str(home) + str(args.pak_dir)):
        raise SystemExit('Use only letters, digits, slash, dot, underscore or hyphen')
    demo_dir = home / '.yq2/baseq2/demos'
    demo_dir.mkdir(parents=True)
    source = None
    for pak in sorted(args.pak_dir.glob('pak*.pak')):
        with pak.open('rb') as stream:
            magic, offset, size = struct.unpack('<4sii', stream.read(12))
            assert magic == b'PACK' and 0 < size < 2**22 and size % 64 == 0
            stream.seek(offset)
            directory = stream.read(size)
            for index in range(0, size, 64):
                name, start, length = struct.unpack('<56sii', directory[index:index+64])
                if name.split(b'\0')[0] == b'demos/demo1.dm2':
                    assert 0 < length < 2**26
                    stream.seek(start)
                    source = stream.read(length)
                    assert len(source) == length
                    break
        if source is not None:
            break
    assert source is not None, 'Owned Quake II demo1 not found'
    records, frame_records = [], []
    position = 0
    while position + 4 <= len(source):
        length = struct.unpack_from('<i', source, position)[0]
        position += 4
        if length == -1:
            break
        assert 0 <= length <= 65536 and position + length <= len(source)
        packet = source[position:position+length]
        position += length
        if packet[:1] == b'\x14':  # svc_frame at the start of this packet
            frame_records.append((len(records), struct.unpack_from('<i', packet, 1)[0]))
        records.append(packet)
        if len(frame_records) >= 200:
            break
    assert len(frame_records) == 200
    first, last = frame_records[49][0], frame_records[199][0]
    assert last - first == 150, 'Unexpected non-frame records in measured section'
    assert frame_records[199][1] - frame_records[49][1] == 150, 'Nonconsecutive frames'

    def marker(text):
        return b'\x0b' + text.encode() + b'\n\0'  # svc_stufftext

    packets = (records[:first] + [marker('echo BROOK_DEMO_TIMING_START')]
               + records[first:last] + [marker('echo BROOK_DEMO_TIMING_END; quit')])
    with (demo_dir / 'brook-timing.dm2').open('wb') as stream:
        for packet in packets:
            stream.write(struct.pack('<i', len(packet)))
            stream.write(packet)
        stream.write(struct.pack('<i', -1))
    metadata = dict(recorded_frame_delta=150, measured_demo_records=150,
                    inserted_start_record=1, expected_server_interval_s=15.1,
                    scope='150 original packets plus the start marker tick; '
                          'log observations include reader scheduling latency')
    print('Q2_TIMING_PREPARED', json.dumps(metadata), flush=True)
    wrapper = (Path('/nix/bin/brook-yquake2-gl').read_text()
               .replace('/data/yq2', str(home))
               .replace('/data/games/quake2', str(args.pak_dir.parent)))
    argv = ['/nix/profile/bin/bash', '-c', wrapper, 'q2-timing', 'gl3',
            '+set', 'r_mode', '-1', '+set', 'r_customwidth', '1280',
            '+set', 'r_customheight', '720', '+set', 'vid_fullscreen', '0',
            '+set', 'cl_maxfps', '60', '+set', 'timedemo', '0',
            '+set', 'nextdemo', 'quit', '+demomap', 'brook-timing.dm2']
    started = time.monotonic_ns()
    markers, cursor, pending = {}, 0, b''

    def inspect_log():
        nonlocal cursor, pending
        path = home / '.yq2/baseq2/qconsole.log'
        if not path.exists() or path.stat().st_size <= cursor:
            return
        before = time.monotonic_ns()
        with path.open('rb') as stream:
            stream.seek(cursor)
            data = stream.read(65536)
        after = time.monotonic_ns()
        cursor += len(data)
        lines = (pending + data).split(b'\n')
        pending = lines.pop()
        for line in lines:
            for name in ('START', 'END'):
                if (line.strip() == ('BROOK_DEMO_TIMING_' + name).encode()
                        and name not in markers):
                    markers[name] = dict(before_ns=before, after_ns=after)
                    print('Q2_MARKER', name, after, flush=True)

    with (home / 'timing-stdout.log').open('wb') as output:
        process = subprocess.Popen(argv, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 110
        try:
            while process.poll() is None:
                inspect_log()
                if time.monotonic() > deadline:
                    raise TimeoutError('Bounded game deadline')
                time.sleep(.02)
            inspect_log()
            exit_code = process.wait(timeout=10)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    result = dict(**metadata, exit=exit_code, markers=markers,
                  launch_to_exit_s=(time.monotonic_ns()-started)/1e9,
                  observation='Per-line-flushed qconsole.log; 20ms guest polling; '
                              'includes reader scheduling and filesystem visibility delay')
    if set(markers) == {'START', 'END'}:
        result['observed_demo_interval_s'] = (
            markers['END']['after_ns'] - markers['START']['after_ns']) / 1e9
    (home / 'timing-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    assert exit_code == 0 and set(markers) == {'START', 'END'}, result
    assert result['observed_demo_interval_s'] > 5, 'Buffered markers invalidate timing'


if __name__ == '__main__':
    main()
