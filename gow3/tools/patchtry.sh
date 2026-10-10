#!/bin/bash
# patchtry.sh <name> <patch lines...>: write the test profile's game patch with these lines, cold-boot, and report
# whether the game reaches the first fight or stops (frame counter frozen) on the way
cd "$(dirname "$0")/.."
name=$1; shift
{ printf 'Version: 1.2\n\nPPU-4d5c51503a81a327c2a99427390a395b8dcb3767:\n  "Geometry jobs on five SPUs":\n    Games:\n      "God of War III":\n        BCES00510: [ All ]\n    Author: local\n    Notes: test\n    Patch Version: 1.0\n    Patch:\n'; for l in "$@"; do printf '      - [ be32, %s ]\n' "$l"; done; } > profile/rpcs3/patches/patch.yml
pgrep -x python3 -a | grep "[g]w.py key" | awk '{print $1}' | xargs -r kill 2>/dev/null
rm -f /tmp/pt /tmp/pt.*
python3 tools/gw.py launch "pt-$name" --defaults --cfg=test RPCS3_SPU_SAMPLER=/tmp/pt >/dev/null 2>&1
state=timeout
for i in $(seq 50); do
  r=$(python3 tools/gw.py fps 8 cold 2>/dev/null | python3 -I -c "import sys,json; r=json.loads(sys.stdin.read()); print(r['spu_cores'], r['ppu_cores'], r['rsx_cores'])" 2>/dev/null)
  python3 tools/gw.py seq "x:0.06" 2>/dev/null
  set -- $r
  if [ -n "$1" ] && python3 -I -c "import sys; sys.exit(0 if float('$1') > 4.6 else 1)"; then state=fight; break; fi
  if [ $i -gt 6 ] && [ -n "$2" ] && python3 -I -c "import sys; sys.exit(0 if float('$2') < 0.2 and float('$3') > 0.95 else 1)"; then state="hung after $((i*9)) s"; break; fi
done
echo "$name: $state; $(grep '^1[0-2] ' /tmp/pt.spurs 2>/dev/null | head -1 | cut -c1-30)"
