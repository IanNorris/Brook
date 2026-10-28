#!/usr/bin/env python3
"""Small stdio MCP adapter for a running portable Brook instance.

Use: python3 mcp.py /absolute/path/to/session.json
Images are artifact paths, never base64 text. Full profiling/guest-job tools
remain available in the separately included Linux development bridge.
"""
import json
from pathlib import Path
import sys
import urllib.error
import urllib.request

TOOLS = {
    'vm_status': ('Inspect the VM and obtain its current generation.', 'status'),
    'vm_logs': ('Return the last 32 KiB of the guest serial log.', 'logs'),
    'vm_screenshot': ('Save a full-resolution PPM screenshot; return its absolute path.', 'screenshot'),
    'vm_input': ('Send QMP key/button/absolute/relative events into this VM.', 'input'),
    'vm_sync': ('Flush guest filesystems through the guarded COM2 protocol.', 'sync'),
    'vm_shutdown': ('Request clean guest shutdown and confirm filesystem sync and QEMU exit.', 'shutdown'),
    'vm_restart': ('Request clean guest restart and wait for a fresh boot identity.', 'restart'),
    'vm_force_stop': ('Force-stop the VM; may lose guest data. Requires explicit acknowledgement.', 'force_stop'),
}


def request(descriptor, payload):
    endpoint = json.loads(Path(descriptor).read_text())
    # Never follow redirects or use host proxy configuration for private control.
    if not endpoint['url'].startswith('http://127.0.0.1:') or not endpoint['url'].endswith('/control'):
        raise ValueError('Expected a loopback control descriptor')
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    req = urllib.request.Request(endpoint['url'], json.dumps(payload).encode(),
                                 {'Authorization': 'Bearer ' + endpoint['token'], 'Content-Type': 'application/json'})
    try:
        with opener.open(req, timeout=270) as reply:
            return json.load(reply)['result']
    except urllib.error.HTTPError as exc:
        raise RuntimeError(json.load(exc).get('error', str(exc))) from exc


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    for line in sys.stdin:
        response = None
        try:
            message = json.loads(line)
            if 'id' not in message:
                continue
            method = message['method']
            if method == 'initialize':
                result = dict(protocolVersion=message.get('params', {}).get('protocolVersion', '2024-11-05'),
                              capabilities={'tools': {}}, serverInfo={'name': 'brook-portable', 'version': '0.1'})
            elif method == 'ping':
                result = {}
            elif method == 'tools/list':
                result = {'tools': []}
                for name, (description, action) in TOOLS.items():
                    properties = {} if action == 'status' else {'generation': {'type': 'integer', 'minimum': 1}}
                    required = [] if action == 'status' else ['generation']
                    if action == 'input':
                        properties['events'] = {'type': 'array', 'minItems': 1, 'maxItems': 64, 'items': {'type': 'object'}}
                        required.append('events')
                    if action == 'force_stop':
                        properties['acknowledge_data_loss'] = {'type': 'boolean', 'const': True}
                        required.append('acknowledge_data_loss')
                    result['tools'].append(dict(name=name, description=description,
                                               inputSchema=dict(type='object', properties=properties, required=required, additionalProperties=False)))
            elif method == 'tools/call':
                params = message['params']
                try:
                    action = TOOLS[params['name']][1]
                    arguments = dict(params.get('arguments', {}), action=action)
                    value = request(sys.argv[1], arguments)
                    result = {'content': [{'type': 'text', 'text': json.dumps(value)}]}
                except Exception as exc:
                    result = {'isError': True, 'content': [{'type': 'text', 'text': str(exc)}]}
            else:
                response = dict(jsonrpc='2.0', id=message['id'], error=dict(code=-32601, message='Unknown method'))
            if response is None:
                response = dict(jsonrpc='2.0', id=message['id'], result=result)
        except Exception as exc:
            response = dict(jsonrpc='2.0', id=None, error=dict(code=-32600, message=str(exc)))
        print(json.dumps(response), flush=True)


if __name__ == '__main__':
    main()
