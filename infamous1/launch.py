#!/usr/bin/env python3
"""Start inFamous (BCES00609) on this build with your normal RPCS3 profile (saves, firmware, controller).

  launch.py [game folder] [--rpcs3 PATH] [--config FILE] [--set NAME=VALUE ...]

Differences from starting the game in stock RPCS3:
- this build (build/bin/rpcs3 of the checkout, or --rpcs3 / RPCS3_BIN), whose renderer and SPU changes are on by
  themselves; --set RPCS3_SPU_XFLOAT_FAST=0, --set RPCS3_VK_FAST_DRAWS=0 and so on turn one off
- play-config.yml next to this file instead of the per-game configuration: LLVM recompilers, 1280x720, SPU XFloat
  Accuracy Accurate (the game needs it), Strict Rendering Mode off, Multithreaded RSX off (it keeps one core
  spinning for no gain) and full resolution particles on (set "inFamous 2 Full Resolution Particles" to false there for the game's own 512x288)
- a cache of its own (~/.cache/rpcs3-infamous1), so the stock AppImage's cache is left alone; the first start
  compiles the game again, which takes a few minutes
- an X11 window (QT_QPA_PLATFORM=xcb)
"""
import argparse, os, pathlib, subprocess, sys

root = pathlib.Path(__file__).resolve().parent
p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('game', nargs='?', default=os.environ.get('INFAMOUS1_GAME', str(pathlib.Path.home() / 'Games/PS3/inFamous')))
p.add_argument('--rpcs3', default=os.environ.get('RPCS3_BIN', str(root.parent / 'build/bin/rpcs3')))
p.add_argument('--config', default=str(root / 'play-config.yml'))
p.add_argument('--set', action='append', default=[], metavar='NAME=VALUE')
args = p.parse_args()
if not pathlib.Path(args.rpcs3).is_file(): sys.exit('RPCS3 binary not found: ' + args.rpcs3)
if not (pathlib.Path(args.game) / 'PS3_GAME').is_dir(): sys.exit('No PS3_GAME in ' + args.game)
env = {k: v for k, v in os.environ.items() if not k.startswith('RPCS3_') and k not in ('APPIMAGE', 'APPDIR', 'LD_PRELOAD')}
env['XDG_CACHE_HOME'] = str(pathlib.Path(os.environ.get('XDG_CACHE_HOME') or pathlib.Path.home() / '.cache') / 'rpcs3-infamous1')
env.setdefault('QT_QPA_PLATFORM', 'xcb')
env.update(dict(s.split('=', 1) for s in args.set))
sys.exit(subprocess.call([args.rpcs3, '--no-gui', '--config=' + args.config, args.game], env=env))
