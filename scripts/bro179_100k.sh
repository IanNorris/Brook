#!/usr/bin/env bash
# BRO-179 quarantine 100k-rep stress: N independent 64-CPU schedstress runs at
# ROUNDS each (default 5 x 20000 = 100k total). Faithful SIG1 repro params
# (concurrent/threads) passed via the rc. Watches for the SIG1 poison signature
# (0xDFDF / RSVD #PF) AND any other fatal fault. Each run is independent (fresh
# boot), maximizing fork/exit/exit_group churn across 64 CPUs.
set -u
cd /workspace/brook || exit 1

RUNS="${1:-5}"
OUTDIR=/tmp/bro179_100k
mkdir -p "$OUTDIR"

# SIG1-specific (what the quarantine targets) vs any fatal fault.
SIG1='0xdfdf|RSVD-BIT|BRO179-DOUBLEALLOC|BRO176-MAPLEAK|BRO176-ALLOCMAPPED|RWLOCKFIELD-POISON'
FATAL='KERNEL FAULT|\[FAULT\] User fault|KILLING PROCESS|panic'
DONE='result +: +(PASS|FAIL)'

kill_qemu() {
  for f in /proc/[0-9]*/cmdline; do
    tr '\0' ' ' <"$f" 2>/dev/null | grep -q qemu-system && \
      kill -9 "$(basename "$(dirname "$f")")" 2>/dev/null
  done
}

pass=0; sig1=0; otherfault=0; inconclusive=0
for i in $(seq 1 "$RUNS"); do
  kill_qemu
  LOG="$OUTDIR/run_${i}.log"
  rm -f "$LOG"
  echo "[100k] run $i/$RUNS starting $(date '+%H:%M:%S')" >> "$OUTDIR/100k.log"
  env SERIAL_OPT="-serial file:${LOG}" BROOK_SMP=64 \
    ./scripts/run-qemu.sh --release --headless --instance=3 --script schedstress_max \
    >/dev/null 2>&1 &
  RUNPID=$!
  outcome="timeout"
  # backstop: 64-CPU runs are slow; allow up to ~50 min/run (1000*3s).
  for _ in $(seq 1 1000); do
    sleep 3
    if [ -f "$LOG" ]; then
      if grep -qiE "$SIG1" "$LOG"; then outcome="SIG1"; sleep 2; break; fi
      if grep -qE "$DONE" "$LOG"; then
        if grep -qE "result +: +PASS" "$LOG"; then outcome="pass"; else outcome="fail"; fi
        sleep 1; break
      fi
      if grep -qE "$FATAL" "$LOG"; then outcome="otherfault"; sleep 2; break; fi
    fi
    kill -0 "$RUNPID" 2>/dev/null || { outcome="exited"; break; }
  done
  kill_qemu
  wait "$RUNPID" 2>/dev/null
  last=$(grep "spawned=" "$LOG" 2>/dev/null | tail -1 | tr -s ' ')
  case "$outcome" in
    pass) pass=$((pass+1)); echo "[100k] run $i PASS ($last) $(date '+%H:%M:%S')" | tee -a "$OUTDIR/100k.log" ;;
    SIG1) sig1=$((sig1+1)); cp "$LOG" "$OUTDIR/SIG1_${i}.log"
          echo "[100k] run $i *** SIG1 SIGNATURE *** ($last)" | tee -a "$OUTDIR/100k.log" ;;
    otherfault) otherfault=$((otherfault+1)); cp "$LOG" "$OUTDIR/FAULT_${i}.log"
          echo "[100k] run $i other-fault ($last) $(date '+%H:%M:%S')" | tee -a "$OUTDIR/100k.log" ;;
    fail) otherfault=$((otherfault+1)); echo "[100k] run $i schedstress FAIL ($last)" | tee -a "$OUTDIR/100k.log" ;;
    *) inconclusive=$((inconclusive+1)); echo "[100k] run $i $outcome ($last)" | tee -a "$OUTDIR/100k.log" ;;
  esac
  echo "[100k] tally: pass=$pass sig1=$sig1 otherfault=$otherfault inconclusive=$inconclusive" >> "$OUTDIR/100k.log"
done
kill_qemu
echo "[100k] DONE pass=$pass sig1=$sig1 otherfault=$otherfault inconclusive=$inconclusive $(date '+%H:%M:%S')" | tee -a "$OUTDIR/100k.log"
