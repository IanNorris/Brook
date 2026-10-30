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


def exchange(host,port,commands,timeout=8):
    """Bounded framed RPCs on one connection; never retry unknown delivery."""
    validate_commands(commands)
    frames=[(str(int(uuid.uuid4().hex[:12],16)),command) for command in commands]
    deadline = time.monotonic() + timeout
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall(''.join(f'@{token} {command}\n' for token,command in frames).encode())
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
                finish=buffer.find(end,start+len(begin)) if start>=0 else -1
                if finish<0:break
                results.append(parse_guest_reply(buffer[start+len(begin):finish].decode(errors='replace')))
                del buffer[:finish+len(end)]
            if len(results)==len(frames):return results
    raise TimeoutError("No complete guest response; delivery/completion is unknown (not retried)")
