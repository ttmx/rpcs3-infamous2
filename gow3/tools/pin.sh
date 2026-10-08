#!/bin/bash
# pin.sh: give each of the test emulator's eight busy threads (six SPU, render thread, main PPU thread) a physical
# core of its own, on the first hardware thread of each core. Experiment for the fight scene.
cd "$(dirname "$0")/.."
pid=$(python3 -c "import json;print(json.load(open('state.json'))['emu'])")
firsts=($(lscpu -p=CPU,CORE | grep -v '^#' | awk -F, '!seen[$2]++ {print $1}'))
i=0
for t in /proc/$pid/task/*; do
  n=$(cat $t/comm 2>/dev/null)
  case "$n" in SPU\[*|rsx::thread|"PPU[0x1000000]"*) taskset -pc ${firsts[$((i % ${#firsts[@]}))]} $(basename $t) >/dev/null 2>&1; i=$((i+1));; esac
done
echo "pinned $i threads to cores ${firsts[*]}"
