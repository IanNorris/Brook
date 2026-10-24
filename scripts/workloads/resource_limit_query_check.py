#!/usr/bin/env python3
"""Compare x86-64 Linux resource-limit query entry points without changing limits."""
import argparse
import ctypes as c
import json
import platform
import resource


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest', action='store_true')
    parser.add_argument('--report-only', action='store_true')
    args = parser.parse_args()
    assert platform.machine() == 'x86_64'
    libc = c.CDLL(None, use_errno=True)
    libc.syscall.restype = c.c_long
    cases = {}
    values = []
    for number in (0, 1, 2, 3, 7, 9):
        legacy = (c.c_uint64 * 2)()
        modern = (c.c_uint64 * 2)()
        assert libc.syscall(c.c_long(97), c.c_ulong(number), c.byref(legacy)) == 0
        assert libc.syscall(c.c_long(302), c.c_ulong(0), c.c_ulong(number),
                            c.c_void_p(), c.byref(modern)) == 0
        cases[f'query_agreement_{number}'] = list(legacy) == list(modern)
        python_value = tuple(x & ((1 << 64) - 1) for x in resource.getrlimit(number))
        cases[f'libc_agreement_{number}'] = python_value == tuple(legacy)
        values.append(dict(resource=number, legacy=list(legacy), modern=list(modern)))
        if args.brook_guest and number in (0, 1, 2, 9):
            cases[f'unbounded_default_{number}'] = list(modern) == [(1 << 64) - 1] * 2
        if args.brook_guest and number == 3:
            cases['stack_default_preserved'] = list(modern) == [8 * 1024 * 1024, (1 << 64) - 1]
        if args.brook_guest and number == 7:
            cases['descriptor_default_preserved'] = list(modern) == [1024, 1024]
    passed = all(cases.values())
    print(json.dumps(dict(resource_limit_queries_pass=passed, checks=len(cases),
                          cases=cases, values=values, scope='Queries only; no limit mutation or enforcement test')),
          flush=True)
    return int(not passed and not args.report_only)


if __name__ == '__main__':
    raise SystemExit(main())
