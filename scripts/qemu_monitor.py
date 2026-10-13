#!/usr/bin/env python3
"""Send HMP commands to a Brook VM (e.g. info status, screendump, quit)."""

import argparse
import socket


def read_prompt(connection):
    chunks = bytearray()
    while not chunks.endswith(b"(qemu) "):
        data = connection.recv(65536)
        if not data:
            break
        chunks.extend(data)
    return chunks.decode(errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", type=int)
    parser.add_argument("--socket", help="Override the Unix monitor socket")
    parser.add_argument("commands", nargs="+", help="One quoted HMP command per argument")
    args = parser.parse_args()
    suffix = "" if args.instance is None else f"_{args.instance}"
    path = args.socket or f"/tmp/qemu_monitor{suffix}.sock"
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(10)
            connection.connect(path)
            read_prompt(connection)
            for command in args.commands:
                if "\n" in command or "\r" in command:
                    parser.error("Pass each monitor command as a separate argument")
                connection.sendall((command + "\n").encode())
                response = read_prompt(connection)
                # HMP echoes typed characters with terminal cursor sequences.
                # The actual response starts after the echoed line's CRLF.
                output = response.partition("\r\n")[2]
                output = output.removesuffix("(qemu) ").strip()
                if output:
                    print(output)
                    if output.startswith(("Error:", "invalid parameter", "unknown command")):
                        parser.exit(1)
    except OSError as error:
        parser.exit(1, f"Monitor {path}: {error}\n")


if __name__ == "__main__":
    main()
