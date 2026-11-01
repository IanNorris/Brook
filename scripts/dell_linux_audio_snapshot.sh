#!/bin/sh
# Read-only, Python-free Dell audio inventory. Output may be redirected to a file.
# BROOK_AUDIO_SYSROOT is for fixture tests; on the Dell it defaults to /.
set -u
root=${BROOK_AUDIO_SYSROOT:-}
printf '%s\n' BROOK_AUDIO_SHELL_V1
printf '\n[system]\n'
uname -sr 2>/dev/null || :
show_file() {
    if [ -r "$1" ]; then head -c 65536 "$1" 2>/dev/null || :; printf '\n'
    else printf '%s\n' '(unavailable)'; fi
}
show_device() {
    path=$1
    [ -e "$path" ] || return 0
    printf '\nDEVICE %s\n' "${path##*/}"
    printf 'driver='; readlink "$path/driver" 2>/dev/null || printf '%s\n' '(unbound)'
    for field in status modalias hid uid resource irq; do
        printf '%s=' "$field"; show_file "$path/$field"
    done
    for node in "$path"/physical_node*; do
        [ -e "$node" ] || continue
        printf 'physical_node='; readlink "$node" 2>/dev/null || :
        printf 'physical_driver='; readlink "$node/driver" 2>/dev/null || printf '%s\n' '(unbound)'
        printf 'physical_resource='; show_file "$node/resource"
        printf 'physical_irq='; show_file "$node/irq"
    done
}
printf '\n[acpi]\n'
for p in "$root"/sys/bus/acpi/devices/INT3438:* "$root"/sys/bus/acpi/devices/INT33C8:* "$root"/sys/bus/acpi/devices/INT343A:* "$root"/sys/bus/acpi/devices/INT33CA:* "$root"/sys/bus/acpi/devices/INT3439:* "$root"/sys/bus/acpi/devices/INT33C9:*; do show_device "$p"; done
printf '\n[platform]\n'
for p in "$root"/sys/bus/platform/devices/INT3438:* "$root"/sys/bus/platform/devices/INT33C8:* "$root"/sys/bus/platform/devices/INT343A:* "$root"/sys/bus/platform/devices/INT33CA:* "$root"/sys/bus/platform/devices/INT3439:* "$root"/sys/bus/platform/devices/INT33C9:* "$root"/sys/bus/platform/devices/broadwell-audio*; do show_device "$p"; done
printf '\n[i2c]\n'
for p in "$root"/sys/bus/i2c/devices/*; do
    [ -e "$p" ] || continue
    case "${p##*/}" in *INT343A*|*INT33CA*) show_device "$p";;
    *) driver=$(readlink "$p/driver" 2>/dev/null || :); [ "${driver##*/}" != rt286 ] || show_device "$p";; esac
done
printf '\n[sound cards]\n'; show_file "$root/proc/asound/cards"
printf '\n[pcm]\n'; show_file "$root/proc/asound/pcm"
printf '\n[hda codec topology; each file limited to 65536 bytes]\n'
found_codec=0
for codec in "$root"/proc/asound/card[0-9]*/codec\#*; do
    [ -f "$codec" ] || continue
    found_codec=1; printf '\nCODEC %s/%s\n' "${codec%/*}" "${codec##*/}"
    show_file "$codec"
done
[ "$found_codec" -ne 0 ] || printf '%s\n' '(unavailable)'
if [ -z "$root" ]; then
    printf '\n[pci hda controller]\n'
    if command -v lspci >/dev/null 2>&1; then lspci -nnk -s 00:1b.0 2>/dev/null || printf '%s\n' '(lspci failed)'
    else printf '%s\n' '(lspci unavailable)'; fi
    printf '\n[card 0 mixer; first 400 lines]\n'
    if command -v amixer >/dev/null 2>&1; then amixer -c 0 contents 2>/dev/null | head -400
    else printf '%s\n' '(amixer unavailable)'; fi
fi
for card in "$root"/sys/class/sound/card[0-9]*; do
    [ -e "$card" ] || continue
    case "${card##*/}" in *[!0-9]card*|card*[!0-9]*) continue;; esac
    printf '\nCARD %s\n' "${card##*/}"; printf 'id='; show_file "$card/id"
    printf 'device='; readlink "$card/device" 2>/dev/null || printf '%s\n' '(unavailable)'
    printf 'driver='; readlink "$card/device/driver" 2>/dev/null || printf '%s\n' '(unbound)'
done
printf '\n[firmware hashes]\n'
if ! command -v sha256sum >/dev/null 2>&1; then printf '%s\n' '(sha256sum unavailable)'; fi
for f in "$root"/sys/firmware/acpi/tables/DSDT "$root"/sys/firmware/acpi/tables/SSDT* "$root"/lib/firmware/intel/IntcSST2.bin* "$root"/lib/firmware/intel/catpt/bdw/dsp_basefw.bin*; do
    [ -f "$f" ] || continue
    sha256sum "$f" 2>/dev/null || :
done
printf '\n[filtered kernel messages]\n'
if [ -z "$root" ]; then
    if messages=$(dmesg 2>/dev/null); then
        printf '%s\n' "$messages" | grep -i -E 'catpt|(^|[^a-z])sst([^a-z]|$)|rt28[68]|i2c[-_]designware|snd[-_]soc|sof[-_]audio|snd[-_]hda|hdaudio|alc3263' | head -200 || :
    else printf '%s\n' '(dmesg unavailable)'; fi
fi
printf '\nEND_BROOK_AUDIO_SHELL_V1\n'
