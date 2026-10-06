# inFamous (BCES00609 v1.00): what the inFamous 2 work does for it

The first game went through the same list as inFamous 2: for every change, whether it applies as it is, whether the
game has a similar mechanism, or why not. Then two things of its own were added. `launch.py` here starts the game
(see "Running it"); `bench/harness.py` is the test tool (run it without arguments), with the per-run configurations
`bench/cfg-*.yml`. Savestates, the test profile and captures contain game data and are not included.

All figures: Ryzen 7 8845HS / Radeon 780M on AC, power profile "balanced", 1280x720, frame limit off and vblank 120
(`bench/cfg-bench.yml`), switched live inside one boot or one boot per arm (boots of the same arm were within 1 FPS of each other, less inside one hour).

## What the game does differently from inFamous 2

- No SPU image work on rendered frames. The executable has none of the occlusion kernels, no lighting job (byte
  search, `bench/compare_jobs.py`), and at run time the renderer never copies an image from the GPU to guest memory:
  0 readbacks, 0 blit engine transfers and 0 buffer copies over title screen, intro cutscene and crater
  (`RPCS3_ROUNDTRIP_SURVEY=<file>`). It is a forward renderer.
- The only textures the SPUs write are small: Bink video planes for the billboards and TVs (128x96 and 256x192, one
  byte per pixel), a 64x64 two-channel noise image and a 64x36 image
  (`RPCS3_ROUNDTRIP_SURVEY_DUMP=<directory>`).
- The frame is draw calls: about 10,200 per frame at the title city, 7,000 of them into a 1024x1024 shadow map,
  2,200 into the 1280x720 picture. The render thread is at one full core in every measurement, so it is the limit;
  the GPU sits near 1000 MHz.

## Result

Three savestates: `title` (the in-engine city behind the title screen), `crater` (first seconds of gameplay), `fire`
(standing in a burning street a little further on). Game configuration as saved from the home menu (XFloat
Accurate), Multithreaded RSX off.

| scene | RPCS3 paths (every fork switch off) | fork, as it starts by itself | at the 60 FPS cap: SPU threads, RPCS3 paths to fork |
|---|---|---|---|
| title city | 54.3 FPS | 76.5 FPS | 53.7 FPS at 3.5 cores to 60.0 FPS at 3.5 cores |
| crater | 47.3 FPS | 66.5 FPS | 46.8 FPS at 4.1 cores to 59.3 FPS at 4.3 cores |
| fire | 64.3 FPS | 79.0 FPS | 58.6 FPS at 4.5 cores to 60.0 FPS at 3.9 cores |

The package power sensor sits at the profile's 30 W limit in every one of these runs, so it says nothing here; the
frame rates are what that budget buys.

What limits the frame: at the title city the render thread (10,200 draws per frame). At the crater and in the fire
the game's own threads: all five SPU workers are busy and the main thread waits for them for a third of the frame.
Dropping every shadow draw, as an experiment, gives 85 FPS at the title city and 69 at the crater, which is where
the game side stops the frame rate with the renderer out of the way.

## Table

| inFamous 2 optimization | inFamous 1 | note |
|---|---|---|
| Ambient occlusion on the GPU | not applicable | The game has no occlusion SPU job: none of the five kernels or their dispatcher is in the executable and nothing is read back from the GPU for one to work on. |
| Deferred lighting on the GPU | not applicable | The game lights in its forward passes; there is no lighting SPU job and no G-buffer copy. |
| No G-buffer readback | not applicable | There are no readbacks to remove (0 in every scene surveyed). |
| No G-buffer blit | not applicable | The game issues no blit engine transfers at all. |
| Half-resolution depth from the GPU pass | not applicable | That texture was an output of the occlusion job, which this game does not have. |
| Up to 256 lights per frame | not applicable | Part of the GPU lighting pass. |
| Resolution scale in the GPU passes | not applicable | Part of the GPU passes; RPCS3's own resolution scale needs nothing extra here. |
| Faster lighting passes | not applicable | Part of the GPU lighting pass. |
| Static geometry kept on the GPU | applies directly, on | 99.8% of vertex and index requests are served from the cache; alone +12% at the crater and at the title city. |
| Fast repeat draws | applies directly, on | 9,030 of 10,150 draws per frame take the fast path; alone +19% (crater), +16% (title); with the geometry cache +29% and +36%. |
| Texture and polygon offset changes inside fast runs | applies directly, on | This game changes textures inside runs often (1,490 times per frame): title city 66.9 to 74.1 FPS. |
| Descriptor set reuse | applies directly, on | 1,600 sets reused per frame against 150 written; no measurable FPS difference in either scene. |
| Pipeline state reuse | applies directly, on | Active (300 states cleared per frame); no measurable difference, as in inFamous 2. |
| Periodic command submission | applies directly, on | No measurable difference: the GPU is not what the frame waits for. |
| Submit during semaphore waits | applies directly, on | It triggers (the game waits on the semaphore), no measurable difference for the same reason. |
| Same-frame vertex cache | applies directly, on | No measurable difference; with the geometry cache on nothing is left for it (0 same-frame hits). |
| Inline command fetch | applies directly, on | Within noise (+0.3 FPS at the title city). |
| Last image view | applies directly, on | Within noise (+0.3 FPS at the title city). |
| Partial upload after a blit | not applicable | No blits. |
| Streaming loads | not applicable in practice | The game's uploads from guest memory are at most 64 KiB, the threshold; no measurable difference. |
| Streaming readback copy | not applicable | No readbacks. |
| Fused readback conversion | not applicable | No readbacks. |
| Serialized foreign readbacks | not applicable | No readbacks. |
| Texture hits during a readback | not applicable | No readbacks. |
| InstCombine in the SPU recompiler | applies directly | No measurable difference with the switch set, at the crater or the title city. |
| Native SPU kernel (RWV) | not applicable | It is matched by its bytes and the block does not occur among the 3,400 SPU functions this game has built so far. |
| Savestates | applies directly | Creating one needs the same recipe (InstCombine and the native kernel off, Compatible Savestate Mode on); the states load under the normal settings. |
| Full resolution particles | done | The game uses the same 512x288 particle target, so the setting now also acts for BCES00609 (only the title check differs); crater 64.5 to 62.4 FPS, fire is visibly sharper (`images/particles-off-top-on-bottom.png`). |
| Strict Rendering Mode off | applies directly, and matters more | On: 18.7 FPS at the title city against 54 (RPCS3 paths) or 74 (fork); the game's configuration already has it off. |
| GPU clock floor | not applicable | The GPU is not the limit: under the performance profile it runs at 1500-1800 MHz for the same FPS. |
| Performance power profile | not useful here | Same FPS as "balanced" (73.7 against 75.8 at the title city) for 1-3 W more. |

## New work

| change | state | note |
|---|---|---|
| Accurate xfloat without doubles (`RPCS3_SPU_XFLOAT_FAST`, on by default, acts only when `SPU XFloat Accuracy` is Accurate) | done | The game needs Accurate (RPCS3 wiki: enemies disappear and a boss kill crashes without it), and Accurate costs a fifth of the frame rate where the SPUs are the limit (fire: 70.7 FPS against 87.1 with Approximate). The game's float code is mostly scalar: load, rotate into place, one operation, insert, store, so nearly every operation converts its operands to doubles and its result back. Now add, subtract, multiply, the three multiply-add forms and the four compares run in single precision, which gives the same bits because the SPU threads already run with truncation and denormals as zero; operands with the largest exponent and results at the host float limit (1.6% of operations in the fire) take the old double code, and a zero result is made positive. Fire 71.2 to 80.9 FPS, crater 63.7 to 65.4, title 75.1 to 76.2; at the 60 cap 0.3 to 0.5 fewer SPU cores. Check mode (`=2`) compared 2.5 billion results with the double code over the three scenes: no difference. inFamous 2 runs Approximate and is not affected. |
| Material binding cache on for this game (live control 10 = 5, `RPCS3_VK_MATERIAL_BINDINGS`) | done | The cache of compressed texture bindings that showed no reliable gain in inFamous 2 is now the default for BCES00609 only: this game sets its textures up again for most draws of the picture (4,700 lookups per frame, 72% answered by the cache). Switched live at the title city 74.3 to 76.9 FPS, one boot per arm +1.1% (title), +1.3% (crater), +0.4% (fire). Its check mode (6) compared every reuse with the full lookup at the three scenes, with a camera turn: 5.2 million reuses, no mismatch. A version that never invalidates reaches 78.6 FPS, so a finer invalidation is worth 2% more at most and was not built. |
| Full resolution particles for this game | done | See the table above. |
| Main thread waits for a write instead of spinning (`rejected/job-wait.patch`) | tried, not kept | The spin loop at 0x41fc60 is a third of the main thread, but waiting on the counters' cache line (MONITORX/MWAITX) changed neither FPS nor power: the waits are 10 microseconds on average. The first version waited without marking the thread as paused and halved the frame rate, because other threads' memory locks then wait for it. |
| Lane shuffles on doubles (`rejected/xfloat-shuffle.patch`) | tried, not kept | Whole-word rotations, constant shuffles and register moves of double-held values without converting; 5 billion checked, no mismatch, and no FPS difference: the conversions that cost are at loads and stores. Superseded by the change above. |

## Open city, from a post-story save

An EU save found online ("40. Aftermath", hero, 89%, all districts and powers; its `STATE` file is not encrypted, so
the folder works in RPCS3 as it is). With it the game loads straight into the city after the logos. States: `roof`
(where the save starts) and `street` (street level nearby).

| scene | RPCS3 paths | fork |
|---|---|---|
| roof | 73.8 FPS | 83.0 FPS |
| street | 68.6 FPS | 80.0 FPS (76.0 without the fast accurate xfloat) |

Running and jumping through the district: 67 to 108 FPS on the fork. Bolts, grenades, shockwave and the hammer on
the roof: 73 to 89 FPS. Check modes at `street` while moving: material bindings 485,000 reuses, no mismatch;
fast xfloat 326 million operations, no mismatch. Nothing below 60 FPS was found, but only this one district was
visited, in daylight, with no enemies in view; the busier districts and fights are still untested.

## Running it

```sh
infamous1/launch.py /path/to/inFamous        # the disc folder that contains PS3_GAME
```

It uses your normal RPCS3 profile, `play-config.yml` and a cache of its own (`~/.cache/rpcs3-infamous1`); `--rpcs3`
names another binary, for example a release AppImage. Tested to the title screen and, on the test profile, in the
scenes above.

## Limits

- Tested in the first minutes of the game and in one district of the open city in daylight, with no enemies in
  view. Neither of the two bugs the RPCS3 wiki ties to xfloat accuracy (disappearing enemies, a crash when a boss is
  killed) was exercised. The check modes say the new paths give the same results as the old ones in the scenes that
  were run; they say nothing about the rest.
- `RPCS3_SPU_XFLOAT_FAST` is on for every title that runs with `SPU XFloat Accuracy: Accurate`; only this game was
  tested with it.
- Seen on the way, not changed: `RPCS3_EXPERIMENT_SPU_INSTCOMBINE`, `RPCS3_SPU_NATIVE_RWV` and the other switches
  that `SPULLVMRecompiler.cpp` reads into namespace-scope constants are read before `main()` sets the defaults of
  `infamous2_defaults.h`, so those defaults do not reach them; they are on only when a launcher (or the environment)
  sets them. The new switch is read at first use for that reason.

## Configuration findings

- `SPU XFloat Accuracy`: keep Accurate (see above). Approximate is still 8% faster than the new path in the fire
  (87 against 81 FPS) and nothing here tested whether it shows the wiki's bugs.
- `Strict Rendering Mode` must stay off: 18.7 FPS at the title city with it on.
- `SPU Block Size` Safe, Mega: no difference (Giga did not finish compiling in ten minutes). `Max SPURS Threads` below
  6 costs frame rate (3: 66.7 FPS at the title city, 2: 53.1). SPU loop detection, Preferred SPU Threads and the
  RPCS3 scheduler: within 2%.
- `Multithreaded RSX: true` keeps the offload thread spinning on
  `sched_yield` at one full core for no FPS (title 75.3 against 73.6, crater 61.2 against 64.4, 2 W more under the
  performance profile). Recommended off.
