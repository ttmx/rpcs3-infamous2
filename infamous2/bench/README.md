# Benchmark and test scripts

What produced the numbers in `../docs/INVESTIGATION.md`. They drive a real emulator window through X11 (keyboard
input, window title FPS, screenshots), so they need `xwininfo`, `xprop`, ImageMagick (`import`, `magick`), `bc`, and an
X11 or XWayland session. Power, GPU clock and temperature columns come from amdgpu and `k10temp` sysfs files and stay
empty on other hardware.

Not included, because they contain game data: savestates and the RPCS3 profile they run against. To use the
scripts, provide:

- `profile/rpcs3/`: a copy of your RPCS3 configuration folder (`~/.config/rpcs3`), so tests never touch your real
  saves, with an input profile named `ProfilingKeyboard` that maps the pad to the keyboard.
- `states/dock.SAVESTAT.zst`, `states/city.SAVESTAT.zst`: savestates of the scenes to measure. Creating one needs the
  settings described under "Savestates" in the top-level README (`cfg-nostrict-compat.yml`).
- A compiled cache: the first boot of a savestate on an empty cache needs the game's SPU block list, which
  `live_test.py` copies from the play cache (`~/.cache/rpcs3-infamous2`, or `LIVE_TEST_SEED_CACHE`).

Scripts:

- `live_test.py launch|status|stop|shot|key`: start one identity-checked test emulator on `profile/` and talk to it.
  `NAME=VALUE` arguments set runtime switches, `CFG:setting=value` overrides one line of `../play-config.yml`.
- `city.sh <label>`, `heavy.sh <label>`: boot the city state (or `STATE=dock`), the second one uncapped and with the
  camera turned to the slow street view. Sessions go to `sessions/<label>`.
- `ab3.sh <label> <reps> <seconds> "<live controls 8..>" ...`: switch changes on and off inside one boot through
  `live.ctl` and print FPS, power, GPU clock, GPU busy and render-thread CPU per arm. `ab.sh` is the older two-control
  form. Only arms switched inside one boot are comparable: the camera does not land on the same view every boot.
- `spawn-bench.sh <label> <binary> <state> <capped|uncapped>`: one build at a state's spawn camera, four arms of 10 s.
- `replace_bench.sh`, `lightning_shots.sh`: dock state standing still and firing lightning, per lighting mode.
- `roam.sh <rounds>`: walk, turn, jump and fire, for the content-check modes.
- `run.py launch|stop|key|fps|shot`, `pier-bench.sh`, `dock-bench.sh`, `ready.py`, `cpu.sh`: the earlier harness, which
  boots the game or a state on your real profile (or `BENCH_PROFILE`); the others use its `fps` and `key`.
- `analyze.py`, `fifo_shapes.py`, `gbuffer_shapes.py`, `shadow_stats.py`: readers for `RPCS3_VK_GEOM_TRACE` captures.
- `x11_hotkey.py`: sends a hotkey such as Ctrl+S (savestate) to the game window.
- `cfg-bench-vb120.yml` (frame limit off, vblank 120, to see differences above the game's 60 FPS cap),
  `cfg-nostrict-compat.yml` (savestate-compatible SPU mode), `cfg-spu-interp.yml` (SPU interpreter, for captures).

The meaning of each `live.ctl` value is in `rpcs3/Emu/RSX/VK/VKLiveCtl.hpp`.
