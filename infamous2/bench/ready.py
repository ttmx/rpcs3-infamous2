#!/usr/bin/env python3
"""Wait until the game is in gameplay at the pier: minimap (blue disc, bottom right) visible. Presses Circle to leave menus."""
import subprocess, sys, time
def minimap():
    subprocess.run(['python3', 'run.py', 'shot', 'ready-probe.png'], check=True)
    out = subprocess.run(['magick', 'ready-probe.png', '-crop', '50x50+815+455', '-resize', '1x1!', '-format', '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]', 'info:'], capture_output=True, text=True).stdout.split()
    r, g, b = map(int, out)
    return b > r + 20 and b > 50 and r < 60, (r, g, b)
for attempt in range(30):
    ok, rgb = minimap()
    if ok:
        time.sleep(3)
        ok2, _ = minimap()
        if ok2:
            print('ready', rgb); sys.exit(0)
    subprocess.run(['python3', 'run.py', 'key', 'c']); time.sleep(4)
print('NOT READY', rgb); sys.exit(1)
