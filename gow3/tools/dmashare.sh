#!/bin/bash
# dmashare.sh [launch arguments]: the transfer functions' share of the three geometry SPU threads' time in the fight (perf, 2 x 8 s)
cd "$(dirname "$0")/.."
timeout 560 tools/fight.sh go ds --defaults --cfg=test "$@" >/dev/null 2>&1; python3 tools/gw.py seq "wait:25"
for i in 1 2; do python3 tools/gw.py revive >/dev/null; pid=$(pgrep -x rpcs3 | head -1)
tids=$(for t in /proc/$pid/task/*; do c=$(cat $t/comm 2>/dev/null); case "$c" in "SPU[0x0000100]"*|"SPU[0x1000100]"*|"SPU[0x2000100]"*) basename $t;; esac; done | paste -sd,)
perf record -o /tmp/ds.data -F 1500 -t $tids -- sleep 8 >/dev/null 2>&1
perf report -i /tmp/ds.data --no-children --sort symbol 2>/dev/null | grep -E "do_dma_transfer|do_list_transfer|process_mfc_cmd|readback_copy::stream" | awk '{s+=$1; printf "%s ", $1} END{print "sum", s}'; done
python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9; pgrep -x python3 -a | grep "[g]w.py key" | awk '{print $1}' | xargs -r kill
