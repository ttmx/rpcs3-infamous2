#!/usr/bin/env python3
"""Start Ratchet & Clank Future: Tools of Destruction (BCES00052) on this build with your normal RPCS3 profile
(saves, firmware, controller).

  launch.py [game folder] [--rpcs3 PATH] [--config FILE] [--set NAME=VALUE ...]

Differences from starting the game in stock RPCS3:
- this build (build/bin/rpcs3 of the checkout, or --rpcs3 / RPCS3_BIN), whose renderer changes are on by themselves;
  --set RPCS3_VK_PERIODIC_SUBMIT_US=0, --set RPCS3_VK_GEOMETRY_CACHE=0, --set RPCS3_VK_FAST_DRAWS=0 turn the three
  that matter for this game off
- play-config.yml next to this file instead of a per-game configuration: RPCS3's defaults with Multithreaded RSX
  off. Leave "Disable ZCull Occlusion Queries" off: with it on Ratchet and other moving objects are not drawn
- a cache of its own (~/.cache/rpcs3-ratchet-tod), so the stock AppImage's cache is left alone
- --unlocked: play-config-unlocked.yml (frame limit off). Above 75 FPS this needs the game patch "Unlock frame
  rate" (patch-unlock-frame-rate.yml here: import it in RPCS3's patch manager and
  enable it); without it the game stops at 81 FPS
- --scale N: resolution scale in percent. Above 100 the GPU is the limit; --set RPCS3_VK_MSAA_PIXEL_SHADING=1
  (one shader run per pixel instead of per sample) or MSAA: Disabled in the configuration make it cheaper
- an X11 window (QT_QPA_PLATFORM=xcb)
"""
import argparse, os, pathlib, subprocess, sys

root = pathlib.Path(__file__).resolve().parent
p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('game', nargs='?', default=os.environ.get('RATCHET_TOD_GAME', str(pathlib.Path.home() / 'Games/PS3/RatchetToD')))
p.add_argument('--rpcs3', default=os.environ.get('RPCS3_BIN', str(root.parent / 'build/bin/rpcs3')))
p.add_argument('--config', default=str(root / 'play-config.yml'))
p.add_argument('--set', action='append', default=[], metavar='NAME=VALUE')
p.add_argument('--unlocked', action='store_true')
p.add_argument('--scale', type=int)
args = p.parse_args()
if args.unlocked and args.config == str(root / 'play-config.yml'): args.config = str(root / 'play-config-unlocked.yml')
if args.scale:
    # RPCS3 takes one configuration file: write a copy with the scale next to the cache
    import re, tempfile
    text = re.sub(r'(?m)^(\s*Resolution Scale:).*$', r'\1 %d' % args.scale, pathlib.Path(args.config).read_text())
    args.config = str(pathlib.Path(tempfile.gettempdir()) / 'rpcs3-ratchet-tod-config.yml'); pathlib.Path(args.config).write_text(text)
if not pathlib.Path(args.rpcs3).is_file(): sys.exit('RPCS3 binary not found: ' + args.rpcs3)
if not (pathlib.Path(args.game) / 'PS3_GAME').is_dir(): sys.exit('No PS3_GAME in ' + args.game)
env = {k: v for k, v in os.environ.items() if not k.startswith('RPCS3_') and k not in ('APPIMAGE', 'APPDIR', 'LD_PRELOAD')}
env['XDG_CACHE_HOME'] = str(pathlib.Path(os.environ.get('XDG_CACHE_HOME') or pathlib.Path.home() / '.cache') / 'rpcs3-ratchet-tod')
env.setdefault('QT_QPA_PLATFORM', 'xcb')
env.update(dict(s.split('=', 1) for s in args.set))
sys.exit(subprocess.call([args.rpcs3, '--no-gui', '--config=' + args.config, args.game], env=env))
