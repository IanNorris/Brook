#!/bin/sh
# Read-only PCI resource inventory from Linux sysfs; no configuration writes.
set -u
root=${BROOK_PCI_SYSROOT:-}
printf '%s\n' BROOK_PCI_RESOURCES_V1
found=0
for p in "$root"/sys/bus/pci/devices/*; do
    [ -e "$p" ] || continue
    found=1;printf '\nDEVICE %s\n' "${p##*/}"
    for field in vendor device class resource; do
        printf '%s=\n' "$field"
        if [ -r "$p/$field" ]; then head -c 65536 "$p/$field" 2>/dev/null || :
        else printf '%s\n' '(unavailable)'; fi
    done
done
[ "$found" -ne 0 ] || printf '%s\n' '(no PCI devices available)'
printf '\nEND_BROOK_PCI_RESOURCES_V1\n'
