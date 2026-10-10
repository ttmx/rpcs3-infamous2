#!/bin/bash
# sixtry.sh <seconds> <patch lines...>: cold boot with a sixth SPU in the main SPURS instance plus these game patch
# lines, wait, and print where it stands: frame rate, access violations, the SPURS kernel context of each SPU
cd "$(dirname "$0")/.."
secs=$1; shift
{ printf 'Version: 1.2\n\nPPU-4d5c51503a81a327c2a99427390a395b8dcb3767:\n  "Geometry jobs on five SPUs":\n    Games:\n      "God of War III":\n        BCES00510: [ All ]\n    Author: local\n    Notes: test\n    Patch Version: 1.0\n    Patch:\n      - [ be32, 0x0028ec7c, 0x38c00006 ]\n'; for l in "$@"; do printf '      - [ be32, %s ]\n' "$l"; done; } > profile/rpcs3/patches/patch.yml
printf 'PPU-4d5c51503a81a327c2a99427390a395b8dcb3767:\n  "Geometry jobs on five SPUs":\n    "God of War III":\n      BCES00510:\n        All:\n          Enabled: true\n' > profile/rpcs3/patch_config.yml
rm -f /tmp/sx /tmp/sx.*
RPCS3_SPU_EXTRA_THREADS=1 python3 tools/gw.py launch six --defaults --cfg=test RPCS3_SPU_SAMPLER=/tmp/sx $SIX_ARGS >/dev/null 2>&1
sleep "$secs"
python3 tools/gw.py fps 2 x 2>&1 | cut -c1-170
grep -a "Access violation\|·F" cache/rpcs3/RPCS3.log | head -4 | cut -c1-260
grep -A1 "^5000100" /tmp/sx.kctx | cut -c1-220
[ -z "$SIX_KEEP" ] && { python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9; rm -f profile/rpcs3/patch_config.yml; }
