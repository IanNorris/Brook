"""Bounded framed Brook TCP exchanges, independent of hypervisor/process state."""
import json
import socket
import time
import uuid

def parse_guest_reply(reply):
    # A census is a framed JSON-lines stream, not just its first header.
    # Keep single-object responses and surrounding human logs compatible.
    lines = [line for line in reply.splitlines() if line.startswith("{")]
    if not lines:
        return {"reply": reply.strip()}
    first = json.loads(lines[0])
    if first.get("kind") != "memory_census":
        return first
    records = [json.loads(line) for line in lines[1:]]
    if not records or records[-1].get("snapshot") != "rolling" or "spaces" not in records[-1]:
        raise ValueError("Incomplete memory census response")
    return {**first, "records": records[:-1], "summary": records[-1]}



def validate_commands(commands):
    if not isinstance(commands,list) or not 1<=len(commands)<=8:
        raise ValueError("Guest batch must contain 1–8 commands")
    for command in commands:
        if not isinstance(command,str) or any(c in command for c in "\n\r\0") or len(command.encode())>220:
            raise ValueError("Guest command must fit one 220-byte protocol line")


def exchange(host,port,commands,timeout=8,progress=None,on_reply=None):
    """Bounded framed RPCs on one connection; never retry unknown delivery."""
    validate_commands(commands)
    frames=[(str(int(uuid.uuid4().hex[:12],16)),command) for command in commands]
    deadline = time.monotonic() + timeout
    if progress:progress('connecting',0)
    with socket.create_connection((host, port), timeout=timeout) as s:
        if progress:progress('connected',0)
        s.sendall(''.join(f'@{token} {command}\n' for token,command in frames).encode())
        if progress:progress('request_sent',0)
        buffer = bytearray()
        results=[]
        received=0
        while time.monotonic() < deadline:
            s.settimeout(max(.1, deadline - time.monotonic()))
            chunk = s.recv(65536)
            if not chunk:
                break
            buffer.extend(chunk)
            received+=len(chunk)
            if received > 2 * 1024 * 1024:
                raise ValueError("Guest reply exceeded bounded receive buffer")
            # Replies use LF; the log stream may use CRLF. Consume only the
            # next token's complete frame, including when recv coalesces them.
            while len(results)<len(frames):
                token=frames[len(results)][0]
                begin,end=f'BRIDGE_BEGIN {token}\n'.encode(),f'BRIDGE_END {token}\n'.encode()
                start=buffer.find(begin)
                if start>=0 and progress:progress('frame_started',len(results))
                finish=buffer.find(end,start+len(begin)) if start>=0 else -1
                if finish<0:break
                if progress:progress('frame_complete',len(results)+1)
                results.append(parse_guest_reply(buffer[start+len(begin):finish].decode(errors='replace')))
                if on_reply:on_reply(len(results),results[-1])
                del buffer[:finish+len(end)]
            if len(results)==len(frames):return results
    raise TimeoutError("No complete guest response; delivery/completion is unknown (not retried)")


def exchange_v3(host, port, commands, timeout, key_file, boot_id=None, progress=None, on_reply=None):
    """One authenticated command and terminal response per socket."""
    from pathlib import Path
    from bridge_v3 import COMMAND, DATA, END, ZERO_ID, BridgeV3Error, open_bridge_v3, read_operator_key

    validate_commands(commands)
    if boot_id is None and commands != ["bridge hardware caps"]:
        raise ValueError("An unbound bridge boot supports only status discovery")
    key = read_operator_key(Path(key_file))
    expected = ZERO_ID if boot_id is None else bytes.fromhex(boot_id)
    results = []
    deadline = time.monotonic() + timeout
    for command in commands:
        token = str(int(uuid.uuid4().hex[:12], 16))
        frame = f"@{token} {command}\n".encode()
        if progress:
            progress("connecting", len(results))
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Bridge command batch deadline")
        with socket.create_connection((host, port), timeout=min(remaining, 4)) as sock:
            if progress:
                progress("connected", len(results))
            connection = open_bridge_v3(
                sock, key, COMMAND, 0 if boot_id is None else 1, expected, uuid.uuid4().bytes,
                deadline=min(deadline, time.monotonic() + 4),
            )
            connection.send(DATA, frame, deadline)
            connection.send(END, b"", deadline)
            if progress:
                progress("request_sent", len(results))
            reply = bytearray()
            while True:
                kind, payload = connection.receive(deadline)
                if kind == END:
                    break
                reply.extend(payload)
                if len(reply) > 2 * 1024 * 1024:
                    raise BridgeV3Error("Guest reply exceeded bounded receive buffer")
        begin = f"BRIDGE_BEGIN {token}\n".encode()
        end = f"BRIDGE_END {token}\n".encode()
        if not reply.startswith(begin) or not reply.endswith(end):
            raise BridgeV3Error("Guest command reply lacks its complete framed response")
        response = parse_guest_reply(reply[len(begin):-len(end)].decode(errors="replace"))
        if boot_id is None and response.get("boot_id") != connection.boot_id.hex():
            raise BridgeV3Error("Guest status boot differs from its handshake")
        results.append(response)
        if progress:
            progress("frame_complete", len(results))
        if on_reply:
            on_reply(len(results), response)
    return results
