#!/bin/bash
# uptest.sh <live control 19> [seconds]: the fight with the SPU vertex upload heap, its statistics lines and a screenshot
cd "$(dirname "$0")/.."
tools/fight.sh go up --defaults --cfg=test RPCS3_SPU_VERTEX_UPLOAD_STATS=1 >/dev/null 2>&1
python3 tools/gw.py ctl "1000 65536 1 0 0 0 0 0 1 2 0 1 1 0 0 0 0 0 0 $1"; python3 tools/gw.py seq "wait:10"
python3 tools/gw.py fps "${2:-8}" x | cut -c1-150; python3 tools/gw.py shot /tmp/up.png 800
python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9; pkill -f "[g]w.py key"
grep -a "SPU vertex upload" sessions/up/stdout.log | tail -3 | cut -c1-330
