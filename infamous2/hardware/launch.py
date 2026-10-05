#!/usr/bin/env python3
"""../launch.py plus host tuning for one machine: Ryzen 7 8845HS laptop, Radeon 780M, RADV, Linux.

  hardware/launch.py [--gpu floor-1600|high|auto] [--power-profile performance|none] [launch.py options] [GAME_DIR]

--gpu            GPU clock policy while the game runs, restored to auto on exit. The amdgpu governor left the 780M
                 near 800-1300 MHz in heavy scenes. Needs the rpcs3-gpu-perf helper installed (see README.md here).
--power-profile  power-profiles-daemon profile held for the session.

Both cost power and heat; see README.md in this folder for the measurements before using them elsewhere.
"""
import argparse, pathlib, shutil, signal, subprocess, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import launch

p = launch.parser()
p.description = __doc__
p.add_argument('--gpu', default='floor-1600', help='floor-<MHz>, high or auto (default: floor-1600)')
p.add_argument('--gpu-helper', default='/usr/local/bin/rpcs3-gpu-perf', help='installed copy of rpcs3-gpu-perf')
p.add_argument('--power-profile', default='performance', help="profile to hold, or 'none' (default: performance)")
args = p.parse_args()
argv, env = launch.command(args)

power = shutil.which('powerprofilesctl')
if power and args.power_profile != 'none':
    argv = [power, 'launch', '--profile', args.power_profile, '--reason', 'RPCS3 gameplay', '--appid', 'rpcs3', *argv]


def gpu(policy):
    subprocess.run(['sudo', '-n', args.gpu_helper, policy], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
code = 1
try:
    if args.gpu != 'auto':
        gpu(args.gpu)
    code = subprocess.call(argv, env=env)
finally:
    if args.gpu != 'auto':
        gpu('auto')
sys.exit(code)
