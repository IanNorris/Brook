#!/usr/bin/env python3
"""Collect Dell crash snapshots once, or watch an explicitly supplied endpoint."""
import argparse
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools' / 'vm_bridge')]
from hardware import Hardware, HardwareBootChanged
from hardware_panic_collect import PanicCollector


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', required=True, help='Explicit Brook IPv4 address')
    p.add_argument('--port', type=int, default=1234)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--watch', action='store_true', help='Poll until interrupted; default is one check')
    p.add_argument('--interval', type=int, default=60, help='Watch interval in seconds (minimum 60)')
    args = p.parse_args()
    if args.interval < 60:
        p.error('Watch interval must be at least 60 seconds')
    bridge = Hardware()
    bridge.endpoint(args.host, args.port)
    collector = PanicCollector(bridge, args.output)
    session = None
    while True:
        try:
            if session is None:
                attached = bridge.attach(args.host, args.port)
                session = attached['session']
                print(json.dumps({'state': 'attached', 'boot_id': attached['guest_boot_id'],
                                  'session': session}), flush=True)
            rows = collector.collect(session)
            print(json.dumps({'state': 'checked', 'session': session, 'records': rows}), flush=True)
        except HardwareBootChanged:
            session = None
            print(json.dumps({'state': 'boot_changed', 'message': 'Reattach on the next check'}), flush=True)
            if not args.watch:
                return 1
        except (OSError, RuntimeError, ValueError) as error:
            print(json.dumps({'state': 'unavailable', 'error': str(error),
                              'message': 'Connection failure does not identify a crash'}), flush=True)
            if not args.watch:
                return 1
        if not args.watch:
            return 0
        time.sleep(args.interval)


if __name__ == '__main__':
    sys.exit(main())
