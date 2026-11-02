#!/usr/bin/env bash
# Host-only checks; creates a sparse disposable 17 GiB ext2 fixture.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
image="$tmp/store.img"
truncate -s 17G "$image"
mke2fs -q -t ext2 -F -b 4096 -I 128 -N 8192 -m 0 \
    -O none,filetype,sparse_super,large_file "$image"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -iquote "$root/src/kernel/src" \
    "$root/src/tests/test_large_ext2_geometry/main.cpp" -o "$tmp/geometry"
"$tmp/geometry"
"$tmp/geometry" "$image"
e2fsck -fn "$image" > "$tmp/fsck.log" 2>&1
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -iquote "$root/src/kernel/src" \
    "$root/src/tests/test_ext2_extended_time/main.cpp" -o "$tmp/time"
"$tmp/time"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-misleading-indentation -iquote "$root/src/kernel/src" \
    "$root/src/tests/test_large_data_session/main.cpp" -o "$tmp/session"
"$tmp/session"
echo 'Large ext2 geometry, header, session and extended time host checks passed'
