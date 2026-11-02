#!/usr/bin/env bash
# Exercise the portable 8-bit effect mixer without requiring game data.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"${CC:-cc}" -std=gnu11 -O2 -ffunction-sections -fdata-sections \
  -I"$root/tools/quake2/source" \
  "$root/tools/quake2/source/snd_mix.c" \
  "$root/tools/quake2/source/snd_mix_8bit_test.c" \
  -Wl,--gc-sections -lm -o "$tmp/snd-mix-8bit-test"
"$tmp/snd-mix-8bit-test"
echo 'Quake 2 8-bit mixer regression passed'
