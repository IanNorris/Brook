#!/usr/bin/env bash
# Exercise synthetic and independently generated embedded GPT/ext2 fixtures.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
disk="$tmp/disk.img"
truncate -s 64M "$disk"
printf 'label: gpt\nunit: sectors\n\nstart=2048,size=8192,type=uefi\nstart=16384,size=32768,type=linux\n' |
    sfdisk --no-reread "$disk" > "$tmp/sfdisk.log"
# Partition starts at 8 MiB and contains 16 MiB / 4096 = 4096 blocks.
mke2fs -q -t ext2 -F -b 4096 -I 128 -N 1024 -m 0 \
    -O none,filetype,sparse_super,large_file -E offset=8388608 "$disk" 4096
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -iquote "$root/src/kernel/src" \
    "$root/src/tests/test_ahci_data_selector/main.cpp" -o "$tmp/test"
"$tmp/test"
"$tmp/test" "$disk" "$disk"
sfdisk --verify "$disk" > "$tmp/verify.log"
echo 'AHCI data selector synthetic and embedded GPT/ext2 fixtures passed'
