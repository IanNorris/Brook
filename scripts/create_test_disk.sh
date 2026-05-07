#!/usr/bin/env bash
# Create an ext2 disk image containing the Ladybird test suite for Brook OS.
#
# The disk is mounted at /tests in the guest and contains:
#   /tests/bin/        - test binaries
#   /tests/lib/        - shared libraries (liblagom-*.so)
#   /tests/data/       - test data files (long_lines.txt etc.)
#   /tests/nix/store/  - full Nix closure for the test binaries
#   /tests/run_all.sh  - runner script
#
# Usage:
#   scripts/create_test_disk.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_TEST_DISK:-${ROOT_DIR}/brook_test_disk.img}"
SIZE_MB=256

# Build the test package
echo "Building Ladybird tests..."
TESTS_PKG=$(nix-build "${ROOT_DIR}/tools/ladybird-tests-pkg/default.nix" --no-out-link)
echo "  Built: ${TESTS_PKG}"

# Get full closure
echo "Computing closure..."
CLOSURE=$(nix-store -qR "$TESTS_PKG")
CLOSURE_SIZE=$(echo "$CLOSURE" | xargs du -csh 2>/dev/null | tail -1 | awk '{print $1}')
echo "  Closure: ${CLOSURE_SIZE}"

# Determine disk size needed (add 20% overhead for filesystem)
CLOSURE_BYTES=$(echo "$CLOSURE" | xargs du -csb 2>/dev/null | tail -1 | awk '{print $1}')
SIZE_MB=$(( (CLOSURE_BYTES / 1048576) * 120 / 100 + 10 ))
echo "  Disk size: ${SIZE_MB}MB"

# Remove old disk
rm -f "${DISK_IMG}"

# Create ext2 image
echo "Creating ${SIZE_MB}MB ext2 disk..."
dd if=/dev/zero of="${DISK_IMG}" bs=1M count="${SIZE_MB}" status=none
mkfs.ext2 -q -b 4096 -L "BROOKTEST" "${DISK_IMG}"

# Mount and populate
MOUNT_DIR=$(mktemp -d)
mount -o loop "${DISK_IMG}" "${MOUNT_DIR}" 2>/dev/null || {
    # No loop mount available (containers) — use debugfs
    echo "No loop mount, using debugfs to populate..."

    # Create directories
    debugfs -w "${DISK_IMG}" -R "mkdir nix" 2>/dev/null
    debugfs -w "${DISK_IMG}" -R "mkdir nix/store" 2>/dev/null
    debugfs -w "${DISK_IMG}" -R "mkdir bin" 2>/dev/null
    debugfs -w "${DISK_IMG}" -R "mkdir lib" 2>/dev/null
    debugfs -w "${DISK_IMG}" -R "mkdir data" 2>/dev/null

    # Write mount point marker
    TMPDIR2=$(mktemp -d)
    echo -n "/tests" > "${TMPDIR2}/BROOK.MNT"
    debugfs -w "${DISK_IMG}" -R "write ${TMPDIR2}/BROOK.MNT BROOK.MNT" 2>/dev/null

    # Copy Nix store closure using e2cp
    echo "Copying Nix store closure..."
    for path in $CLOSURE; do
        STORE_NAME=$(basename "$path")
        # Create store path directory in image
        e2mkdir "${DISK_IMG}:nix/store/${STORE_NAME}" 2>/dev/null || true
        # Recursively copy
        find "$path" -type f | while read -r f; do
            REL="${f#$path}"
            DIR="nix/store/${STORE_NAME}$(dirname "$REL")"
            e2mkdir "${DISK_IMG}:${DIR}" 2>/dev/null || true
            e2cp "$f" "${DISK_IMG}:${DIR}/$(basename "$f")" 2>/dev/null || true
        done
    done

    # Copy test binaries directly to /bin for easy access
    for f in "${TESTS_PKG}/bin/"*; do
        e2cp "$f" "${DISK_IMG}:bin/$(basename "$f")" 2>/dev/null || true
    done

    # Copy shared libs
    for f in "${TESTS_PKG}/lib/"*.so*; do
        [ -L "$f" ] && continue  # skip symlinks for e2cp
        e2cp "$f" "${DISK_IMG}:lib/$(basename "$f")" 2>/dev/null || true
    done

    # Copy data files
    for f in "${TESTS_PKG}/data/"*; do
        e2cp "$f" "${DISK_IMG}:data/$(basename "$f")" 2>/dev/null || true
    done

    rm -rf "${TMPDIR2}"
    echo "Done (debugfs method). Disk at: ${DISK_IMG}"
    exit 0
}

# If mount succeeded:
echo "Populating disk..."

# Mount marker
echo -n "/tests" > "${MOUNT_DIR}/BROOK.MNT"

# Copy full Nix store closure
mkdir -p "${MOUNT_DIR}/nix/store"
for path in $CLOSURE; do
    cp -a "$path" "${MOUNT_DIR}/nix/store/"
done

# Convenience symlinks/copies
mkdir -p "${MOUNT_DIR}/bin" "${MOUNT_DIR}/lib" "${MOUNT_DIR}/data"
cp -a "${TESTS_PKG}/bin/"* "${MOUNT_DIR}/bin/"
cp -a "${TESTS_PKG}/lib/"* "${MOUNT_DIR}/lib/"
cp -a "${TESTS_PKG}/data/"* "${MOUNT_DIR}/data/"

# Runner script
cat > "${MOUNT_DIR}/run_all.sh" << 'RUNNER'
#!/bin/sh
# Run all Ladybird OS-relevant tests
cd /tests/data
export LD_LIBRARY_PATH=/tests/lib
PASS=0
FAIL=0
for test in /tests/bin/Test*; do
    name=$(basename "$test")
    echo "=== $name ==="
    if "$test" 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
    echo ""
done
echo "=== RESULTS: $PASS passed, $FAIL failed ==="
RUNNER
chmod +x "${MOUNT_DIR}/run_all.sh"

umount "${MOUNT_DIR}"
rmdir "${MOUNT_DIR}"

echo "Test disk created: ${DISK_IMG} (${SIZE_MB}MB)"
echo "Mount point in guest: /tests"
echo "Run all tests: /tests/run_all.sh"
