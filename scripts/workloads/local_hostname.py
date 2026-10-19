#!/usr/bin/env python3
"""Verify localhost and the running machine's hostname resolve through libc."""
import ipaddress
import json
import socket
import time


def main():
    name = socket.gethostname()
    start = time.monotonic()
    try:
        def addresses(host):
            return sorted({item[4][0] for item in socket.getaddrinfo(
                host, None, socket.AF_INET, socket.SOCK_STREAM)})
        local = addresses('localhost')
        own = addresses(name)
        assert local and all(ipaddress.ip_address(a).is_loopback for a in local)
        assert own
        print(json.dumps(dict(passed=True, hostname=name, localhost=local,
                              addresses=own, seconds=time.monotonic() - start)))
        return 0
    except Exception as error:
        print(json.dumps(dict(passed=False, hostname=name, error=str(error),
                              seconds=time.monotonic() - start)))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
