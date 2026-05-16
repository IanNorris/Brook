#!/usr/bin/env bash
# Archive debug symbols for the current build.
#
# Creates a compressed tarball of all ELF binaries (kernel, drivers, apps)
# in symbols/<git-hash>.tar.xz. The crash decoder can look up the right
# symbols by git hash from the panic dump.
#
# Usage:
#   archive_symbols.sh [--build-dir build/debug]
#
# The archive contains:
#   kernel/BROOK.elf
#   drivers/*.mod
#   apps/*              (only ELF binaries, not .o files)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

BUILD_DIR="${ROOT_DIR}/build/debug"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

SYMBOLS_DIR="${ROOT_DIR}/symbols"
mkdir -p "$SYMBOLS_DIR"

# Get git hash (must match what CMake embeds via BROOK_GIT_HASH)
GIT_HASH=$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_BRANCH=$(git -C "$ROOT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")

ARCHIVE="${SYMBOLS_DIR}/${GIT_HASH}.tar.xz"

# Skip if archive already exists for this hash
if [ -f "$ARCHIVE" ]; then
    echo "symbols: ${GIT_HASH} already archived ($(du -h "$ARCHIVE" | cut -f1))"
    exit 0
fi

# Build a file list of ELFs to archive
TMPLIST=$(mktemp)

# Kernel ELF
KERNEL_ELF="${BUILD_DIR}/kernel/BROOK.elf"
if [ -f "$KERNEL_ELF" ]; then
    echo "$KERNEL_ELF" >> "$TMPLIST"
fi

# Driver modules (.mod = ELF relocatable with debug info)
for mod in "${BUILD_DIR}/kernel/drivers/"*.mod; do
    [ -f "$mod" ] && echo "$mod" >> "$TMPLIST"
done

# Userspace apps (ELF binaries only, skip .o and .d files)
APP_DIR="${ROOT_DIR}/build/apps"
if [ -d "$APP_DIR" ]; then
    for app in "$APP_DIR"/*; do
        [ -f "$app" ] || continue
        # Only include ELF binaries
        case "$(file -b "$app")" in
            ELF*) echo "$app" >> "$TMPLIST" ;;
        esac
    done
fi

FILE_COUNT=$(wc -l < "$TMPLIST")
if [ "$FILE_COUNT" -eq 0 ]; then
    echo "symbols: no ELF files found in ${BUILD_DIR}" >&2
    exit 1
fi

# Create the archive using a staging directory for clean paths
STAGING=$(mktemp -d)
trap "rm -f '$TMPLIST'; rm -rf '$STAGING'" EXIT

# Copy files into staging with the desired directory structure
KERNEL_ELF="${BUILD_DIR}/kernel/BROOK.elf"
if [ -f "$KERNEL_ELF" ]; then
    mkdir -p "$STAGING/kernel"
    cp "$KERNEL_ELF" "$STAGING/kernel/"
fi

mkdir -p "$STAGING/drivers"
for mod in "${BUILD_DIR}/kernel/drivers/"*.mod; do
    [ -f "$mod" ] && cp "$mod" "$STAGING/drivers/"
done

if [ -d "$APP_DIR" ]; then
    mkdir -p "$STAGING/apps"
    for app in "$APP_DIR"/*; do
        [ -f "$app" ] || continue
        case "$(file -b "$app")" in
            ELF*) cp "$app" "$STAGING/apps/" ;;
        esac
    done
fi

tar -cJf "$ARCHIVE" -C "$STAGING" .

SIZE=$(du -h "$ARCHIVE" | cut -f1)
echo "symbols: archived ${FILE_COUNT} files → ${ARCHIVE} (${SIZE}, ${GIT_BRANCH}/${GIT_HASH})"

# Write a manifest for quick lookups without extracting
MANIFEST="${SYMBOLS_DIR}/${GIT_HASH}.manifest"
cat > "$MANIFEST" <<EOF
git_hash=${GIT_HASH}
git_branch=${GIT_BRANCH}
build_dir=${BUILD_DIR}
archive=${ARCHIVE}
created=$(date -u +%Y-%m-%dT%H:%M:%SZ)
files=$(cat "$TMPLIST" | sed "s|${BUILD_DIR}/||;s|${ROOT_DIR}/build/apps/|apps/|" | tr '\n' ' ')
EOF
