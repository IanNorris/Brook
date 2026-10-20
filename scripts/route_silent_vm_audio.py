#!/usr/bin/env python3
"""Route one agent-owned Brook VM to its isolated PipeWire null sink.

The null daemon deliberately has no session manager. Configure and link only
nodes belonging to the validated QEMU PID; otherwise callback HDA can stall.
Use routed.json as the readiness gate before launching playback workloads.
"""
import argparse
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/vm_bridge'))
from common import process_is_alive, write_json
import server
from linux_workload_reference import SilentRoutes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', required=True)
    parser.add_argument('--audio-daemon', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=3600,
                        choices=range(10, 14401), metavar='10..14400')
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / 'build'):
        parser.error('Keep routing evidence under the ignored build directory')
    descriptor = server.load_session(args.session)
    if descriptor.get('provenance') != 'agent':
        parser.error('This benchmark helper requires an agent-owned VM')
    if not process_is_alive(descriptor['pid'], descriptor['process_identity']):
        parser.error('The selected QEMU process is no longer live')
    output.mkdir(parents=True, exist_ok=False)
    routes = SilentRoutes(args.audio_daemon.resolve(strict=True))
    write_json(output / 'identity.json', dict(session=args.session,
               pid=descriptor['pid'], process_identity=descriptor['process_identity'],
               audio_daemon=str(args.audio_daemon.resolve())))
    deadline = time.monotonic() + args.timeout
    try:
        while process_is_alive(descriptor['pid'], descriptor['process_identity']):
            if time.monotonic() >= deadline:
                raise TimeoutError('Routing observation reached its bound; links remain owned by PipeWire')
            if (output / 'stop').exists():
                raise RuntimeError('Routing observer stopped before the selected VM exited')
            routes.update(descriptor['pid'])
            write_json(output / 'routes.json', routes.events)
            if routes.events and not (output / 'routed.json').exists():
                write_json(output / 'routed.json', dict(session=args.session,
                           host_monotonic_ns=time.monotonic_ns(), events=routes.events))
                print('SILENT_VM_AUDIO_ROUTED', args.session, flush=True)
            time.sleep(.25)
    except Exception as error:
        write_json(output / 'exit.json', dict(passed=False, error=str(error),
                   routes=routes.events))
        raise
    write_json(output / 'exit.json', dict(passed=True, routes=routes.events,
               qemu_process_terminal=True))
    print('SILENT_VM_AUDIO_STOPPED', args.session, flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
