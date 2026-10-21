#!/usr/bin/env python3
"""Bounded GL3 demo timing using locally supplied, licensed Quake II data.

No game data is bundled. A short derived demo stays in the fresh private home.
Run in an isolated test VM; log visibility and reader scheduling affect timing.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import urllib.request


def notify_marker(url, name):
    """Receive an independent host timestamp; transport remains in the interval."""
    before = time.monotonic_ns()
    with urllib.request.urlopen(url.rstrip('/') + '/' + name, timeout=10) as response:
        receipt = json.load(response)
    after = time.monotonic_ns()
    if not isinstance(receipt, dict):
        raise ValueError('Invalid host marker acknowledgement')
    host = receipt.get('host_ns')
    if type(host) is not int or host <= 0 or receipt.get('marker') != name:
        raise ValueError('Invalid host marker acknowledgement')
    return dict(before_guest_ns=before, after_guest_ns=after, host_ns=host)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--home', type=Path, required=True)
    parser.add_argument('--uncapped', action='store_true',
                        help='Benchmark with timedemo=1 and r_vsync=0; bypass engine frame pacing')
    parser.add_argument('--measured-frames', type=int, default=150,
                        help='Original demo frame records after 49 warmup records (default: 150)')
    parser.add_argument('--engine', type=Path, help='Direct engine path for the Linux reference; default uses Brook wrapper')
    parser.add_argument('--pak-dir', type=Path,
                        default=Path('/data/games/quake2/baseq2'))
    parser.add_argument('--music-track', type=int, choices=range(2, 22),
                        help='Restart this locally supplied OGG track at the START marker')
    parser.add_argument('--music-only', action='store_true',
                        help='Mute sound effects for a music-track capture')
    parser.add_argument('--marker-url',
                        help='Optional host HTTP endpoint acknowledging /START and /END '
                             'with JSON marker and host_ns fields')
    args = parser.parse_args()
    if not 1 <= args.measured_frames <= 2000:
        parser.error('--measured-frames must be between 1 and 2000')
    total_frames = 50 + args.measured_frames
    if args.music_only and args.music_track is None:
        parser.error('--music-only requires --music-track')
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
        if len(frame_records) >= total_frames:
            break
    assert len(frame_records) == total_frames, 'Owned demo is shorter than requested capture'
    first, last = frame_records[49][0], frame_records[total_frames-1][0]
    measured = frame_records[49:total_frames]
    assert all(b[1] > a[1] for a, b in zip(measured, measured[1:])), 'Non-increasing frame numbers'
    server_frame_delta = measured[-1][1] - measured[0][1]

    def marker(text):
        return b'\x0b' + text.encode() + b'\n\0'  # svc_stufftext

    start_command = 'echo BROOK_DEMO_TIMING_START'
    if args.music_track is not None:
        track = args.pak_dir / 'music' / ('track%02d.ogg' % args.music_track)
        if not track.is_file():
            raise SystemExit('Requested locally supplied music track is missing: ' + str(track))
        start_command = ('ogg stop; ogg play %d; ogg info; ' % args.music_track
                         + start_command)
    packets = (records[:first] + [marker(start_command)]
               + records[first:last] + [marker('echo BROOK_DEMO_TIMING_END; quit')])
    with (demo_dir / 'brook-timing.dm2').open('wb') as stream:
        for packet in packets:
            stream.write(struct.pack('<i', len(packet)))
            stream.write(packet)
        stream.write(struct.pack('<i', -1))
    metadata = dict(recorded_frame_delta=server_frame_delta, measured_frame_records=args.measured_frames,
                    measured_demo_records=last-first,
                    uncapped=args.uncapped,
                    music_track=args.music_track,
                    music_only=args.music_only,
                    inserted_start_record=1,
                    expected_server_interval_s=None if args.uncapped else (server_frame_delta+1)/10,
                    scope=str(args.measured_frames) + ' original frame records with embedded packets and frame-number gaps preserved, plus the start marker tick; '
                          'log observations include reader scheduling latency')
    print('Q2_TIMING_PREPARED', json.dumps(metadata), flush=True)
    if args.engine:
        if not args.engine.is_absolute() or not args.engine.is_file():
            raise SystemExit('Direct engine must be an existing absolute executable')
        os.environ.update(HOME=str(home), XDG_CONFIG_HOME=str(home/'.config'),
                          XDG_CACHE_HOME=str(home/'.cache'), SDL_VIDEODRIVER='wayland')
        prefix = [str(args.engine), '-datadir', str(args.pak_dir.parent),
                  '+set', 'logfile', '2', '+set', 'vid_renderer', 'gl3']
    else:
        wrapper = (Path('/nix/bin/brook-yquake2-gl').read_text()
                   .replace('/data/yq2', str(home))
                   .replace('/data/games/quake2', str(args.pak_dir.parent)))
        prefix = ['/nix/profile/bin/bash', '-c', wrapper, 'q2-timing', 'gl3']
    argv = prefix + [
            '+set', 'r_mode', '-1', '+set', 'r_customwidth', '1280',
            '+set', 'r_customheight', '720', '+set', 'vid_fullscreen', '0',
            '+set', 'cl_maxfps', '60', '+set', 'timedemo', '0',
            '+set', 'nextdemo', 'quit', '+demomap', 'brook-timing.dm2']
    if args.uncapped:
        # Yamagi 8.60 separates packet (cl_maxfps) and render (vid_maxfps)
        # limits. Its true bypass is timedemo: common/frame.c skips both
        # pacing tests. vid_maxfps=0 alone would be clamped to 999, not infinity.
        argv = prefix + [
            '+set', 'r_vsync', '0',
        ] + argv[len(prefix):]
        argv[argv.index('cl_maxfps')+1] = '250'
        timedemo = argv.index('timedemo')
        argv[timedemo+1] = '1'
        # The wrapper adds arguments of its own and Yamagi accepts at most 50.
        # Keep initialization-sensitive values above; use a private late script
        # for the remaining settings and queries instead of overflowing argv.
        queries = 'set vid_maxfps 999\nset cl_async 1\n'
        if args.music_only:
            queries += 'set s_volume 0\nset ogg_volume 0.7\n'
        queries += 'r_vsync\nvid_maxfps\ncl_maxfps\ncl_async\ntimedemo\n'
        query_name = 'brook-benchmark-query.cfg'
        (demo_dir.parent / query_name).write_text(queries)
        # Print effective cvars into the retained log as an independent check
        # of command-line intent; saved configuration remains another check.
        argv += ['+exec', query_name]
    if args.music_only and not args.uncapped:
        # OpenAL streams use ogg_volume separately from the effects gain.
        argv = prefix + ['+set', 's_volume', '0', '+set', 'ogg_volume', '0.7'] + argv[len(prefix):]
    started = time.monotonic_ns()
    markers, host_markers, cursor, pending = {}, {}, 0, b''

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
                    if args.marker_url:
                        host_markers[name] = notify_marker(args.marker_url, name)

    with (home / 'timing-stdout.log').open('wb') as output:
        process = subprocess.Popen(argv, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + max(110, args.measured_frames/5+30)
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
    if args.marker_url:
        result['host_markers'] = host_markers
        result['host_timing_scope'] = ('HTTP acknowledgement timestamps on the host; '
                                      'includes guest log-reader and transport latency; '
                                      'not exact rendered-frame throughput')
        if set(host_markers) == {'START', 'END'}:
            interval = host_markers['END']['host_ns'] - host_markers['START']['host_ns']
            if interval <= 0:
                raise ValueError('Non-increasing host marker timestamps')
            result['host_marker_interval_s'] = interval / 1e9
    (home / 'timing-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    assert exit_code == 0 and set(markers) == {'START', 'END'}, result
    if not args.uncapped:
        assert result['observed_demo_interval_s'] > min(5, (args.measured_frames+1)/20), 'Buffered markers invalidate timing'


if __name__ == '__main__':
    main()
