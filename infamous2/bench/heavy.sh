#!/bin/bash
# heavy.sh <label> [args for city.sh]: boot the city state uncapped and turn the camera about 60 degrees to the right,
# to the slow street view (the user's reference view; about 10.7k draws per frame).
cd "$(dirname "$0")" || exit 1
label="$1"; shift
./city.sh "$label" "CFG:Frame limit='Off'" "CFG:Vblank Rate='120'" "$@" || exit 1
sleep 8; python3 live_test.py key Next 0.30; sleep 4
python3 live_test.py shot "sessions/$label/view.png"
