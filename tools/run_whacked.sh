#!/usr/bin/env bash
# run_whacked.sh --iso PATH [--frames N] [--width W] [--height H] [--log FILE]
# Runs WhackedRecomp headless-ish: exits after --frames frames so we can inspect
# the boot log without a human watching the window.
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" || exit 1
EXE=build/WhackedRecomp.exe
LOG=build_work/boot.log
FR=0
ISO="Whacked! (USA, Europe).xiso.iso"
while [ $# -gt 0 ]; do
  case "$1" in
    --iso)   shift; ISO="$1";;
    --frames) shift; FR="$1";;
    --width) shift; W="$1";;
    --height) shift; H="$1";;
    --log)   shift; LOG="$1";;
    *) echo "unknown arg: $1" >&2; exit 1;;
  esac
  shift
done
if [ ! -x "$EXE" ]; then echo "no exe at $EXE" >&2; exit 1; fi
if [ "$FR" -gt 0 ]; then
  timeout --signal=TERM 240 "$EXE" --iso "$ISO" --frames "$FR" --trace > "$LOG" 2>&1
else
  timeout --signal=TERM 240 "$EXE" --iso "$ISO" --trace > "$LOG" 2>&1
fi
echo "returncode=$?  log=$LOG"
