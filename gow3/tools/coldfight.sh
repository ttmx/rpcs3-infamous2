#!/bin/bash
# coldfight.sh <session> <windows> [launch arguments]: cold.sh, then that many 5-second windows of the fight (Kratos
# revived every sixth), a picture, and what the log says; stops the emulator. COLD_KEEP=1 leaves it running.
cd "$(dirname "$0")/.."
s=$1; n=$2; shift 2
tools/cold.sh "$s" "$@"
python3 tools/gw.py seq "wait:12"; python3 tools/gw.py revive >/dev/null
for i in $(seq "$n"); do
  python3 tools/gw.py fps 5 x | python3 -I -c "import sys,json; r=json.loads(sys.stdin.read()); print(r['fps'],r['spu_cores'],r['rsx_cores'],r.get('W',''))"
  [ $((i % 6)) = 0 ] && python3 tools/gw.py revive >/dev/null
done > /tmp/cf-$s.txt
python3 tools/gw.py shot /tmp/cf-$s.png 640 >/dev/null
if [ -z "$COLD_KEEP" ]; then python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9; pgrep -x python3 -a | grep "[g]w.py key" | awk '{print $1}' | xargs -r kill; fi
awk '$2>4.2{s+=$1;c++; if($1<m||!m)m=$1; l=l" "int($1+.5)} END{print "'"$s"': windows",c,"of",NR,"mean fps",s/c,"min",m,"|",l}' /tmp/cf-$s.txt
grep -a "God of War III\|Access violation\|·F" cache/rpcs3/RPCS3.log | head -4 | cut -c1-160
