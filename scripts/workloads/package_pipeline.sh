#!/usr/bin/env bash
# Controlled package-path decomposition; run through brook-bootstrap-shell.
set -euo pipefail
work=$1
url=$2
rounds=${3:-2}
cd "$work"
export PATH=/nix/bin:$PATH
stage() {
    local label=$1 begin end
    shift
    echo "PIPELINE_BEGIN $label"
    cat /proc/blkprobe
    begin=$(/boot/PIPECLK)
    "$@"
    end=$(/boot/PIPECLK)
    printf 'PIPELINE_TIME %s %s %s\n' "$label" "$begin" "$end"
    cat /proc/blkprobe
}
read_local() { dd if=archive.nar of=/dev/null bs=64K status=none; }
pipe_local() { cat archive.nar | cat >/dev/null; }
write_local() { dd if=archive.nar of="$target-copy.nar" bs=64K conv=fsync status=none; }
decompress_local() { zstd -dq -c archive.zst >/dev/null; }
unpack_local() { nar-unpack "$target-plain" <archive.nar; }
unpack_pipe() { cat archive.nar | nar-unpack "$target-pipe"; }
unpack_zstd() { zstd -dq -c archive.zst | nar-unpack "$target-zstd"; }
network_null() { curl --fail --silent --show-error "$url" -o /dev/null -w 'PIPELINE_CURL bytes=%{size_download} seconds=%{time_total} speed=%{speed_download}\n'; }
network_file() { curl --fail --silent --show-error "$url" -o "$target-download.zst" -w 'PIPELINE_CURL bytes=%{size_download} seconds=%{time_total} speed=%{speed_download}\n'; }
network_decompress() { curl --fail --silent --show-error "$url" | zstd -dq -c >/dev/null; }
network_unpack() { curl --fail --silent --show-error "$url" | zstd -dq -c | nar-unpack "$target-network"; }
functions=(read_local pipe_local write_local decompress_local unpack_local unpack_pipe unpack_zstd network_null network_file network_decompress network_unpack)
for ((round=1; round<=rounds; round++)); do
    target="$work/round-$round"
    if ((round % 2)); then
        for fn in "${functions[@]}"; do stage "$round/$fn" "$fn"; done
    else
        for ((i=${#functions[@]}-1; i>=0; i--)); do stage "$round/${functions[i]}" "${functions[i]}"; done
    fi
    python3 /boot/PIPECHK "$work" "$round"
done
sync
echo PIPELINE_ALL_PASS
