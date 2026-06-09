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

# DETECT: any fault — used to kill a (possibly hung) VM quickly and end the
# iteration without waiting out the backstop.
DETECT_SIGS='FREE-DURING-RWCLEANUP|RWLOCKFIELD-POISON|RWCLEANUP-UAF|KERNEL FAULT|POISON-ORIGIN|\[FAULT\] User fault'
# STOP: a capture worth keeping — one where the reverse-map actually ran
# (kernel RSVD #PF path halts cleanly and names the mapper). User-poison faults
# hang BEFORE the reverse-map, so they match DETECT but not STOP: we kill them
# fast and keep hunting for a clean RSVD capture.
STOP_SIGS='end reverse-map|MAPPER pid=|no live user PTE maps this frame'

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
  # and a fault can HANG the VM (user-poison path) instead of halting cleanly.
  # So run qemu in the background and poll the serial log: the moment a fault
  # signature appears, give it ~3s to finish flushing its dump, then kill the VM
  # and move on. This makes every iteration end seconds after a fault instead of
  # waiting out the full timeout. Hard cap at 780s as a backstop.
  env SERIAL_OPT="-serial file:${LOG}" BROOK_SMP=8 \
    ./scripts/run-qemu.sh --release --headless --instance=3 --script schedstress \
    >/dev/null 2>&1 &
  RUNPID=$!
  hit=0
  for _ in $(seq 1 260); do      # 260 * 3s = 780s backstop
    sleep 3
    if [ -f "$LOG" ] && grep -qE "$DETECT_SIGS" "$LOG"; then hit=1; sleep 3; break; fi
    kill -0 "$RUNPID" 2>/dev/null || break   # run-qemu wrapper exited
  done
  kill_qemu
  wait "$RUNPID" 2>/dev/null
  # Only STOP for a capture where the reverse-map ran (RSVD path). A user-poison
  # fault matches DETECT but hangs before the reverse-map — record it and keep
  # hunting for a clean RSVD capture that names the mapper.
  if grep -qE "$STOP_SIGS" "$LOG"; then
    echo "[hunt] STOP-HIT (reverse-map) on iteration $i $(date '+%H:%M:%S')" | tee -a "$OUTDIR/hunt.log"
    {
      echo "HIT iteration $i at $(date)"
      echo "log: $LOG"
      echo "--- reverse-map + fault lines ---"
      grep -nE "$STOP_SIGS|$DETECT_SIGS" "$LOG"
    } > "$HITFILE"
    cp "$LOG" "$OUTDIR/HIT_run_${i}.log"
    break
  fi
  if [ "$hit" = 1 ]; then
    echo "[hunt] iteration $i fault-no-revmap (user-poison hang), preserved, continuing $(date '+%H:%M:%S')" >> "$OUTDIR/hunt.log"
    cp "$LOG" "$OUTDIR/fault_run_${i}.log"
  else
    echo "[hunt] iteration $i clean $(date '+%H:%M:%S')" >> "$OUTDIR/hunt.log"
  fi
done
kill_qemu
echo "[hunt] done $(date '+%H:%M:%S')" >> "$OUTDIR/hunt.log"
