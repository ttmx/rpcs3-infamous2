#!/usr/bin/env python3
"""Launch inFamous 2 (BCES01143 v1.04) on this RPCS3 build with the software optimizations enabled.

  launch.py [options] [GAME_DIR] [-- extra rpcs3 arguments]

GAME_DIR is the game's disc folder (the one that contains PS3_GAME); INFAMOUS2_GAME is used when it is not given.
The emulator binary is --rpcs3, then RPCS3_BIN, then build/bin/rpcs3 and bin/rpcs3 of this checkout.

Nothing here touches the host: no power profile, no GPU clock. Those live in hardware/launch.py.
"""
import argparse, os, pathlib, signal, subprocess, sys

root = pathlib.Path(__file__).resolve().parent
repo = root.parent

# Readback and SPU recompiler changes (the first round of work).
BASE_FLAGS = {
    'RPCS3_EXPERIMENT_SPU_INSTCOMBINE': '1',
    'RPCS3_VK_READBACK_ADMISSION': '1',
    'RPCS3_SPU_NATIVE_RWV': '1',
    'RPCS3_VK_READBACK_SHARED_HITS': '1',
    'RPCS3_VK_READBACK_COMPRESSED_HITS': '1',
    'RPCS3_VK_SEMAPHORE_PIPELINE_PREFETCH': '1',
    'RPCS3_VK_READBACK_STREAM_COPY': '1',
    'RPCS3_VK_READBACK_OOP': '1',
}

FLAGS = {
    **BASE_FLAGS,
    'RPCS3_VK_PERIODIC_SUBMIT_US': '1000',
    'RPCS3_EXPERIMENT_BLIT_COMPLEMENT': '1',
    'RPCS3_FIFO_INLINE_CACHE': '1',
    'RPCS3_EXPERIMENT_LAST_IMAGE_VIEW': '1',
    # Ambient occlusion computed on the GPU instead of by the game's SPU job (1 = use GPU image, 4 = idle the SPU job).
    # '0' is the game's own SPU version.
    'RPCS3_NATIVE_SSAO': '5',
    # Deferred lighting computed on the GPU instead of by the game's SPU job (1 = use GPU images, 4 = idle the SPU
    # job's pixel work, 8 = drop its writes of the two light images, 16 = neither SPU job loads the G-buffer images,
    # which then are not read back from the GPU, 32 = the game's two blits of those images are skipped as well and
    # the GPU passes read the render targets). '0' is the game's own SPU version.
    'RPCS3_NATIVE_LIGHTING': '61',
    # Static geometry kept on the GPU across frames (1 = on, 2 = on with a content check of every reuse, slow).
    'RPCS3_VK_GEOMETRY_CACHE': '1',
    # Repeat draws taken straight from the command stream (6 = on, 4 = on with every texture change checked against a
    # full program lookup, 3 = off but every qualifying draw is checked). Needs the geometry cache for most of its effect.
    'RPCS3_VK_FAST_DRAWS': '6',
    # Reuse validated unchanged pipeline properties. 2 audits every clean reuse.
    'RPCS3_VK_PIPELINE_REUSE': '1',
    # Compressed material image/sampler bindings (1 = on, 2 = full lookup with reuse comparisons). No reliable gain measured.
    'RPCS3_VK_MATERIAL_BINDINGS': '0',
    # Descriptor sets with identical contents are reused.
    'RPCS3_VK_DESCRIPTOR_REUSE': '1',
}


def find_binary(given=None):
    candidates = [given, os.environ.get('RPCS3_BIN'), repo / 'build/bin/rpcs3', repo / 'bin/rpcs3', repo / 'bin/rpcs3.exe']
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file():
            return pathlib.Path(candidate).resolve()
    sys.exit('RPCS3 binary not found: build this checkout, or pass --rpcs3 PATH or set RPCS3_BIN')


def cache_home(given=None):
    # A cache of its own, kept apart from the one a stock RPCS3 uses for the same game.
    if given:
        return pathlib.Path(given).resolve()
    base = os.environ.get('XDG_CACHE_HOME') or pathlib.Path.home() / '.cache'
    return pathlib.Path(base) / 'rpcs3-infamous2'


def base_env(flags=FLAGS, cache=None, profile=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith('RPCS3_') and k not in ('APPIMAGE', 'APPDIR')}
    env.update(flags)
    env['RPCS3_VK_LIVE_CTL'] = str(root / 'live.ctl')
    env['XDG_CACHE_HOME'] = str(cache_home(cache))
    if profile:
        env['XDG_CONFIG_HOME'] = str(pathlib.Path(profile).resolve())
    # Tested with the RPCS3 window under X11 / XWayland.
    if sys.platform.startswith('linux'):
        env.setdefault('QT_QPA_PLATFORM', 'xcb')
    return env


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game', nargs='?', default=os.environ.get('INFAMOUS2_GAME'), help='game disc folder')
    p.add_argument('--rpcs3', help='emulator binary')
    p.add_argument('--strict', action='store_true', help='use play-config-strict.yml (Strict Rendering Mode on)')
    p.add_argument('--config', help='another RPCS3 configuration file')
    p.add_argument('--input-config', default='Default', help='RPCS3 input profile (default: Default)')
    p.add_argument('--profile', help='directory to use as XDG_CONFIG_HOME, to run against another RPCS3 profile')
    p.add_argument('--cache', help='directory to use as XDG_CACHE_HOME (default: ~/.cache/rpcs3-infamous2)')
    p.add_argument('--set', action='append', default=[], metavar='NAME=VALUE', help='override one runtime switch')
    p.add_argument('extra', nargs=argparse.REMAINDER, help='arguments after -- go to rpcs3')
    return p


def command(args):
    if not args.game:
        sys.exit('Game folder not given: pass it as an argument or set INFAMOUS2_GAME')
    game = pathlib.Path(args.game).expanduser().resolve()
    if not game.exists():
        sys.exit(f'Game folder not found: {game}')
    config = pathlib.Path(args.config).resolve() if args.config else root / ('play-config-strict.yml' if args.strict else 'play-config.yml')
    env = base_env(cache=args.cache, profile=args.profile)
    env.update(item.split('=', 1) for item in args.set)
    extra = args.extra[1:] if args.extra[:1] == ['--'] else args.extra
    argv = [str(find_binary(args.rpcs3)), '--no-gui', '--input-config=' + args.input_config, '--config=' + str(config), *extra, str(game)]
    return argv, env


def main():
    argv, env = command(parser().parse_args())
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
    sys.exit(subprocess.call(argv, env=env))


if __name__ == '__main__':
    main()
