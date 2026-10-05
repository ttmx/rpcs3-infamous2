# RPCS3: inFamous 2 optimizations

A fork of [RPCS3](https://github.com/RPCS3/rpcs3) (upstream commit `0ca1c58`, version 0.0.43) with changes that make
inFamous 2 (BCES01143, v1.04) lighter to run on the Vulkan renderer. Upstream's own README is in
[README.upstream.md](README.upstream.md).

Everything was measured on one machine: a Ryzen 7 8845HS laptop with its integrated Radeon 780M (RADV), Linux, game
at 1280x720 with no quality settings lowered. On that machine, with the GPU left on its default governor:

| Scene | Before | Now |
|---|---|---|
| Burning dock (heavy lighting and effects) | 17.6 FPS | 60 FPS (the game's cap) at about 37 W |
| City, slow street view (about 10,000 draws per frame) | 44 FPS | 59-61 FPS |
| Swamp pier (mission start) | 46.8 FPS | 60 FPS (cap) |

These are software changes. The two host tuning steps that were also tried (a GPU clock floor and a power profile)
are kept apart in [`infamous2/hardware/`](infamous2/hardware/) and are off unless you ask for them.

This is experimental work on one game, tested in three areas of it, on one GPU driver. Every change is off unless its
switch is set, and the tree still contains the diagnostics and the rejected experiments of the whole project.
It is not affiliated with or endorsed by the RPCS3 team.

## Using it

Build as upstream describes in [BUILDING.md](BUILDING.md), then:

```sh
infamous2/launch.py /path/to/inFamous2          # the disc folder that contains PS3_GAME
```

or set `INFAMOUS2_GAME` and `RPCS3_BIN` once and run `infamous2/launch.py`. The launcher finds `build/bin/rpcs3` of this
checkout by itself, uses your normal RPCS3 profile (firmware, saves, controller), a cache of its own
(`~/.cache/rpcs3-infamous2`), and the configuration in [`infamous2/play-config.yml`](infamous2/play-config.yml).
`infamous2/launch.py --help` lists the options; `--set NAME=VALUE` changes one switch, for example
`--set RPCS3_NATIVE_LIGHTING=0` to get the game's own lighting back.

Requirements and limits:
- Linux, Vulkan. Nothing was tried on Windows, macOS, the OpenGL renderer, or Nvidia and Intel drivers.
- The exact game version above. The GPU lighting and occlusion passes recognise the game's SPU jobs by their bytes
  and fall back to the original SPU code when they do not match; fixed guest addresses are used as well.
- Keeping static geometry on the GPU needs Linux 6.7 or newer (write tracking through `userfaultfd` and
  `PAGEMAP_SCAN`); on older kernels that one change does nothing.
- The streaming copies and the native SPU kernel are used only on CPUs with AVX-512; elsewhere the stock paths run.
- `play-config.yml` is a complete RPCS3 configuration (LLVM recompilers, 1280x720, 6 SPURS threads, Strict Rendering
  Mode off). Review it against your own settings.

## What was changed

Each line is one change, with the switch that enables it in parentheses; the launcher sets all of them. Figures are
from the machine above and are not additive: they were measured at different stages and scenes.

### The game's SPU rendering jobs, moved to the GPU

inFamous 2 renders a G-buffer, copies it to main memory, computes ambient occlusion and tiled deferred lighting on
the SPUs, and uploads the results as textures. Emulated, that round trip was most of the frame in heavy scenes.

- **Ambient occlusion on the GPU** (`RPCS3_NATIVE_SSAO=5`): the game's five-stage occlusion job is replaced by five
  full-screen passes written from a reverse-engineered reference; dock 44 to 56 FPS.
- **Deferred lighting on the GPU** (`RPCS3_NATIVE_LIGHTING`, bits 1, 4 and 8): the tiled lighting job (point and spot
  lights, diffuse and specular) runs as compute passes and the SPU job skips its pixel work; about +15% at the dock.
- **No G-buffer readback** (`RPCS3_NATIVE_LIGHTING` bit 16): with both jobs on the GPU their image transfers are
  skipped, so the two 1280x720 images are no longer copied from the GPU to guest memory and back each frame; dock 73
  to 85 FPS uncapped.
- **Half-resolution depth from the GPU pass**: the depth texture the game samples for particle effects, which the
  occlusion job used to produce, now comes from the GPU pass.
- **Up to 256 lights per frame**: eight mask words per tile; frames with more lights, or with light records the
  passes do not know, go back to the SPU job.
- **Faster lighting passes**: five parallel compute passes with ordinary GPU arithmetic replace two that reproduced
  the SPU arithmetic bit for bit; about a third less GPU work and 2-4 W less at the 60 FPS cap, with a few pixels
  differing by a level or two.

### Draw submission

- **Periodic command submission** (`RPCS3_VK_PERIODIC_SUBMIT_US=1000`): recorded commands are submitted every
  millisecond instead of at the end of the frame, so the GPU works while the frame is still being recorded; dock 17.6
  to 27.7 FPS.
- **Submit during semaphore waits** (`RPCS3_VK_SEMAPHORE_PIPELINE_PREFETCH`): rendering that is already recorded is
  submitted while the game is still computing a semaphore value; +5.9%.
- **Static geometry kept on the GPU** (`RPCS3_VK_GEOMETRY_CACHE=1`): vertex and index data that does not change stays
  in persistent GPU buffers across frames, kept valid by kernel write tracking of the guest pages; city +11%.
- **Fast repeat draws** (`RPCS3_VK_FAST_DRAWS=6`): runs of draws that change only geometry and transform constants
  (the shadow-map passes most of all) are read straight from the command stream and drawn with the state already
  bound; city +10%.
- **Texture and polygon offset changes inside fast runs**: those commands no longer end a run when the shader variant
  stays the same; city 55.5 to 58.8 FPS at the cap.
- **Descriptor set reuse** (`RPCS3_VK_DESCRIPTOR_REUSE=1`): a descriptor set already written with the same textures,
  samplers and buffers is bound again instead of allocating and writing a new one; city +2%.
- **Pipeline state reuse** (`RPCS3_VK_PIPELINE_REUSE=1`): pipeline properties that a full decode found unchanged are
  marked clean for the following draws; +0.4%, inside measurement noise.
- **Same-frame vertex cache** (`live.ctl`, third value): repeated vertex blocks within a frame are found through a
  128-bit fingerprint, and the cache switches itself off while few draws repeat; about +2% at the pier.
- **Inline command fetch** (`RPCS3_FIFO_INLINE_CACHE`): command stream reads that hit the validated cache skip the
  out-of-line fetch; pier 55.9 to 56.7 FPS.
- **Last image view** (`RPCS3_EXPERIMENT_LAST_IMAGE_VIEW`): each image remembers the view it handed out last, which
  saves a hash lookup when the same texture is sampled again; pier 55.9 to 57.0 FPS.

### Transfers between guest memory and the GPU

- **Partial upload after a blit** (`RPCS3_EXPERIMENT_BLIT_COMPLEMENT`): when a blit overwrites most of a surface, only
  the bands outside it are uploaded from guest memory (80% less data in the measured case); pier 55.9 to 59.1 FPS.
- **Streaming loads** (`live.ctl`, second value): uploads of 64 KiB and more from guest memory use non-temporal
  copies; pier about 53 to 55.9 FPS.
- **Streaming readback copy** (`RPCS3_VK_READBACK_STREAM_COPY`): multi-megabyte readbacks are written to guest memory
  with non-temporal stores; +1.6%.
- **Fused readback conversion** (`RPCS3_VK_READBACK_OOP`): the byte swap and the copy of the two G-buffer readbacks
  are done in one GPU step; +0.9%.
- **Serialized foreign readbacks** (`RPCS3_VK_READBACK_ADMISSION`): threads that fault on memory the GPU still owns
  queue for the readback one at a time instead of crowding the renderer's flush queue; about +5%.
- **Texture hits during a readback** (`RPCS3_VK_READBACK_SHARED_HITS`, `RPCS3_VK_READBACK_COMPRESSED_HITS`): read-only
  texture cache lookups, plain and compressed, can proceed while another thread holds the cache waiting for a readback.

### SPU

- **InstCombine in the SPU recompiler** (`RPCS3_EXPERIMENT_SPU_INSTCOMBINE`): LLVM's instruction-combining pass is
  added to the SPU pipeline, with its own object cache suffix.
- **Native SPU kernel** (`RPCS3_SPU_NATIVE_RWV`): one hot kernel of the game is replaced by a hand-written AVX-512
  version, matched by its exact bytes.
- **Savestates**: creating one retries the SPU thread lock 60 times instead of 15. Savestates still crash with
  InstCombine on; `--config infamous2/bench/cfg-nostrict-compat.yml --set RPCS3_EXPERIMENT_SPU_INSTCOMBINE=0 --set
  RPCS3_SPU_NATIVE_RWV=0` makes them work, and states made that way load under the normal settings.

### Configuration

- **Strict Rendering Mode off**: pier 49.4 to 52.8 FPS; `infamous2/launch.py --strict` turns it back on.

### Host tuning, kept separate

- **GPU clock floor** ([`infamous2/hardware/`](infamous2/hardware/)): the amdgpu governor left the 780M at a low clock
  in heavy scenes, and a 1600 MHz floor was the best FPS per watt before the GPU passes existed. With the current
  build the dock holds 60 FPS without it, so it is not part of the default launcher.
- **Performance power profile**: held through `powerprofilesctl` for the session, same folder.

Also in the tree but off: a cache of material image and sampler bindings (`RPCS3_VK_MATERIAL_BINDINGS`), which showed
no reliable gain, and the experiments the log records as rejected (ordered texture index, vertex copy offload,
zero-copy guest memory on amdgpu, source prefetching, and others).

## What else is here

- [`infamous2/docs/INVESTIGATION.md`](infamous2/docs/INVESTIGATION.md): the full log, with how each number was
  measured, what was checked for correctness, what was not, and what did not work.
- [`infamous2/bench/`](infamous2/bench/): the scripts that boot a savestate, switch changes on and off live and report
  FPS, power and clocks.
- [`infamous2/reference/`](infamous2/reference/): the offline tools behind the two GPU passes: an SPU disassembler and
  interpreter, NumPy references for both jobs, and the validators that compare the shaders against them.

No game data is included: no executable, savestates, saves, captured frames or disassembly of the game's code.

## Known gaps

- Tested at the swamp pier, the burning dock and one part of the city, in sessions of minutes, not hours.
- Scenes with many lights were checked against the reference offline (64 lights); in the emulator only what those
  three areas contain.
- In frames with 20-60 lightning lights, 10-200 pixels of 921,600 differ visibly from the game's own SPU output, and
  the GPU result matches the offline SPU interpreter on those pixels. Unexplained.
- The geometry cache drew stale bytes for 3 of about 85 million reuses in one battery-powered session; it did not
  recur and the cause is not established.
- `--no-gui` sessions do not always exit on SIGTERM.

## License

GPL-2.0, as RPCS3 (see [LICENSE](LICENSE)).
