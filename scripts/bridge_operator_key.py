#!/usr/bin/env python3
"""Generate an owner-only Ed25519 seed and print its public key as lowercase hex."""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from bridge_v3 import create_operator_key, read_operator_key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['generate', 'public'])
    parser.add_argument('seed', type=Path)
    args = parser.parse_args()
    try:
        if args.action == 'generate':
            public_key = create_operator_key(args.seed)
        else:
            public_key = bytes(read_operator_key(args.seed).verify_key)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    print(public_key.hex())


if __name__ == '__main__':
    main()
