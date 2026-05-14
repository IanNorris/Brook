#!/usr/bin/env bash
# Create the Brook OS home disk image (ext2, mounted at /home).
# Pre-creates directory structure needed by GTK/XFCE apps.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_HOME_DISK:-${ROOT_DIR}/brook_home_disk.img}"
SIZE_MB="${1:-32}"

if [ -f "${DISK_IMG}" ]; then
    echo "Disk image already exists at ${DISK_IMG}"
    echo "Delete it first if you want to recreate: rm ${DISK_IMG}"
    exit 1
fi

echo "Creating ${SIZE_MB}MB ext2 home disk at ${DISK_IMG}..."

dd if=/dev/zero of="${DISK_IMG}" bs=1M count="${SIZE_MB}" status=none
mkfs.ext2 -q -b 1024 -L "BROOKHOME" "${DISK_IMG}"

# Create directory structure for GTK/XFCE apps
debugfs -w "${DISK_IMG}" <<'EOF'
mkdir .config
mkdir .config/Mousepad
mkdir .local
mkdir .local/share
mkdir .local/share/Mousepad
EOF

# Write BROOK.MNT marker (tells kernel to mount at /home)
TMPF=$(mktemp)
echo -n "/home" > "$TMPF"
debugfs -w "${DISK_IMG}" -R "write ${TMPF} BROOK.MNT" 2>/dev/null
rm -f "$TMPF"

echo "Home disk created: ${DISK_IMG}"
echo "  Mount point: /home"
echo "  Directories: .config/Mousepad, .local/share/Mousepad"
