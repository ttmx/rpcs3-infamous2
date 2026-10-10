#!/bin/bash
# pin6.sh on|off, for the six-SPU boot (nine busy threads on eight cores): on = the main PPU thread on a core of its
# own, the six SPURS SPU threads each on a core of their own, the render thread and the second instance's SPU thread
# (both wait a part of the time) on the two hardware threads of the last core, every other thread on the second
# hardware threads of the SPU cores; off = every thread anywhere again
cd "$(dirname "$0")/.."
pid=$(python3 -c "import json;print(json.load(open('state.json'))['emu'])")
if [ "$1" = off ]; then for t in /proc/$pid/task/*; do taskset -pc 0-15 $(basename $t) >/dev/null 2>&1; done; exit; fi
i=0
for t in /proc/$pid/task/*; do
  n=$(cat $t/comm 2>/dev/null); tid=$(basename $t)
  case "$n" in
    "PPU[0x1000000]"*) taskset -pc 0 $tid >/dev/null 2>&1;;
    rsx::thread) taskset -pc ${PIN_RSX:-2} $tid >/dev/null 2>&1;;
    "SPU[0x0000200]"*) taskset -pc ${PIN_NOISE:-3} $tid >/dev/null 2>&1;;
    SPU\[*) taskset -pc $((4 + 2 * (i % 6))) $tid >/dev/null 2>&1; i=$((i+1));;
    *) taskset -pc 5,7,9,11,13,15 $tid >/dev/null 2>&1;;
  esac
done
