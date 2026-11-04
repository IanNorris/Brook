#!/usr/bin/env python3
"""Check cooperative close and isolation with two disposable xeyes windows.

Start two xeyes instances on the same rootless Xwayland server before running.
Both are closed; do not use this acceptance check on a personal session.
"""
import argparse
import uuid
import server


def close(session, row, wait_for='closed'):
    return server.window_action(session, row['reference'], 'close',
                                uuid.uuid4().hex, wait_for=wait_for, wait_s=15)


def main():
    if not __debug__:
        raise RuntimeError('Acceptance checks require assertions to be enabled')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', required=True)
    args = parser.parse_args()
    session = args.session
    windows = [w for w in server.windows(session)['windows'] if w['title'] == 'xeyes']
    if len(windows) != 2:
        raise RuntimeError('Expected exactly two disposable xeyes windows')
    first, second = windows
    assert first['provider'] == second['provider']
    for window in windows:
        assert window['client_origin'] == 'unknown' and window['client'] is None
        assert window['api_window'] and 'close' in window['supports']

    result = close(session, first)
    assert result['state'] == 'completed' and result['wait_condition_met']
    assert result['guest']['close_method'] == 1
    assert not result['conditions']['client_group_exit']
    remaining = server.windows(session)['windows']
    assert any(w['reference'] == second['reference'] for w in remaining)
    assert not any(w['reference'] == first['reference'] for w in remaining)

    stale = close(session, first, 'applied')
    assert stale['state'] == 'rejected' and stale['effect'] == 'rejected'
    assert stale['guest']['error_code'] == -2
    assert any(w['reference'] == second['reference'] for w in server.windows(session)['windows'])

    result = close(session, second)
    assert result['state'] == 'completed' and result['wait_condition_met']
    assert result['guest']['close_method'] == 1
    print('PASS: cooperative close, sibling survival and stale-reference rejection')


if __name__ == '__main__':
    main()
