#!/bin/bash
# spawn-bench.sh <label> <binary> <state> <capped|uncapped>: boot a state at its spawn camera with the
# play settings and print FPS, power, GPU clock and GPU busy over 4 arms of 10 s
cd "$(dirname "$0")" || exit 1
label="$1"; binary="$2"; state="$3"; cap="$4"
extra=(); [ "$cap" = uncapped ] && extra=("CFG:Frame limit='Off'" "CFG:Vblank Rate='120'")
LIVE_TEST_BINARY="$binary" STATE="$state" ./city.sh "$label" RPCS3_VK_GEOMETRY_CACHE=1 RPCS3_VK_FAST_DRAWS=6 RPCS3_VK_PIPELINE_REUSE=1 RPCS3_VK_MATERIAL_BINDINGS=0 RPCS3_VK_DESCRIPTOR_REUSE=1 "${extra[@]}" >/dev/null || exit 1
sleep 12
./ab3.sh "$label" 4 10 "1 6 0 1 1" | tee "sessions/$label/bench.txt" | python3 -c '
import re,sys
v=[[float(x) for x in re.search(r"fps ([\d.]+) .* W ([\d.]+) gpuMHz (\d+) cpuC (\d+) gpuBusy (\d+)",l).groups()] for l in sys.stdin if "fps" in l]
n=len(v); print("'"$label"': fps %.2f  W %.2f  gpuMHz %.0f  cpuC %.1f  gpuBusy %.1f%%"%tuple(sum(x[i] for x in v)/n for i in range(5)))'
python3 live_test.py shot "sessions/$label/view.png" >/dev/null
python3 live_test.py stop >/dev/null
