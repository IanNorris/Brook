#!/usr/bin/env bash
# BRO-179 batch hunter: run schedstress repeatedly with the armed free-site
# detector until it captures the racing free (FREE-DURING-RWCLEANUP) or the
# Process-UAF / field-poison detectors, or the kernel #GP at KRwLockCleanupOnExit.
# Writes a HIT marker + preserves the offending serial log on the first capture.
set -u
cd /workspace/brook || exit 1

ITERS="${1:-20}"
OUTDIR=/tmp/bro179_hunt
mkdir -p "$OUTDIR"
HITFILE="$OUTDIR/HIT.txt"
rm -f "$HITFILE"

SIGS='FREE-DURING-RWCLEANUP|RWLOCKFIELD-POISON|RWCLEANUP-UAF|KERNEL FAULT|POISON-ORIGIN|\[FAULT\] User fault'

kill_qemu() {
  for f in /proc/[0-9]*/cmdline; do
    tr '\0' ' ' <"$f" 2>/dev/null | grep -q qemu-system && \
      kill -9 "$(basename "$(dirname "$f")")" 2>/dev/null
  done
}

for i in $(seq 1 "$ITERS"); do
  kill_qemu
  LOG="$OUTDIR/run_${i}.log"
  rm -f "$LOG"
  echo "[hunt] iteration $i/$ITERS starting $(date '+%H:%M:%S')" >> "$OUTDIR/hunt.log"
  # run-qemu.sh never self-exits (the OS idles at the desktop after schedstress),
  # so cap each iteration with a timeout, then kill any lingering qemu.
  timeout 780 env SERIAL_OPT="-serial file:${LOG}" BROOK_SMP=8 \
    ./scripts/run-qemu.sh --release --headless --instance=3 --script schedstress \
    >/dev/null 2>&1
  kill_qemu
  if grep -qE "$SIGS" "$LOG"; then
    echo "[hunt] HIT on iteration $i $(date '+%H:%M:%S')" | tee -a "$OUTDIR/hunt.log"
    {
      echo "HIT iteration $i at $(date)"
      echo "log: $LOG"
      echo "--- matching lines ---"
      grep -nE "$SIGS" "$LOG"
    } > "$HITFILE"
    cp "$LOG" "$OUTDIR/HIT_run_${i}.log"
    break
  fi
  echo "[hunt] iteration $i clean $(date '+%H:%M:%S')" >> "$OUTDIR/hunt.log"
done
kill_qemu
echo "[hunt] done $(date '+%H:%M:%S')" >> "$OUTDIR/hunt.log"
