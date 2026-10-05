#!/bin/bash
# roam.sh <rounds>: walk, turn the camera, jump and fire lightning in the running test emulator (keyboard profile).
cd "$(dirname "$0")" || exit 1
k() { python3 live_test.py key "$1" "$2"; }
for i in $(seq 1 "${1:-6}"); do
  k w 2.5; k Next 0.7; k w,x 1.5; k q,e 0.6; k Delete 1.2; k w 2.0; k q,e 0.6; k d 1.0; k Next 0.5; k x 0.2; k w 1.5; k Home 0.3; k End 0.3
done
