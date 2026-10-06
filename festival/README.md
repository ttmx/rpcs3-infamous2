# inFamous: Festival of Blood (NPEA00322 v1.00)

The standalone expansion runs on the inFamous 2 engine, so the inFamous 2 work applies to it nearly whole. Tested
with `infamous1/bench/harness.py` (`INFAMOUS_TITLE=NPEA00322`) on two savestates: `crypt` (first gameplay frame) and
`hall` (a few steps on, free control). Configuration: `infamous2/play-config.yml`.

## What is the same and what is not

- The occlusion job and its five kernels are the same bytes as in inFamous 2 (word by word against the inFamous 2
  executable: 0 of 2,258 + 3,508 words differ). The lighting job has the same program hash
  and the same first bytes of its tile function at the same place, but it was compiled again: 2,313 of 5,918 words
  differ, nearly all of it register numbering. The one change read so far is a tile test that skipped on
  "not greater" in inFamous 2 and skips on "less" here.
- Everything sits at other guest addresses: the two G-buffer copies and the occlusion scratch buffers 0x3cc00
  higher, the job parameters 0x4ee00 higher, the kernels 0x48000 higher. The occlusion image is at 0xcf800000 in
  both.

## Change

`Emu/infamous_titles.h` now holds one address set per game (`infamous_native::addresses`), chosen when the Vulkan
renderer starts; `VKNativeSSAO.cpp`, `VKNativeLighting.cpp` and the hooks in `SPUThread.cpp` read it instead of
constants. Any other title gets a set that matches nothing. The full resolution particles setting covers this title
too.

## Table

| inFamous 2 optimization | Festival of Blood | note |
|---|---|---|
| Ambient occlusion on the GPU | done | Same job, other addresses. Hall: 33.2 to 40.3 FPS on top of the generic changes. |
| Deferred lighting on the GPU | done | Same parameter block and light records; hall 40.3 to 73.9 FPS. Frozen frame against the game's own SPU lighting: largest difference 5 levels, 0.05% of pixels more than 2. |
| No G-buffer readback | done | Hall 73.9 to 85.9 FPS. |
| No G-buffer blit | done | Hall 85.9 to 89.2 FPS. |
| Half-resolution depth from the GPU pass | done | Part of the occlusion pass, address shifted with the scratch buffers. |
| Up to 256 lights, resolution scale, faster lighting passes | apply directly | Properties of the passes themselves; resolution scales other than 100% were not tried here. |
| Static geometry cache, fast repeat draws and the other draw submission changes | apply directly, on | Hall 83.4 to 89.2 FPS with the two on; the rest was not measured one by one for this game. |
| Transfer changes (partial upload after a blit, streaming loads and readbacks, readback admission) | apply directly, on | The game blits and reads back exactly as inFamous 2 does; with both GPU passes on there is almost nothing left for them (4 readbacks in 1,800 frames). |
| InstCombine, native SPU kernel | apply as in inFamous 2 | Not measured separately. |
| Full resolution particles | done | Title added to the check; hall 91.5 FPS off, 89.2 on. |
| Strict Rendering Mode off | applies | From the inFamous 2 configuration. |
| Accurate xfloat without doubles, material binding cache default (the inFamous 1 work) | not used | The configuration is Approximate, and the material default is for BCES00609 only. |

## Result

| scene (uncapped) | RPCS3 paths | generic changes | + occlusion | + lighting | + no readback | + no blit (fork default) |
|---|---|---|---|---|---|---|
| hall | 29.5 | 33.2 | 40.3 | 73.9 | 85.9 | 89.2 FPS |

Crypt (first frame): 34.0 FPS on RPCS3 paths, 119.7 on the fork. At the 60 cap with both passes: 60.0 FPS with
2.3 SPU cores and 0.36 of the render thread; RPCS3 paths reach 33 FPS there with 4 SPU cores.

Full-size captures of one frozen frame (SPU jobs twice, GPU occlusion only, GPU lighting only, both): the same frame
captured twice with the SPU jobs differs by as much as the GPU versions differ from it, apart from the animated
button prompt. Firing lightning in the hall looks the same in both and logs no unsupported light.

## Not done

- Only the catacombs at the start were reached: no city, no fire, no fights, no vampires. Scenes with many lights
  are where the turned-around tile test of the lighting job could matter, and none was compared.
- The lighting job's differences were not read through beyond that one test, and there is no offline reference
  run for this game as there was for inFamous 2.

## Running it

```sh
infamous2/launch.py /path/to/dev_hdd0/game/NPEA00322
```

The inFamous 2 launcher and configuration, pointed at the installed game folder. Tested to the opening cutscene on a
normal profile: 60 FPS, both passes active.

## inFamous 2 after the change

The dock savestate on this build: 60.00 FPS at the cap, both passes active (mode 61), no fatal error.

## Flickering light glows (2026-10-06, night)

Reported while the hall state was on screen: the glows around the candles and lanterns came and went from frame to
frame. They are drawn when the game's occlusion reports say the light is visible, and the game reads the reports
during the same frame.

| configuration, hall at the 60 FPS cap | glows |
|---|---|
| RPCS3 paths, `Relaxed ZCULL Sync: true` | absent, except for a moment about every 20 s |
| fork, `Relaxed ZCULL Sync: true` | on in roughly every other capture: the flicker |
| either, `Relaxed ZCULL Sync: false` | on in every capture (187 of 187 over 30 s on the fork) |

With the relaxed setting a report is written whenever the GPU happens to be done. Without periodic command
submission that is nearly always too late (so stock shows no glows at all); with it, about half the time. The
switch that makes the difference is periodic submission alone (live control 0), not the GPU passes, the fast draws
or the dynamic face state. Delivering all reports at the end of the frame instead (tried) does not help: still
flickering.

Fix: Festival of Blood runs with `Relaxed ZCULL Sync: false`. `infamous2/play-config-festival.yml` has it, and `infamous2/launch.py` picks that file when the game folder is
`NPEA00322`. Cost in the hall: 87.7 to 72.2 FPS uncapped; at the cap 60.0 FPS with the render thread at
0.84 of a core instead of 0.56.

Not checked: inFamous 2 uses the same engine and still runs with the relaxed setting. inFamous 1 in the `street` state is steady with the relaxed setting (50 captures, no
change in brightness).
