# Host tuning (one machine)

Nothing in this folder is needed for the software changes, and `../launch.py` never uses it. It is what was tried on
the development laptop (Ryzen 7 8845HS, Radeon 780M, amdgpu / RADV, power-profiles-daemon), kept so the measurements
in the log can be reproduced.

- `rpcs3-gpu-perf high|auto|floor-<MHz>|range`: sets the amdgpu power policy or a minimum GPU clock inside the stock
  range, through `power_dpm_force_performance_level` and `pp_od_clk_voltage`. Needs root.
- `launch.py`: `../launch.py` wrapped with a GPU policy (default `floor-1600`, restored to `auto` on exit) and a
  power profile held through `powerprofilesctl` (default `performance`). Takes the same arguments.

## Why it exists, and why it is not the default

In heavy scenes the amdgpu governor kept the 780M near 800-1300 MHz although the GPU was what the frame waited for.
Burning dock, on AC, before the game's SPU jobs were moved to the GPU:

| GPU policy | FPS | Package power |
|---|---|---|
| auto | 43.0 | 40.3 W |
| floor-1200 | 46.0 | 42.0 W |
| floor-1400 | 48.4 | 43.4 W |
| floor-1600 | 49.8 | 44.3 W |
| floor-2000 | 51.6 | 46.7 W |
| high (2700 MHz) | 54.7 | 53.5 W, CPU at 100 C and throttling within a minute |

On an integrated GPU the extra heat lands on the CPU, which then throttles, so `high` is not a free gain and
`floor-1600` was the best FPS per watt. Later software changes removed most of the reason for it: with the current
build the same scene holds the 60 FPS cap on `auto` at about 37 W and 1200 MHz. Measure before keeping it on.

## Installing the helper

```sh
sudo install -m 755 rpcs3-gpu-perf /usr/local/bin/rpcs3-gpu-perf
echo "$USER ALL=(root) NOPASSWD: /usr/local/bin/rpcs3-gpu-perf" | sudo tee /etc/sudoers.d/zz-rpcs3-gpu-perf
sudo chmod 440 /etc/sudoers.d/zz-rpcs3-gpu-perf
rpcs3-gpu-perf range        # prints the card it found and its clock range
```

The sudoers rule lets your user change the GPU clock policy without a password; leave it out and run the helper by
hand if you prefer. `launch.py` calls it with `sudo -n` and carries on silently when that fails.
