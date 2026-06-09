#!/usr/bin/env bash
# BRO-179 quarantine validation: run schedstress N times, count clean PASS vs
# any SIG1 fault signature. Does NOT stop on hit — runs the full batch so we get
# a pass/fault ratio for the stopgap.
set -u
cd /workspace/brook || exit 1

ITERS="${1:-5}"
OUTDIR=/tmp/bro179_quar
mkdir -p "$OUTDIR"

FAULT_SIGS='KERNEL FAULT|\[FAULT\] User fault|POISON-ORIGIN|RWLOCKFIELD-POISON|RSVD-BIT|BRO179-DOUBLEALLOC|BRO176-MAPLEAK|BRO176-ALLOCMAPPED'

kill_qemu() {
  for f in /proc/[0-9]*/cmdline; do
    tr '\0' ' ' <"$f" 2>/dev/null | grep -q qemu-system && \
      kill -9 "$(basename "$(dirname "$f")")" 2>/dev/null
  done
}

pass=0; fault=0; other=0
for i in $(seq 1 "$ITERS"); do
  kill_qemu
  LOG="$OUTDIR/val_${i}.log"
  rm -f "$LOG"
  echo "[val] iteration $i/$ITERS starting $(date '+%H:%M:%S')" >> "$OUTDIR/val.log"
  env SERIAL_OPT="-serial file:${LOG}" BROOK_SMP=8 \
    ./scripts/run-qemu.sh --release --headless --instance=3 --script schedstress \
    >/dev/null 2>&1 &
  RUNPID=$!
  outcome="timeout"
  for _ in $(seq 1 260); do
    sleep 3
    if [ -f "$LOG" ]; then
      if grep -qE "$FAULT_SIGS" "$LOG"; then outcome="fault"; sleep 2; break; fi
      if grep -qE "result +: +PASS" "$LOG"; then outcome="pass"; sleep 1; break; fi
    fi
    kill -0 "$RUNPID" 2>/dev/null || { outcome="exited"; break; }
  done
  kill_qemu
  wait "$RUNPID" 2>/dev/null
  case "$outcome" in
    pass)  pass=$((pass+1));  echo "[val] iter $i PASS $(date '+%H:%M:%S')" >> "$OUTDIR/val.log" ;;
    fault) fault=$((fault+1)); cp "$LOG" "$OUTDIR/FAULT_${i}.log"
           echo "[val] iter $i FAULT $(date '+%H:%M:%S')" | tee -a "$OUTDIR/val.log" ;;
    *)     other=$((other+1)); echo "[val] iter $i $outcome $(date '+%H:%M:%S')" >> "$OUTDIR/val.log" ;;
  esac
  echo "[val] running tally: pass=$pass fault=$fault other=$other" >> "$OUTDIR/val.log"
done
kill_qemu
echo "[val] DONE pass=$pass fault=$fault other=$other $(date '+%H:%M:%S')" | tee -a "$OUTDIR/val.log"
