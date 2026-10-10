#!/bin/bash
# pin6.sh on|off, for the six-SPU boot (nine busy threads on eight cores): on = the main PPU thread, the render
# thread, the second instance's SPU thread and SPU 0, 1, 3 and 4 each on a core of their own; SPU 2 and SPU 5 (two of
# the four geometry workers, which wait for the render thread a part of the time) on the two hardware threads of
# the last core; every other thread on the second hardware threads of the SPU cores. off = anywhere again.
cd "$(dirname "$0")/.."
pid=$(python3 -c "import json;print(json.load(open('state.json'))['emu'])")
if [ "$1" = off ]; then for t in /proc/$pid/task/*; do taskset -pc 0-15 $(basename $t) >/dev/null 2>&1; done; exit; fi
for t in /proc/$pid/task/*; do
  n=$(cat $t/comm 2>/dev/null); tid=$(basename $t)
  case "$n" in
    "PPU[0x1000000]"*) c=0;;
    rsx::thread) c=2;;
    "SPU[0x0000200]"*) c=4;;
    "SPU[0x0000100]"*) c=6;;
    "SPU[0x1000100]"*) c=8;;
    "SPU[0x3000100]"*) c=10;;
    "SPU[0x4000100]"*) c=12;;
    "SPU[0x2000100]"*) c=14;;
    "SPU[0x5000100]"*) c=15;;
    *) c=5,7,9,11,13;;
  esac
  taskset -pc $c $tid >/dev/null 2>&1
done
