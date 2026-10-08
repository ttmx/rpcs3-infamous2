#!/usr/bin/env python3
"""Start God of War III (BCES00510) on the fork build with your normal RPCS3 profile (saves, firmware, controller).

  launch.py [game folder or .iso] [--rpcs3 PATH] [--config FILE] [--set NAME=VALUE ...] [--scale N]

Differences from starting the game in stock RPCS3:
- the fork build (native-investigation/build-prep/build/bin/rpcs3), whose changes are on by themselves. For this
  game: --set RPCS3_NATIVE_AA=0 gives the game's own SPU anti-aliasing back, --set RPCS3_SPU_PUTLLC_BACKOFF=0 turns
  the back-off for the SPU job queue off
- play-config.yml next to this file instead of a per-game configuration: RPCS3's defaults. Relaxed ZCULL Sync has to
  stay off for this game (its light and shadow passes read occlusion reports within the frame)
- a cache of its own (~/.cache/rpcs3-gow3), so the stock AppImage's cache is left alone
- an X11 window (QT_QPA_PLATFORM=xcb)
- --fast: play-config-fast.yml, the same with SPU XFloat Accuracy: Relaxed. +10% in the first fight (44.8 against
  40.8 FPS over 6 loads each). It drops the emulation of the console's float corner cases (no infinities or NaNs
  there), so a game calculation that runs into one can come out differently; none was seen in the first fight
The game's speed follows the frame rate: leave the 60 FPS frame limit on.
"""
import argparse, os, pathlib, subprocess, sys

root = pathlib.Path(__file__).resolve().parent
p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('game', nargs='?', default=os.environ.get('GOW3_GAME', str(pathlib.Path.home() / 'Games/PS3/GodOfWar3/GodOfWar3.iso')))
p.add_argument('--rpcs3', default=os.environ.get('RPCS3_BIN', str(root.parent / 'native-investigation/build-prep/build/bin/rpcs3')))
p.add_argument('--config', default=str(root / 'play-config.yml'))
p.add_argument('--set', action='append', default=[], metavar='NAME=VALUE')
p.add_argument('--scale', type=int)
p.add_argument('--fast', action='store_true')
args = p.parse_args()
if args.fast and args.config == str(root / 'play-config.yml'): args.config = str(root / 'play-config-fast.yml')
if args.scale:
    # RPCS3 takes one configuration file: write a copy with the scale next to the cache
    import re, tempfile
    text = re.sub(r'(?m)^(\s*Resolution Scale:).*$', r'\1 %d' % args.scale, pathlib.Path(args.config).read_text())
    args.config = str(pathlib.Path(tempfile.gettempdir()) / 'rpcs3-gow3-config.yml'); pathlib.Path(args.config).write_text(text)
if not pathlib.Path(args.rpcs3).is_file(): sys.exit('RPCS3 binary not found: ' + args.rpcs3)
if not (pathlib.Path(args.game).is_file() or (pathlib.Path(args.game) / 'PS3_GAME').is_dir()): sys.exit('No disc image or PS3_GAME folder at ' + args.game)
env = {k: v for k, v in os.environ.items() if not k.startswith('RPCS3_') and k not in ('APPIMAGE', 'APPDIR', 'LD_PRELOAD')}
env['XDG_CACHE_HOME'] = str(pathlib.Path(os.environ.get('XDG_CACHE_HOME') or pathlib.Path.home() / '.cache') / 'rpcs3-gow3')
env.setdefault('QT_QPA_PLATFORM', 'xcb')
env.update(dict(s.split('=', 1) for s in args.set))
sys.exit(subprocess.call([args.rpcs3, '--no-gui', '--config=' + args.config, args.game], env=env))
