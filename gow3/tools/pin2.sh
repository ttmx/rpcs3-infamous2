#!/bin/bash
# pin2.sh on|off: on = the main PPU thread, the render thread and the six SPU threads each on the first hardware thread
# of a core of their own, every other thread of the emulator on the second hardware threads of the SPU cores (the
# siblings of the main and render threads' cores stay empty); off = every thread anywhere again
cd "$(dirname "$0")/.."
pid=$(python3 -c "import json;print(json.load(open('state.json'))['emu'])")
if [ "$1" = off ]; then for t in /proc/$pid/task/*; do taskset -pc 0-15 $(basename $t) >/dev/null 2>&1; done; exit; fi
i=0
for t in /proc/$pid/task/*; do
  n=$(cat $t/comm 2>/dev/null); tid=$(basename $t)
  case "$n" in
    "PPU[0x1000000]"*) taskset -pc 0 $tid >/dev/null 2>&1;;
    rsx::thread) taskset -pc 2 $tid >/dev/null 2>&1;;
    SPU\[*) taskset -pc $((4 + 2 * (i % 6))) $tid >/dev/null 2>&1; i=$((i+1));;
    *) taskset -pc 5,7,9,11,13,15 $tid >/dev/null 2>&1;;
  esac
done
