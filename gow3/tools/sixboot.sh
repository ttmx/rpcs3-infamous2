#!/bin/bash
# sixboot.sh [launch arguments]: cold boot with six SPUs in the main SPURS instance, the sixth a geometry worker
# (game patch in the test profile, removed again by `sixboot.sh off`)
cd "$(dirname "$0")/.."
if [ "$1" = off ]; then rm -f profile/rpcs3/patch_config.yml; exit; fi
{ printf 'Version: 1.2\n\nPPU-4d5c51503a81a327c2a99427390a395b8dcb3767:\n  "Geometry jobs on five SPUs":\n    Games:\n      "God of War III":\n        BCES00510: [ All ]\n    Author: local\n    Notes: test\n    Patch Version: 1.0\n    Patch:\n'; for l in ${SIX_PATCH:-"0x0028ec7c,0x38c00006" "0x0028dd08,0x38600006" "0x0028dd2c,0x3a600006" "0x0028de08,0x3c000505" "0x0028de0c,0x60000500" "0x0028de10,0x90010070" "0x0028de14,0x92410074" "0x0028de18,0x9b210075"}; do printf '      - [ be32, %s ]\n' "${l/,/, }"; done; } > profile/rpcs3/patches/patch.yml
printf 'PPU-4d5c51503a81a327c2a99427390a395b8dcb3767:\n  "Geometry jobs on five SPUs":\n    "God of War III":\n      BCES00510:\n        All:\n          Enabled: true\n' > profile/rpcs3/patch_config.yml
RPCS3_SPU_EXTRA_THREADS=1 python3 tools/gw.py launch six --defaults --cfg=test RPCS3_SPURS_EXTRA_WORKLOADS=${SIX_WORKLOADS:-1c00} "$@" >/dev/null 2>&1
