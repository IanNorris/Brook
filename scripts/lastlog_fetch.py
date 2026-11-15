#!/usr/bin/env python3
"""Fetch the previous boot's last-log record (kernel serial tail + compat census).

The guest keeps the newest valid record found on disk before its first write
in RAM and serves it with "lastlog previous OFFSET COUNT". This reads it in
4 KiB chunks through the existing bridge for one explicit session and writes
the text plus a small JSON receipt. Read-only on the guest.

  scripts/lastlog_fetch.py --vm SESSION --output DIR
  scripts/lastlog_fetch.py --hw SESSION --output DIR   (attach with hw_attach first)
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools' / 'vm_bridge')]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument('--vm', help='QEMU bridge session')
    target.add_argument('--hw', help='Attached hardware bridge session')
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--timeout', type=float, default=30)
    args = p.parse_args()

    if args.vm:
        import server
        data = server.load_session(args.vm)
        ask = lambda command: server.guest(data, command, timeout=args.timeout)
    else:
        from hardware import Hardware
        bridge = Hardware()
        ask = lambda command: bridge.command(args.hw, command)

    status = ask('lastlog status')
    if status.get('lastlog_api') != 1:
        raise SystemExit(f'Guest has no lastlog support: {status}')
    text, offset, record = [], 0, None
    while True:
        chunk = ask(f'lastlog previous {offset} 4096')
        if chunk.get('lastlog_api') != 1:
            raise SystemExit(chunk.get('lastlog_error', f'Unexpected reply: {chunk}'))
        if record is None:
            record = chunk['record']
        elif chunk['record'] != record:
            raise SystemExit('Previous-boot record changed while reading')
        if chunk['offset'] != offset:
            raise SystemExit('Unexpected chunk offset')
        text.append(chunk['text'])
        offset += chunk['count']
        if not chunk['more'] or not chunk['count']:
            break
    body = ''.join(text)
    if len(body.encode('latin-1', 'replace')) != record['bytes']:
        raise SystemExit(f'Short read: {len(body)} of {record["bytes"]} bytes')
    args.output.mkdir(parents=True, exist_ok=True)
    name = f'lastlog-seq{record["sequence"]}-{record["boot_id"]}'
    (args.output / f'{name}.txt').write_text(body, encoding='utf-8', errors='replace')
    receipt = {'record': record, 'current_status': status, 'text_path': str((args.output / f'{name}.txt').absolute())}
    (args.output / f'{name}.json').write_text(json.dumps(receipt, indent=1) + '\n')
    print(json.dumps(receipt, indent=1))


if __name__ == '__main__':
    main()
