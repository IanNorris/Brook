#!/usr/bin/env python3
"""Query or set one Tasmota relay using its web API.

The password file contains only the web password, optionally followed by a
newline. It is read locally and is never included in command-line arguments.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import sys
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def main():
    config_parser = argparse.ArgumentParser(add_help=False)
    config_parser.add_argument('--config', type=Path, help='file containing DELLPOWER_* assignments')
    preliminary, _ = config_parser.parse_known_args()
    settings = dict(os.environ)
    if preliminary.config:
        try:
            for line in preliminary.config.read_text(encoding='utf-8').splitlines():
                fields = shlex.split(line, comments=True)
                if not fields:
                    continue
                if fields[0] == 'export':
                    fields.pop(0)
                if len(fields) != 1 or '=' not in fields[0]:
                    raise ValueError('expected one KEY=VALUE assignment per line')
                key, value = fields[0].split('=', 1)
                if key not in ('DELLPOWER_HOSTNAME', 'DELLPOWER_USERNAME',
                               'DELLPOWER_PASSWORD', 'DELLPOWER_PASSWORD_FILE'):
                    raise ValueError('unknown setting')
                if key == 'DELLPOWER_PASSWORD':
                    settings.pop('DELLPOWER_PASSWORD_FILE', None)
                elif key == 'DELLPOWER_PASSWORD_FILE':
                    settings.pop('DELLPOWER_PASSWORD', None)
                settings[key] = value
        except (OSError, UnicodeError, ValueError) as exc:
            print(f'Cannot read config: {exc}', file=sys.stderr)
            return 1
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, help='file containing DELLPOWER_* assignments')
    parser.add_argument('action', choices=('status', 'on', 'off'))
    parser.add_argument('--host', default=settings.get('DELLPOWER_HOSTNAME'),
                        help='hostname or IP with optional :port (or DELLPOWER_HOSTNAME)')
    parser.add_argument('--password-file', type=Path,
                        default=settings.get('DELLPOWER_PASSWORD_FILE'),
                        help='password file (or DELLPOWER_PASSWORD_FILE)')
    parser.add_argument('--user', default=settings.get('DELLPOWER_USERNAME', 'admin'),
                        help='web user (or DELLPOWER_USERNAME; default: admin)')
    parser.add_argument('--relay', type=int, default=1, help='Relay number (default: 1)')
    parser.add_argument('--timeout', type=float, default=5, help='HTTP timeout in seconds')
    args = parser.parse_args()
    if not args.host:
        parser.error('supply --host or DELLPOWER_HOSTNAME')
    if not args.password_file and 'DELLPOWER_PASSWORD' not in settings:
        parser.error('supply --password-file, DELLPOWER_PASSWORD_FILE, or DELLPOWER_PASSWORD')
    if not 1 <= args.relay <= 32 or not 0 < args.timeout <= 60:
        parser.error('relay must be 1..32 and timeout must be >0 and <=60 seconds')
    endpoint = urllib.parse.urlsplit('http://' + args.host)
    try:
        if (not endpoint.hostname or endpoint.username is not None or endpoint.password is not None
                or endpoint.path or endpoint.query or endpoint.fragment or endpoint.port == 0):
            raise ValueError()
    except ValueError:
        parser.error('host must be a hostname or IP with an optional port, without a URL path')
    try:
        password = (Path(args.password_file).read_text(encoding='utf-8').rstrip('\r\n')
                    if args.password_file else settings['DELLPOWER_PASSWORD'])
        if not password or '\n' in password or '\r' in password:
            raise ValueError('password file must contain one nonempty line')
    except (OSError, UnicodeError, ValueError) as exc:
        print(f'Cannot read password file: {exc}', file=sys.stderr)
        return 1
    command = f'Power{args.relay}'
    if args.action != 'status':
        command += ' ' + args.action.upper()
    query = urllib.parse.urlencode({'user': args.user, 'password': password, 'cmnd': command})
    url = f'http://{endpoint.netloc}/cm?{query}'
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    try:
        with opener.open(url, timeout=args.timeout) as response:
            data = json.load(response)
        if not isinstance(data, dict):
            raise ValueError('unexpected response')
        key = f'POWER{args.relay}'
        state = data.get(key, data.get('POWER') if args.relay == 1 else None)
        if state not in ('ON', 'OFF'):
            raise ValueError('response did not contain a relay state')
        if args.action != 'status' and state != args.action.upper():
            raise ValueError('relay reported a different state')
    except (OSError, ValueError, json.JSONDecodeError):
        # urllib exceptions can contain the URL and therefore the password.
        print('Tasmota request failed or returned an unexpected response; relay state is unknown.', file=sys.stderr)
        return 1
    print(f'Power{args.relay}: {state}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
