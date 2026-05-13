#!/usr/bin/env bash
# Package Brook OS disk images + OVMF for running on Windows with QEMU.
#
# Creates a self-contained folder with:
#   - All disk images (ESP, ext2 root, nix, home)
#   - OVMF firmware files
#   - run_brook.bat (Windows batch script to launch QEMU)
#   - run_brook.sh (Linux/Mac fallback)
#
# Usage:
#   scripts/package_qemu_folder.sh [--output ./brook-qemu-win]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${ROOT_DIR}/build/release"

OUTPUT="${1:-${ROOT_DIR}/brook-qemu-win}"

echo "=== Packaging Brook OS QEMU folder ==="
echo "Output: ${OUTPUT}"
echo ""

mkdir -p "${OUTPUT}"

# --- Copy firmware ---
echo "Copying OVMF firmware..."
OVMF_CODE="${OVMF_CODE:-}"
OVMF_VARS="${OVMF_VARS:-}"

# Find OVMF (same logic as run-qemu.sh)
if [ -z "${OVMF_CODE}" ]; then
    SEARCH_PATHS=(
        "/run/current-system/sw/share/OVMF"
        "/nix/var/nix/profiles/default/share/OVMF"
        "/usr/share/OVMF"
        "/usr/share/ovmf"
        "/usr/share/edk2-ovmf/x64"
    )
    for p in "${SEARCH_PATHS[@]}"; do
        if [ -f "${p}/OVMF_CODE.fd" ]; then
            OVMF_CODE="${p}/OVMF_CODE.fd"
            OVMF_VARS="${p}/OVMF_VARS.fd"
            break
        fi
    done
fi
# Also check nix store (for nix-shell environments)
if [ -z "${OVMF_CODE}" ]; then
    OVMF_CODE=$(find /nix/store -name "OVMF_CODE.fd" -path "*OVMF*" 2>/dev/null | head -1 || true)
    if [ -n "${OVMF_CODE}" ]; then
        OVMF_VARS="$(dirname "${OVMF_CODE}")/OVMF_VARS.fd"
    fi
fi

if [ -z "${OVMF_CODE}" ] || [ ! -f "${OVMF_CODE}" ]; then
    echo "ERROR: Cannot find OVMF firmware. Set OVMF_CODE env var."
    exit 1
fi

cp "${OVMF_CODE}" "${OUTPUT}/OVMF_CODE.fd"
cp "${OVMF_VARS}" "${OUTPUT}/OVMF_VARS.fd"
echo "  ✓ OVMF_CODE.fd + OVMF_VARS.fd"

# --- Copy ESP ---
echo ""
echo "Creating ESP image..."
ESP_DIR="${BUILD_DIR}/esp"
ESP_IMG="${OUTPUT}/brook_esp.img"
# Create a 512MB FAT32 image from the ESP directory
dd if=/dev/zero of="${ESP_IMG}" bs=1M count=512 status=none
mkfs.fat -F 32 -n "BROOK_ESP" "${ESP_IMG}" >/dev/null

# Recursively copy ESP contents
copy_to_fat() {
    local src_dir=$1
    local fat_prefix=$2
    local img=$3
    for item in "${src_dir}"/*; do
        [ -e "${item}" ] || continue
        local name=$(basename "${item}")
        local fat_path="${fat_prefix}${name}"
        if [ -d "${item}" ]; then
            mmd -i "${img}" "::${fat_path}" 2>/dev/null || true
            copy_to_fat "${item}" "${fat_path}/" "${img}"
        else
            mcopy -o -i "${img}" "${item}" "::${fat_path}"
        fi
    done
}

if [ -d "${ESP_DIR}" ]; then
    copy_to_fat "${ESP_DIR}" "" "${ESP_IMG}"
    echo "  ✓ brook_esp.img (from build/release/esp/)"
else
    echo "  WARNING: ESP directory not found at ${ESP_DIR}"
fi

# --- Copy disk images ---
echo ""
echo "Copying disk images..."
DISKS=(
    "brook_ext2_disk.img"
    "brook_nix_disk.img"
    "brook_home_disk.img"
    "brook_disk.img"
)
for disk in "${DISKS[@]}"; do
    SRC="${ROOT_DIR}/${disk}"
    if [ -f "${SRC}" ]; then
        cp "${SRC}" "${OUTPUT}/${disk}"
        echo "  ✓ ${disk} ($(du -h "${SRC}" | cut -f1))"
    else
        echo "  - ${disk} (not found, skipping)"
    fi
done

# --- Create Windows batch script ---
echo ""
echo "Creating run scripts..."
cat > "${OUTPUT}/run_brook.bat" << 'BATCH'
@echo off
REM Brook OS QEMU launcher for Windows
REM Requires: QEMU installed and qemu-system-x86_64.exe in PATH
REM   or set QEMU_PATH below.
REM
REM GPU passthrough: set BROOK_GPU=1 for virtio-gpu (software)
REM   For actual GPU passthrough you need vfio-pci on Linux.

set QEMU_PATH=qemu-system-x86_64.exe
set SMP=4
set RAM=4G

REM Copy OVMF_VARS so QEMU can write to it
copy /Y OVMF_VARS.fd OVMF_VARS_RW.fd >nul

set DRIVES=-drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd
set DRIVES=%DRIVES% -drive if=pflash,format=raw,file=OVMF_VARS_RW.fd
set DRIVES=%DRIVES% -drive format=raw,file=brook_esp.img
set DRIVES=%DRIVES% -drive if=virtio,format=raw,file=brook_ext2_disk.img
set DRIVES=%DRIVES% -drive if=virtio,format=raw,file=brook_nix_disk.img
set DRIVES=%DRIVES% -drive if=virtio,format=raw,file=brook_home_disk.img

REM Display: SDL window with VGA or virtio-gpu
set DISPLAY_OPTS=-display sdl -device virtio-vga-gl,xres=1920,yres=1200

REM Audio (optional)
set AUDIO_OPTS=-audiodev dsound,id=audio0 -device intel-hda -device hda-output,audiodev=audio0

REM Network (user-mode NAT)
set NET_OPTS=-netdev user,id=net0 -device virtio-net-pci,netdev=net0

REM USB (keyboard + mouse)
set USB_OPTS=-device qemu-xhci,id=xhci -device usb-kbd -device usb-mouse

%QEMU_PATH% ^
    -machine q35,accel=whpx:tcg ^
    -cpu max ^
    -smp %SMP% ^
    -m %RAM% ^
    %DRIVES% ^
    %DISPLAY_OPTS% ^
    %AUDIO_OPTS% ^
    %NET_OPTS% ^
    %USB_OPTS% ^
    -serial stdio

pause
BATCH

cat > "${OUTPUT}/run_brook.sh" << 'SHELL'
#!/usr/bin/env bash
# Brook OS QEMU launcher (Linux/Mac)
set -euo pipefail
cd "$(dirname "$0")"

SMP=${BROOK_SMP:-4}
RAM=${BROOK_RAM:-4G}

# Writable OVMF vars
cp -f OVMF_VARS.fd OVMF_VARS_RW.fd

DRIVES=(
    -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd
    -drive if=pflash,format=raw,file=OVMF_VARS_RW.fd
    -drive format=raw,file=brook_esp.img
)

# Add data drives if they exist
[ -f brook_ext2_disk.img ] && DRIVES+=(-drive if=virtio,format=raw,file=brook_ext2_disk.img)
[ -f brook_nix_disk.img ]  && DRIVES+=(-drive if=virtio,format=raw,file=brook_nix_disk.img)
[ -f brook_home_disk.img ] && DRIVES+=(-drive if=virtio,format=raw,file=brook_home_disk.img)
[ -f brook_disk.img ]      && DRIVES+=(-drive if=virtio,format=raw,file=brook_disk.img)

qemu-system-x86_64 \
    -machine q35,accel=kvm:hvf:whpx:tcg \
    -cpu max \
    -smp "${SMP}" \
    -m "${RAM}" \
    "${DRIVES[@]}" \
    -display sdl \
    -device virtio-vga-gl,xres=1920,yres=1200 \
    -audiodev sdl,id=audio0 -device intel-hda -device hda-output,audiodev=audio0 \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -device qemu-xhci,id=xhci -device usb-kbd -device usb-mouse \
    -serial stdio
SHELL
chmod +x "${OUTPUT}/run_brook.sh"

# --- Create README ---
cat > "${OUTPUT}/README.md" << 'README'
# Brook OS — QEMU Package

## Quick Start (Windows)
1. Install QEMU: https://www.qemu.org/download/#windows
2. Add QEMU to your PATH (or edit `run_brook.bat` to set `QEMU_PATH`)
3. Double-click `run_brook.bat`

## Quick Start (Linux/Mac)
```bash
./run_brook.sh
```

## Requirements
- QEMU 8.0+ (qemu-system-x86_64)
- 4GB+ free RAM
- Windows: WHPX acceleration recommended (enable Hyper-V)
- Linux: KVM acceleration recommended

## Files
- `OVMF_CODE.fd` / `OVMF_VARS.fd` — UEFI firmware
- `brook_esp.img` — EFI System Partition (bootloader + kernel)
- `brook_ext2_disk.img` — Root filesystem
- `brook_nix_disk.img` — Nix store (applications)
- `brook_home_disk.img` — Home directory
- `brook_disk.img` — Boot/FAT32 data disk

## Updating
To update just the kernel after rebuilding:
1. Replace `brook_esp.img` with the new one from your build
2. Or mount the ESP image and replace `KERNEL/BROOK.ELF`

## Display
Default resolution: 1920×1200 (matching Dell XPS 15 panel).
Edit the batch/shell script to change `xres`/`yres`.
README

echo ""
echo "=== Package complete: ${OUTPUT}/ ==="
echo "Contents:"
ls -lh "${OUTPUT}/" | tail -n +2
echo ""
echo "Total size: $(du -sh "${OUTPUT}" | cut -f1)"
