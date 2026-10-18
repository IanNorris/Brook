#!/usr/bin/env python3
"""Guest DRM PRIME identity regression using one tiny source resource.

Repeated imports of the same resource into one render FD must return the same
GEM handle, even through separate exported FDs. This does not test exporter
lifetime, shared mmap backing or safe host-resource retirement.
"""
import argparse
import errno
import fcntl
import json
import os
import struct
import threading


def ioctl(fd, number, payload):
    data = bytearray(payload)
    fcntl.ioctl(fd, (3 << 30) | (len(data) << 16) | (ord('d') << 8) | number, data, True)
    return data


def export(fd, handle):
    return struct.unpack('<IIi', ioctl(fd, 0x2d, struct.pack('<IIi', handle, 0, -1)))[2]


def imported(fd, prime):
    return struct.unpack('<IIi', ioctl(fd, 0x2e, struct.pack('<IIi', 0, 0, prime)))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest', required=True, action='store_true')
    parser.add_argument('--imports', type=int, default=3000)
    parser.add_argument('--race-iterations', type=int, default=256)
    args = parser.parse_args()
    if not 1 <= args.imports <= 16000:
        parser.error('imports must be between 1 and 16000')
    if not 1 <= args.race_iterations <= 10000:
        parser.error('race-iterations must be between 1 and 10000')
    source = os.open('/dev/dri/renderD128', os.O_RDWR)
    target = os.open('/dev/dri/renderD128', os.O_RDWR)
    primes = []
    completed = 0
    handles = set()
    failure = None
    same_fd_pass = False
    alias_pass = False
    own_pass = False
    close_race = None
    try:
        create = struct.unpack('<14I', ioctl(source, 0x44, struct.pack(
            '<14I', 0, 64, 1 << 4, 128, 1, 1, 1, 0, 0, 0, 0, 0, 128, 0)))
        handle, resource = create[10:12]
        primes.append(export(source, handle))
        for i in range(args.imports):
            try:
                handles.add(imported(target, primes[0]))
                completed += 1
            except OSError as error:
                failure = {'index': i, 'errno': error.errno, 'error': str(error)}
                break
        same_fd_pass = completed == args.imports and len(handles) == 1
        if same_fd_pass:
            expected = next(iter(handles))
            for _ in range(16):
                primes.append(export(source, handle))
                assert imported(target, primes[-1]) == expected, 'Separate PRIME FDs changed GEM identity'
            alias_pass = True
            own_pass = imported(source, primes[0]) == handle
            assert own_pass, 'Import into the exporting render FD changed its GEM identity'
            info = struct.unpack('<4I', ioctl(target, 0x45, struct.pack('<4I', expected, 0, 0, 0)))
            assert info[1:3] == (resource, 128), 'Imported resource metadata changed'
            # Import while another thread closes the PRIME capability. Either
            # the pin wins and metadata remains valid, or close wins cleanly.
            barrier = threading.Barrier(2, timeout=30)
            active = [None]
            close_race = {'imported': 0, 'closed_before_pin': 0, 'errors': []}

            def importer():
                try:
                    for _ in range(args.race_iterations):
                        barrier.wait()
                        try:
                            raced = imported(target, active[0])
                            info = struct.unpack('<4I', ioctl(target, 0x45, struct.pack('<4I', raced, 0, 0, 0)))
                            if raced != expected or info[1:3] != (resource, 128):
                                close_race['errors'].append('import metadata or handle changed during close')
                            close_race['imported'] += 1
                        except OSError as error:
                            if error.errno not in (errno.EBADF, errno.EINVAL):
                                close_race['errors'].append(str(error))
                            else:
                                close_race['closed_before_pin'] += 1
                        finally:
                            barrier.wait()
                except Exception as error:
                    close_race['errors'].append(repr(error))
                    barrier.abort()

            worker = threading.Thread(target=importer, daemon=True)
            worker.start()
            raced_fd = None
            race_finished = False
            try:
                for _ in range(args.race_iterations):
                    raced_fd = export(source, handle)
                    active[0] = raced_fd
                    barrier.wait()
                    os.close(raced_fd)
                    raced_fd = None
                    barrier.wait()
                race_finished = True
            finally:
                if raced_fd is not None:
                    os.close(raced_fd)
                if not race_finished:
                    barrier.abort()
                worker.join(timeout=30)
                if worker.is_alive():
                    barrier.abort()
                    worker.join(timeout=1)
            assert not worker.is_alive() and not close_race['errors'], close_race
            assert close_race['imported'] + close_race['closed_before_pin'] == args.race_iterations
            ioctl(target, 0x09, struct.pack('<II', expected, 0))
            handles.clear()
            # Closing the GEM removes that handle. A surviving PRIME capability
            # can be imported again; this must not return an invalid stale GEM.
            fresh = imported(target, primes[0])
            handles.add(fresh)
            info = struct.unpack('<4I', ioctl(target, 0x45, struct.pack('<4I', fresh, 0, 0, 0)))
            assert info[1:3] == (resource, 128), 'Reimport after GEM_CLOSE returned a stale handle'
        result = {'requested': args.imports, 'completed': completed, 'unique_gem_handles': len(handles),
                  'same_fd_pass': same_fd_pass, 'separate_export_fds_pass': alias_pass,
                  'import_into_exporter_pass': own_pass, 'failure': failure, 'close_race': close_race,
                  'resource_count': 1, 'source_bytes': 128}
        print('GPU_PRIME_IMPORT_RESULT', json.dumps(result), flush=True)
        return int(not (same_fd_pass and alias_pass and own_pass))
    finally:
        for prime in primes:
            os.close(prime)
        os.close(target)
        os.close(source)


if __name__ == '__main__':
    raise SystemExit(main())
