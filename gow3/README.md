# God of War III (BCES00510 v1.03): the inFamous and Ratchet work applied, and what this game needed

2026-10-07 and 08. The source changes are in this fork (branch `infamous2-optimizations`). Test tooling in this
folder: `tools/gw.py` (same commands as the inFamous harness), `tools/fight.sh` (the route to gameplay, see below),
`geom/` (offline replay of the geometry job). The scripts carry the paths of the machine they were written on; the
captures, savestates and local store dumps they use are not published.

The game is a decrypted disc image, `~/Games/PS3/GodOfWar3/GodOfWar3.iso` (43 GB), booted as it is.

All figures: Ryzen 7 8845HS / Radeon 780M on AC, power profile "balanced" (30 W package limit), 1280x720.

## Measuring this game

- **No savestates in gameplay.** RPCS3 refuses ("HLE VDEC context(s) exist"). The `menu` state is the main menu;
  `tools/fight.sh` boots it, chooses Load and resumes the autosave at the first fight on Gaia (about 15 skeleton
  soldiers), holding block so that Kratos survives about 75 seconds.
- **The fight is not repeatable.** Enemy positions and the camera differ with every load, and the frame rate with
  them: the same build gives 35 to 46 FPS. Single runs say nothing under 15%; the figures below are means over 6 or
  7 loads, interleaved with the arm they are compared with, 25 seconds each at the 60 FPS cap.
- **The menu is repeatable** (the same view of Kratos every time) and is limited by the same thing as light
  gameplay and cutscenes: how fast the SPU jobs are handed out.
- **Speed follows frame rate.** With the frame limit off the opening cutscene plays at twice its speed at 200 FPS.
  Figures above 60 are capacity, not something to play at; the frame limit has to stay on.

## Result

| scene, 60 FPS cap | every fork switch off | fork before this night | fork now |
|---|---|---|---|
| main menu | 33.7 FPS | 42 to 46 FPS | 60.0 FPS, render thread at 0.54 of a core |
| opening cutscene (in engine), 30 s | | 44.9 FPS (without the two main changes below) | 60.0 FPS at 21 W, render thread at 0.41 of a core |
| first fight on Gaia | 29 FPS (one load) | 32 to 35 FPS (single loads) | 41 to 42 FPS after the first night (means of 6 or 7 loads); about 44 after the second round; 50.6 after the third (host kernels, see below); 56.9 after the fourth (ten kernels, non-temporal vertex copies); the fifth (three more kernels) takes 0.7 to 1.5 ms off the main thread's waits, between 0 and 5 FPS depending on the stretch; all at 30 W |
| first fight, `launch.py --fast` (relaxed SPU floats) | | | 44.8 FPS (mean of 6 loads, first night; not measured again) |
| first fight, performance power profile (54 W) | | | 49 FPS before the third round; 54 to 60 after it |

The fight was limited by the game's main PPU thread and the SPU jobs it waits for (second round, below); after the
host kernels it is the render thread in the slow stretches (fourth round).

## What the game does

- **Anti-aliasing on the SPUs, on the finished picture.** Every frame the game blits its 1280x720 picture to main
  memory (`0x30a80000`, a 1024 pixel wide piece and the remaining 256, the same split inFamous 2 uses), a SPURS
  task on five SPU threads reads it and writes it back row by row (12 MB read, 15 MB written per frame), and the result is
  uploaded as a texture for the final image. In RPCS3 that is two readbacks and two uploads of 3.7 MB per frame;
  the copies took a quarter of the render thread in the 2D opening sequence.
- **Geometry on the SPUs.** Another job reads 26 MB and writes 19 MB per frame in the fight: every vertex and index
  buffer is produced again each frame. Nothing is static (7 of 2,033 index requests per frame).
- **Occlusion reports read within the frame.** With `Relaxed ZCULL Sync` on the game stops at one frame per second
  (see below).
- **A label after every draw.** 2,300 texture read semaphore releases per frame.
- About 3,000 draws per frame in the fight, 500 in the menu.

## Table

| optimization | this game | note |
|---|---|---|
| inFamous 2 GPU occlusion and lighting passes | not applicable | Other engine. The same idea applies to the anti-aliasing job, below. |
| No readback and no blit of the picture | done, new pass | See "GPU anti-aliasing". |
| Periodic command submission | applies directly, on | Menu 44 to 46 FPS without it against 46 to 48 with it (uncapped, before the other changes). |
| Static geometry kept on the GPU | does not apply, now switches itself off | Nothing is static, and the cache cost its lookups (8% of the render thread) and 2.2 ms of page scans per frame. It is suspended while under 5% of the requests are answered from it. |
| Fast repeat draws | applied after changes | 4 draws per frame as it was. Now about 1,000 of 3,000: runs pass over the label after each draw (wide scope, now the default for every game), over the vertex program start the game sets again before every draw, and finished occlusion reports are written inside runs. 1,600 draws per frame cannot take the path because they sample render targets. No frame rate change that the fight can show. |
| Front face and cull mode as dynamic state, descriptor and pipeline reuse | apply directly, on | Not measured separately here. |
| Accurate xfloat without doubles | not used | The configuration is Approximate. |
| Unlocked frame rate | not applicable | Speed follows frame rate. |

## New work

| change | state | note |
|---|---|---|
| GPU anti-aliasing (`RPCS3_NATIVE_AA`, default 7, `VKNativeAA.cpp`) | done, SPU side corrected in the second round | The SPU task is recognised by the first 16 bytes of its program and the two functions its main loop calls for a frame (`0x65d8`, `0x8168`) are made to return at once, every time the program is loaded; the two blits are left out; when the second one would have come, one fragment pass smooths the render target, and the game gets that image when it binds the texture at the copy's address. The pass follows each luminance edge to both of its ends (32 steps of one pixel each way) and shifts the pixel towards its neighbour across the edge by the coverage a straight line would give; pixels away from edges are copied. It is not the game's algorithm; enlarged crops of the menu next to the SPU result show the same smooth edges (`images/aa-spu-left-gpu-right.png`). Opening 2D sequence: the render thread no longer copies anything. Fight, single loads: 32 to 42 FPS. Works at the render target's size, so it follows the resolution scale: at 200% the pass runs at 2560x1440 and the menu's edges are clean (55.9 FPS there, with the GPU at its top clock). |
| Back-off after a failed SPU conditional store (`RPCS3_SPU_PUTLLC_BACKOFF`, default 100 cycles per failure in a row, 16 at most) | done, the largest gain | The game's six SPURS kernels update one 128-byte line whenever a job is handed out. Emulated, the time between GETLLAR and PUTLLC is long, and six threads keep invalidating each other: the jobs take longer to hand out than to run. Menu at the cap 47.8 to 60.0 FPS (render thread 0.93 to 0.54 of a core, SPU threads 3.1 to 2.6 cores); uncapped 50 to 110. Fight 38.3 to 41.3 FPS (6 pairs). Found through `Preferred SPU Threads: 5`, which gives the same (37.8 to 41.6 FPS over 7 pairs) by accident: its limiter never waits in this game, it only makes every transfer a little slower. Values of 50 to 1,000 cycles are equal; 5,000 and more lose again. No difference in Ratchet & Clank (67.7 against 68.1 FPS), inFamous (77.5 against 76.7; 74.2 against 75.1) or inFamous 2. |
| Reports delivered while the render thread waits for the game (`rsx::thread::deliver_reports_while_waiting`) | done | With `Relaxed ZCULL Sync` on, the game waited for an occlusion report, the render thread waited in a semaphore for the game, and nothing delivered the report until the one second timeout: 1 FPS. Now pending reports are delivered in that wait and the relaxed setting runs. It is still not used for this game: 45.9 against 43.4 FPS in the menu before the other changes, and Festival of Blood showed what late reports can do to the picture. |
| Geometry cache suspends itself | done | See the table. |
| Fast draws: same-value vertex program start and output mask, reports written inside runs | done | See the table. |
| Low-power wait while the render thread has no commands (MWAITX on PUT instead of yielding 17,800 times per frame) | tried, not kept | No frame rate difference in the fight (36.3 against 36.3 FPS). |

Checks: inFamous 2 city with the wide fast-draw scope, mode 4: 738,000 draws, no mismatch. Festival of Blood hall
73.5 FPS, inFamous 2 dock 84.2 against 84.0, city 64.1 against 64.7 with the new defaults against the old ones.

## Configuration findings

- `Relaxed ZCULL Sync`: off (the RPCS3 default). On, the game ran at 1 FPS before the fix above.
- `Preferred SPU Threads`: no longer needed with the back-off; 5 with the back-off gives the same as either alone.
- `SPU Block Size: Mega`: 42.3 against 40.9 FPS in the fight (6 pairs). In `play-config.yml`. `Giga`: 33.6 FPS.
- `SPU XFloat Accuracy: Relaxed`: 44.8 against 40.8 FPS in the fight (6 pairs, both with Mega). The recompiler's
  code for the game's hottest SPU loop (`0x10aac`, 770 instructions, 52 of them float multiplies and multiply-adds)
  has 172 `vfixupimmps`, 76 `vcmpneqps` and 24 `vrangeps` in Approximate mode: fix-ups that make a zero operand win
  over an infinity or NaN pattern, as on the console. Relaxed leaves them out. Nothing looked wrong in the fight,
  but a calculation that does run into such a value comes out differently, so it is `play-config-fast.yml`
  (`launch.py --fast`) and not the default.
- `Accurate SPU Reservations: false`: the emulator exits when the menu state is loaded.
- `Thread Scheduler Mode: RPCS3 Alternative Scheduler`: 38.4 against 43.7 FPS in the fight (6 pairs). Worse, although
  with the operating system's placement two of the seven busy threads share a physical core in half of the samples.
- The main thread, the render thread and the six SPU threads each on a core of their own with every other emulator
  thread kept off the main and render threads' sibling hardware threads (`tools/pin2.sh`, switched inside one
  session, sixth round): 53.4 against 53.5 FPS.
- Each of the eight busy threads pinned to a physical core of its own (`tools/pin.sh`): 41.0 against 40.8 FPS in the
  fight (5 pairs). No difference.
- `Thread Scheduler Mode: RPCS3 Scheduler`: 36 FPS in the menu against 51. `Max SPURS Threads` 4 and 3: 46 and 42.
- Vblank rate and clock scale do not change the menu's frame rate.
- Performance power profile: 45.4 FPS at 54 W against 42.0 at 30 W in the fight (4 pairs). 8% for 24 W: not used.

## Second round: what limits the fight

Tools added for this: `RPCS3_SPU_SAMPLER=<file>` (the emulator samples every SPU thread 2,000 times a second: program
in the local store, pc, waiting or not; PPU threads go to `<file>.ppu`), `tools/samp.py` and `tools/ppusamp.py` to read
two snapshots, `RPCS3_SPU_XFER_TRACE=<file>` (every transfer while `<file>.go` exists), `tools/findprog.py` (program
images in the executable by the sampler's hash), `tools/jitclass.py` and `tools/jithot.py` (host instructions of the
recompiled code by kind and by place), `tools/sched.py` (per thread CPU time and context switches), and
`tools/frametimes.py` (distribution of frame intervals). `RPCS3_SPU_PUTLLC_STATS=1` counts line stores by path.

Findings, in the order they changed the picture:

- **The anti-aliasing task was never skipped.** The first night's patch sat at the entry of the task's main function.
  The task is created once and then resumed every frame in the middle of that function (its program and stack are
  loaded again at each resume), so the patched instruction never ran: the five threads kept reading and writing a
  picture that nobody used, 8.7% of all SPU time in the fight. Now the two work functions are patched instead and the
  task is gone from the profile. The menu's render thread went from 0.54 to 0.38 of a core at 60 FPS.
- **Every line store stopped every PPU thread.** With `Accurate SPU Reservations` (the default) a successful PUTLLC
  that changes data, and every PUTLLUC, takes `vm::writer_lock`, which flags each PPU thread and waits until all of
  them have stopped. The game does about 300,000 conditional and 80,000 unconditional line stores a second.
  `RPCS3_SPU_PUTLLC_PIECEWISE=1` (now a default) keeps the reservation lock, compares the line and writes each changed
  16-byte piece with a compare-exchange; if a plain store got in between, what was written is taken back under the full
  lock (never seen in 30 million stores; 400 times the line had changed before the first piece). Inside one fight
  session, switched every few seconds (live control 15): 45.0 FPS without, 47.5 with (4 pairs); the main thread uses
  less CPU for it. Turning the setting off instead is not an option: it also swaps several SPURS functions for HLE
  ones and the emulator exits.
- **Where the time is.** SPU threads: 86% in job programs, 13% in the job manager. One program, the geometry job
  (`c5fcd65c`, about 330 jobs a frame, each a few kilobytes in and out, about 200 microseconds), has 46% of all SPU
  time, spread over a dozen functions; its skinning loop at `0x10aac` has a fifth of that. The rest: `72cc647b` and
  `9d227620` (11% each, software-cached data structures), `9b548a6b` (6%, a noise function at `0x4980`), smaller ones.
  The recompiled code runs at 2.3 instructions per cycle with next to no instruction-TLB misses; by kind, a sixth of
  its time is guest registers written back to the thread context around calls and a seventh is spills.
- **The main PPU thread is the other half.** It is busy 86% of the time: 55 to 60% in game code spread over 950
  functions (97.8% of its host time is recompiled code), 25% spinning in `0x2eafb0` until every worker has passed a
  job index, 6% in a second spin at `0x22fb40`, and 14% in three polling sleeps. With three, four and five SPURS
  threads the fight runs at 37, 42 and 44 FPS: more SPU capacity alone buys little.
- **It is not frame pacing.** Frame intervals are spread around one value (16 to 22 ms in a light stretch), not split
  between one and two refresh periods.
- **Variation.** Inside one session the frame rate swings between 40 and 49 FPS with a period of about 20 seconds,
  and the first 20 seconds after a start run on a higher power allowance (40 W). Whole loads compared with whole loads
  need many of them; switching a feature inside one session is the better instrument where a live control exists.

Result of the round, means of 25 second windows at the 30 W limit: 43.1 FPS over 7 loads and 43.6 over 70 seconds of
one session with the new defaults, against 41 to 42 before. With the anti-aliasing task working and its blits done
(`RPCS3_NATIVE_AA=5`, full lock) the same batch gave 37.4.

Tried and not kept: `SPU Block Size: Giga` (33.6 FPS), a savestate inside the fight (with the HLE video decoder the
main thread never runs again after loading; with `libvdec.sprx` from the firmware the state loads into a dead command
queue), `SPU XFloat Accuracy: Relaxed` as the default (it uses the host's reciprocal estimates, which return infinity
for zero; a vertex that is normalised from a zero vector would come out as NaN, so it stays the `--fast` option).

What 60 FPS in this fight would take: the main thread's own 12 to 13 ms a frame leaves 4 ms for everything it waits
for, and it waits 9 ms now. The waits are the geometry jobs; that job would have to run two to three times faster,
which the recompiler will not give. It is a candidate for a native implementation (decode, skin, cull, pack, and the
draw commands it writes into the command buffer), which means working out its formats from about 15,000 SPU
instructions. The earlier lifted kernels of inFamous 2 show that translating it instruction by instruction gains
nothing.

## Third round: host kernels for the geometry job

How it is done (all tools in `geom/`, captures in `capture/`):

- `RPCS3_SPU_JOB_CAPTURE=<dir>` (with `_EVERY=<n>`, `_LS=1`) writes jobs of one SPU thread: registers and local store at
  the job manager's fetch of the next job's inputs (an interrupt right after the running job's first call, `0x4bd8`),
  then every GET, PUT and atomic command. With it (or the transfer trace) set, the recompiler sends every transfer
  through the C++ functions; otherwise small constant transfers are inlined and no diagnostic sees them.
- `geom/gj.py` replays a captured job in the Python SPU interpreter (extended with the instructions and the SPU float
  range the job needs: it multiplies all-ones masks as floats and relies on saturation) and compares its PUTs with
  the capture. All 396 jobs of the current capture replay with the same structure; bytes differ in the last float bits.
- `geom/prof.py`, `geom/loops.py`: instructions by function and by loop. `geom/live.py`: registers a region needs and
  leaves. `geom/kern.py`: prototypes of host kernels in numpy, run inside the replay with every other register the
  region writes poisoned; `geom/kdiff.py` compares one call with the reference.
- In the emulator: `rpcs3/Emu/Cell/SPUNativeGeometry.hpp` (kernels and their table), hooked in the recompiler's
  instruction emission (`RPCS3_SPU_NATIVE_GEOMETRY=1`; live control 16 = 1 makes the kernels decline, for comparisons
  inside one session with `gw.py ab`).

Facts: about 124,000 geometry jobs a second (one per draw), 20,000 to 50,000 SPU instructions each. By loop, share of
the job's instructions: weighted position skinning `0x10e20..0x10ff4` 26%, `0xfec0..0x10734` 8%, `0x9958..0xa46c` 7%,
`0x7a88..0x809c` 6%, `0xc168..0xc568` 5%, `0xdedc..0xe008` 5%, `0x6c10..0x6e2c` 5%, `0x9040..0x94d4` 5%,
`0x5d70..0x5de4` 5%, `0x5de8..0x66ec` 5%, second skinning loop `0x11314..0x1144c` 4%, `0x11820..0x11964` 3%,
triangle culling `0xfb58..0xfd74` 2%.

| kernel | region | share of the job's instructions | note |
|---|---|---|---|
| weighted position skinning | `0x10d74..0x10ff8` | 26% | up to four bones per vertex; also writes each vertex's blended matrix |
| further skinned streams | `0x111bc..0x1160c` | 4.5% | normals, tangents by the blended matrices |
| matrix transform | function `0x11740` | 3.2% | vec4 array by one 4x4 matrix |
| normalise | function `0xdd50` | 5.5% | exact instead of the SPU's 12-bit estimate; a zero vector stays zero |
| binormals | `0xc168..0xc520` | 5.3% | normalize(cross(B, A) * B.w), in the SPU code's operation order |
| position packing | `0x5cb0..0x5de8` | 4.5% | output format 0 of the vertex packer `0x5b38`: xyz as 12 bytes per vertex |
| noise (another program, the task `9b548a6b`) | function `0x4980` | about 6% of all SPU time | Perlin's improved noise at four points, with the game's gradient table (entries 12 and 14 differ from Perlin's); within 1e-6 of the SPU code on 200 captured calls (`RPCS3_SPU_REGION_CAPTURE`, `geom/region.py`) |

The six geometry kernels in the replay: 48% of the job's instructions replaced; of 807,000 output words in 396 jobs
458 differ beyond the last bits, all from padding vertices (the loops run past the vertex count in steps of 2, 4 or
8, over whatever follows the data).

In the emulator (`RPCS3_SPU_NATIVE_GEOMETRY`, now a default; `tools/abgeom.sh` switches the kernels off and on inside
one fight session, 3 seconds each): with the first five, 43.8 against 49.7 FPS (9 pairs) and 43.1 against 51.5 (5
pairs). The kernels' own cost is 8% of all SPU time (position skinning 3.5%, noise 3%, the rest 1.5% together); the
position kernel leaves out influences with weight zero and loads two matrix rows at once, which halved it.

The fight with everything, 5 second windows over a minute:

| power | FPS |
|---|---|
| 30 W (balanced profile, sustained) | 43 to 55, mean 50.6 (43.6 before this round, 41 to 42 before the second) |
| 40 W falling to 33 W (balanced profile, first minute after the machine has rested) | 50 to 60, mean 56.6; five windows of twelve at the cap |
| 54 W (performance profile) | 59.9, 59.5, 54.3 (the fight ended after these) |

The main PPU thread is now busy 0.8 to 1.0 of a core; giving it a physical core of its own changes nothing (48.3
against 48.8). Other games with the new defaults: inFamous 2 dock 60.0 and city 59.4, Ratchet & Clank 59.9, inFamous
60.0, Festival of Blood 60.0, this game's menu 60.0 FPS at the cap.

Three more kernels after that (the vertex packer's two common formats and the point lights):

| kernel | region | share of the job's instructions | note |
|---|---|---|---|
| packing, 36 bytes a vertex | `0x5ea8..0x5de8` | 4.2% | packer format 7: position, four 16-bit values, one 11:11:10 word, a second position |
| packing, 54 bytes a vertex | `0x66f0..0x5de8` | 4.5% | packer format 4: the same with three more vectors as half floats |
| point lights | function `0x8fd8` | 4.8% | per vertex, the sum over up to 64 lights of colour times a smoothstep of the distance |

The built emulator checked end to end (`geom/verify.py`): jobs captured with the kernels active against the reference
replay of the same inputs. Of 792,628 output words 443 differ beyond the last float bits, all padding vertices or one
step of the 10-bit tangent component; in one job one triangle was culled differently.

Not done, with what they are (shares of the geometry program's time in the sampler, which is half of all SPU time):

- `0xfec0` (5%): unpacks the bone matrices from a packed form (about 10 bytes a bone) to 64 bytes each. 542
  instructions with several table-driven formats.
- `0x95b4`, loop `0x9958..0xa46c` (5%): sets up the lighting streams from the light tables.
- `0xf428`/`0xf898` (4%): transform to the screen and triangle culling.
- Packer formats 3 (46 bytes) and 5 (24 bytes): rare.

## Fourth round: the render thread

With the kernels in, the slow stretches of the fight (about 43 FPS, frames of 20 to 24 ms) are set by the render
thread. The sampler now also writes the main PPU thread's guest call chain while it waits (`<file>.stack`): two thirds
of its waiting is in `0x26d398`, which polls the reference counter the RSX updates, called from the frame function;
and the geometry jobs wait for room in their output ring (`0xd7e8`), which the same counter frees. The game's own work
is about 12 ms a frame.

The render thread in the fight (`perf record --call-graph dwarf` on `rsx::thread`): about 2,300 to 3,300 draws a frame
(one per geometry job), 66% of its time in `VKGSRender::end()`. The largest single item was the copy of vertex data
into the upload buffer: 110,000 to 190,000 copies a second, 6 KB on average, 650 to 1,250 MB a second, about 20 MB a
frame. Almost all of it is new each frame (the jobs write every draw's vertices again): of the bytes, 3% repeat a copy
of the same frame exactly, 15% lie inside an earlier one and 15% overlap one, so the vertex caches have little to find.

- **Non-temporal vertex copies** (`RPCS3_RSX_STREAM_VERTEX_COPY=1`, now a default; `rsx::dma_manager::copy`, 1 KB and
  more, AVX-512 hosts): the render thread never reads that data back. The copy went from 19.6% to 12.8% of the thread.
  Inside one session, in the order A B B A B A A B (`tools/abx.sh`; live control 3, bit 16 switches it off): 51.3
  against 55.3 FPS (24 windows of 4 s each), 51.4 against 55.2 (12 each). A threshold of 128 or 256 bytes instead of
  1,024 (live control 17) is inside the noise.
- The fight at the sustained 30 W after this: 54.5 to 59.0 FPS in 5 second windows, mean 56.9 (8 windows; 51.2 before
  the round).

What is left on that thread: the vertex copy 13%, the rest of the vertex upload 8%, reading and decoding the command
stream 15%, waiting for occlusion query results at the game's semaphore release 9% (the GPU has to catch up there; it
runs 75% busy at its lowest clock), the Vulkan driver 10%, texture and program binding 13%.

Tried and not kept in this round: prefetching the source of the vertex copies (no change), handing the copies to the
offload thread (21 FPS), `Relaxed ZCULL Sync` (no clear change, and it delivers reports late), SPU PUT transfers of
1 KB and more written with non-temporal stores (53.8 against 53.4), a reuse of ranges uploaded earlier in the frame
(not built: at most the 15% that lie inside an earlier copy).

**Where a frame of the fight goes now** (`tools/ownwork.sh`: the main PPU thread from the sampler, 24 seconds,
four sessions at 30 W, 54 to 57 FPS): of 17.6 to 18.5 ms a frame the thread works 11.7 to 12.6 ms on its own (game
code spread over 600 functions), spins 2.5 to 4.2 ms for jobs (`0x2eafb0`, and a loop on a busy flag at `0x22fb40`)
and sleeps 1.9 to 3.3 ms waiting for the render thread. A steady 60 needs the two waits together under 4.5 ms; they
are 5 to 7.

More that was tried after that and not kept:

- **A worker thread for the vertex copies** (one producer, a lock-free ring, a spinning worker, drained before
  submits, label and reference writes): 53.0 FPS without against 51.3 with (43 windows of 2.5 s each), and with a
  worker that goes to sleep after 300 idle spins 52.8 against 52.6 (44 and 43). Taking the copy off the render thread
  buys nothing. Removed.
- **Vertex data read by the GPU from guest memory** (no copy): not built. The worker result bounds what it can give
  to the energy of the copy itself, about a tenth of one core, and the game reuses its vertex ring inside a frame,
  so the host GPU would have to be drained at every reference write.
- **`PPU Vector NaN Handling: false`**: the main thread's own work is 11.7 and 12.6 ms without it, 11.9 and 12.4 with.
  No difference; left at the default.
- **Performance power profile** (54 W, 79 C): 52.1 to 59.9 FPS, mean 57.7 over a minute, against 56.9 at 30 W. Power
  is not what holds the slow stretches back any more.

What would move it further is more of the same on both sides of the main thread's waits: the remaining SPU loops
(bone unpacking, lighting set-up, culling; the two other large programs `72cc647b` and `9d227620`, 14% of SPU time
each) and the render thread's per-draw cost. None of them is worth more than about 1 FPS alone.

Measuring: Kratos dies after 20 to 60 seconds of holding block. `gw.py ab` now restarts from the checkpoint when the
SPU load is gone and measures that window again; `gw.py revive` does the same before a profile. Pairs of windows in a
fixed A B order can lock to the fight's 20 second cycle (one such run showed the streaming copy 5 FPS slower); use
`tools/abx.sh`. `tools/ppudis.py` disassembles the executable's PPU code by address and lists callers.

## Fifth round: what the main thread waits for, job by job

New diagnostics (all part of `RPCS3_SPU_SAMPLER=<file>`): `<file>.stack` has the main PPU thread's guest call chain
whenever it sleeps or spins at one of its two job waits; `<file>.blocked` has what every SPU thread was running at
those moments, by wait site (`tools/blocked.py`); `<file>.rsx` has the state of the render thread's command queue
(running, or spinning on the game's jump to self). `tools/abwait.sh` switches a live control inside one fight session
and prints, per setting, the main thread's milliseconds per frame at each wait site and the render thread's working
time per frame. `RPCS3_SPU_KERNEL_CHECK=<dir>,<pc>` writes the state before and after each run of a built kernel;
`geom/kcheck.py` compares it with the SPU code run in the interpreter.

The main thread's waits, milliseconds per frame (they vary by a factor of two between sessions):

| where | what it is | ms | SPU threads meanwhile |
|---|---|---|---|
| `0x7212d8` (in a firmware module, from `0x2315f8`) | sleeps until up to six handles are done | 1.5 to 3.0 | the tasks `9d227620`, `fb56025d` and the noise task, next to geometry jobs |
| `0x2eafb0` (from `0x2307c8` and `0x236a64`) | spins until every worker has passed a job index | 0.9 to 2.0 | half geometry jobs, a quarter asleep, `a6df6c09`; a worker inside a long task holds it up |
| `0x26d398` | polls the RSX reference with a 30 microsecond sleep | 1.0 to 2.0 | |
| `0x22fb40` | spins on a busy flag after submitting a job | 0.9 to 1.1 | one worker in `b245f318`, the rest geometry |
| `0x28da90` | polls a flag with a 30 microsecond sleep | 0.2 to 0.8 | |

What was done with it:

- **`b245f318` is a bounding sphere job** that the main thread waits for directly. Its two hot functions are host
  kernels now: `0x59a0` (groups of spheres moved by their bone matrices, one sphere around each group) and `0x4ef0`
  (a bit for every overlapping pair of two sphere lists). `geom/b245.py` has the prototypes. The built pair test is
  identical to the SPU code in 200 runs; the sphere kernel differs by the SPU code's square root estimate (largest
  difference 0.2% of a 0.017 radius). The wait at `0x22fb40` goes from 1.12 to 0.90 ms a frame.
- **The noise function is the lever on the job barrier.** With `noise4` skipped as an experiment the fight ran at
  57.4 against 51.6 FPS in one session and the spin at `0x2eafb0` fell from 1.45 to 0.40 ms: the noise task runs
  for milliseconds without yielding, and the barrier waits for the worker that has it. The kernel was rewritten: the
  eight corner gradients of all four points come from three byte shuffles (the components are -1, 0 or 1) instead of
  96 scalar loads, fade and floor are done for the four points at once. It costs 1.1% of SPU time instead of 3.4%;
  results agree with the SPU code to 3e-7. Old against new inside one session: the spin at `0x2eafb0` 1.67 to 0.90
  ms, all waits 5.90 to 5.16 ms. What is left of the task is its loop at `0x61e4` (430 instructions and four calls
  of the function per iteration), 1.5% of SPU time.
- All three together (live control 16 = 6 gives the earlier state): waits 7.18 to 5.71 ms a frame and 51.4 against
  56.5 FPS in 11 windows of 8 seconds each; 54.5 against 54.7 FPS in 40 windows of 2.5 seconds each. The waits fall
  every time; the frame rate follows only where the render thread is not the limit.
- **In the slowest stretches the render thread is the limit**: its queue is running 96 to 98% of the time in windows
  of 43 to 47 FPS, 80 to 90% in windows above 55. It works 16.7 to 18.3 ms a frame.
- **Short sleeps spun through** (`RPCS3_PPU_USLEEP_SPIN=<microseconds>`, off by default; live control 18): with 100
  the reference poll goes from 1.00 to 0.54 ms a frame and the flag poll from 0.52 to 0.75; 54.4 against 55.1 FPS
  (40 windows each). Too little to make it a default for every game.

Tried and not kept:

- The vertex copy worker again, now with the per-draw label (`NV4097_TEXTURE_READ_SEMAPHORE_RELEASE`, which the
  render thread had been waiting at for the worker, 10% of its time) written by the worker after the copy: 50.8
  against 54.8 FPS, the main thread's waits up from 5.3 to 7.0 ms. The eight physical cores are all busy (five to six
  SPU threads, the main thread, the render thread); a ninth busy thread shares a core with one of them and costs
  more than it saves. The same explains the offload thread's 21 FPS.
- SPU transfers: two million a second, 11% of SPU time in the transfer functions, most of it in the copy itself.
  Without `rep movsb` (vector loops for every size): 11.2% against 11.2%. With the list prefetch (live control 7) at
  512 and 4,096 bytes: 10.8% and 11.6% against 11.2%. It is the memory, not the copy routine.
- `SPU Reservation Busy Waiting Enabled`: spinning 3.0 and 2.0 ms against 3.8, sleeping 3.4 and 5.0 against 2.7,
  the same frame rate.

Not done: the rest of the noise task's loop; the four hot functions of `a6df6c09` (2% of SPU time, running while
the main thread spins at the barrier); `72cc647b` (6% in five places, tree walks rather than arithmetic).

## Sixth round: copies, the job chain, and what a frame really waits for

**Vertex data straight from the SPU thread to the GPU buffer** (`rpcs3/Emu/RSX/Common/spu_upload.h`,
`RPCS3_SPU_VERTEX_UPLOAD=1`, now a default; live control 19: 1 = draws do not use it, 2 = every range used is compared
with guest memory, 3 = off). The geometry job PUTs each draw's vertices to guest memory and the render thread copied
them again, cold, into its upload ring: 20 MB a frame. Now the SPU thread's PUT also appends the bytes (still in its
cache) to a 256 MB heap that the vertex shader reads, and publishes the guest range in a table; a draw whose blocks
are published binds the heap at their offsets (`fill_vertex_layout_state` takes one offset per interleaved block) and
copies nothing. Blocks that no job wrote (a static stream of the model next to the job's stream) are copied into the
heap by the render thread, so that the rest of the draw still needs no copy.

- The render thread's vertex copying falls from about 13.6 to 1.6 MB a frame; its working time per frame from 16.95
  to 15.28 ms and from 17.82 to 14.28 ms (two sessions, `tools/abwait.sh`), busy 90% to 77-83%.
- Checked: live control 19 = 2 compares every range used with guest memory at the draw. 19.8, 13.0 and 10.6 million
  ranges over fights with deaths and checkpoint restarts: none differed.
- What is tracked and what is not is in the header. Only the geometry job's PUTs are published (recognised by the
  code at the skinning loop): published for every program, Ratchet & Clank showed 5,379 differing ranges in 3.4
  million, and in games without such a job the path replaced working caches with a copy per draw (Ratchet 52 FPS
  instead of 60). Other games now never create the heap.
- Userptr zero-copy (the GPU reading guest memory) was measured in the inFamous 2 work: amdgpu validates every
  user page on each submission, a net loss. This is the copy-once alternative.

**Whole-function kernels return and are called without a chunk change** (`SPULLVMRecompiler.cpp`). A kernel that
replaces a function used to leave through the dispatcher; it now returns as the function's `BI $LR` would (stack
mirror), and a `BRSL` whose target is such a function calls the kernel at the call site after comparing the target's
first four instructions in the local store, never entering the function's chunk (whose entry loads the registers of
a body that does not run). The noise function is called about 30,000 times a frame. Dispatcher against stack-mirror
return in one session: 54.5 against 56.4 FPS (11 and 12 windows of 8 s), 51.0 against 52.5 (19 and 20 windows of
6 s, no sampler). The function's stub chunk (5% of the noise thread's time) is gone with the call-site form.

**What a frame waits for.** Three places counted as the main thread's own work are spin loops: `0x228ef0` (16 NOPs
around a test of the halfword at `0x59b500`, which the job `d8b7eb58` clears once a frame), `0x22fb40` (the same
block, offset 20) and the code after the submit at `0x230048`. With them the thread waits about 9 ms of a 19 ms
frame and works 10 ms. The waits are a chain of synchronous hand-offs to SPU jobs and tasks: `cellSpursJoinTask2`
for the frame's tasks (`0x7212d8`, 1.2 to 3.2 ms), the `d8b7eb58` flag (1 to 3 ms), the bounding sphere job (0.9),
the job barrier (0.5 to 1.5), the geometry and RSX polls (1 to 2). Their sum stays the same when one of them is
made faster: the time shows up at the next one. That is why single changes move the frame rate so little.

How the SPUs are used (sampler, `.threads` and `.spurs`): the game's three geometry queues (workloads 10 to 12,
priorities `05 05 05 00 00 00 00 03`, one active per frame) run on SPU 0 to 2 only, 86 to 87% of those SPUs' time,
with 10% for workload 7 (`72cc647b`). SPU 3 and 4 run the tasks (`9d227620`, `fb56025d`) and are idle 41% of the
time. A sixth SPU thread (a second SPURS instance) runs a serial chain: noise task, `a6df6c09`, `407ce898`,
`672748c6`, busy 64%. The main thread joins that chain's tasks and its barrier waits while `a6df6c09` runs.

**What the frame time follows** (`vk::live_ctl::probe_delay`, live controls 21 and 22: nanoseconds burnt at one
place, switched inside one session; 8 to 10 windows of 6 s each): no delay 55.8 FPS; 1,500 ns at each published PUT
of the geometry job (about 5 ms more per geometry worker and frame) 49.0; 1,000 ns at each draw's vertex upload on
the render thread (2.5 to 3.3 ms more a frame) 52.8; 100 ns at each call of the noise kernel (3 ms more on that
thread) 54.4. A millisecond more on each geometry worker costs about half a millisecond of frame time, a millisecond
on the render thread a third, the noise chain next to nothing now (with the noise function skipped altogether:
52.3 against 52.6 FPS).

**The geometry SPUs kept for geometry** (`RPCS3_SPURS_RESERVE=1`, now a default; live control 20: 1 = off, 2 = on).
Workload 7 (priorities `04 04 04 03 02 02`, `72cc647b` jobs) ranks one step above the geometry queues on SPU 0 to 2
and takes 10% of their time, while SPU 3 and 4, where it ranks higher still, idle 41% of the time. The emulator
rewrites its table to `06 06 06 03 02 02` the way `cellSpursSetPriorities` does (new table, update message to every
SPURS kernel), when a kernel reads the first line of the SPURS structure; switching it off restores the game's
table. Off against on in one session: 50.3 against 51.4 FPS and 52.4 against 54.4 (20 windows of 6 s per arm each
time), windows under 50 FPS 16 against 8. Only a workload exactly one step above a queue that is tied to SPU 0 to 2,
and with a higher priority on SPU 3 and 4, is touched: inFamous 2 measures the same with and without (59.2 and
59.3 against 59.4 and 59.5).

**No command-fetch lock for vertex PUTs.** With `RSX FIFO Fetch Accuracy: Atomic` (the default) every SPU PUT to
main memory takes the lock that keeps the render thread's command reads whole. The geometry job's PUTs of 1 KB and
more are vertex and index data (its command chunks are a third of a kilobyte), so they leave it out (live control
19 = 4 keeps it). `do_dma_transfer` on the three geometry workers and the render thread: 8.8 and 8.6% of their time
with the lock, 7.7 and 7.3% without.

**The publish path itself** was 40% of `do_dma_transfer` on the geometry workers after that: three stores with
full barriers into a cold record, a shared id counter, two shared statistics counters. With plain stores, ids taken
a thousand at a time and the counters only under `RPCS3_SPU_VERTEX_UPLOAD_STATS`, the function goes from 8.8 to 8.1%
of those threads' time (1 KB granules instead of 256 bytes: 7.8%, but three times as many ranges fall back to the
render thread's copy; kept at 256). Prefetching every element of a list GET before the first copy (the job reads its
bone matrices as about a hundred 64-byte elements) changes nothing: 19.6, 19.6 and 18.1% for the three transfer
functions without, 20.2, 18.9 and 18.3% with 128 and 4,096 bytes per element.

What is left on the geometry workers, by `perf` on the three threads: transfers 19%, the kernels 19%, and of the
recompiled code the bone decoder `0xfec0` 4.6% (a bit stream reader, 542 instructions per bone), the lighting driver
`0x9958` 3.5% (per light two per-vertex passes at `0x9c48` and `0x9f90`, about 215 instructions together, then the
point lights), transform and culling `0xf428`/`0xf918` 3.3%. By the delay probe each percent of those threads is
worth about 0.1 ms of frame time.

The test configuration `cfg-test.yml` has `SPU Block Size: Safe`; `play-config.yml` has `Mega`. Everything of this
round was run with both: Mega 51.9 and 57.8 FPS, Safe 52.0 and 55.4 (alternating sessions), and the compare mode of
the upload heap found no difference in 8 million ranges with Mega.

Tried and not kept:

- **Geometry on five SPUs.** The queues are created at `0x28dd24..0x28de3c` with three priority bytes and a count of
  3 (`li r18, 3`, `li r19, 3`). A game patch that sets five (cold boot; the tables are built at start-up) and two
  emulator-side variants (priorities through the SPURS update message, ready count 3 to 5) all stop the game as soon
  as an SPU other than 0 to 2 has run a geometry job: the workload's contention count stays up with no SPU in it.
  The job code has per-SPU state for three workers that I did not find.
- **Keeping SPU 0 to 2 for geometry, first form** (workloads 6 and 7 both lowered below the queues' priority there): the geometry poll
  went from 1.09 to 0.11 ms a frame, the `d8b7eb58` wait from 1 to 3 ms (that job then waits behind long tasks on
  SPU 3 and 4). 53.9 against 53.9 FPS.
- **Workload 6 ahead of geometry on SPU 0 to 2** (4 instead of the tie at 5, for the `d8b7eb58` job: 0.08 ms of work
  a frame that the main thread waits 1 to 3 ms for, because SPU 3 and 4 take it only when a long `9d227620` job
  ends and SPU 0 to 2 do not leave geometry for an equal priority): 50.3 against 53.5 FPS. The workload also carries
  `72cc647b` jobs, and every change of workload reloads programs; the geometry polls grew by more than the flag wait
  shrank.
- **The noise loop's float arithmetic without the approximate mode's clamps** (FS, FM, FMA, FMS as plain host
  operations in its two chunks): the chunks' share of the noise thread did not change (6.0 and 3.6% against 6.1 to
  6.4 and 3.5 to 3.8%). Removed.
- `MFC Commands Shuffling Limit` (deferred transfers): the game stops. Huge pages for guest memory: the game's heap is
  made of 1 MB mappings, and TLB misses cost the render thread about 5% and the others 1%: not built.
- An `a6df6c09` kernel: one function of 1,250 instructions with trigonometric polynomials; not attempted.
- `SPU Reservation Busy Waiting`, short-sleep spinning (again): no change.

The fight at the sustained 30 W with everything: 45.9 to 60.0 FPS in 5 second windows, mean 54.0 over 20 windows
(three stretches of one session); later sessions of the same build gave means of 51.9, 57.8 (Mega) and 48.4 (Mega,
a minute and a half into the session). Sessions differ by 3 to 4 FPS in the mean (49.5 and 56.9 were measured for states
that A/B switching inside a session shows to be within 2 FPS of each other), so the absolute numbers of different
rounds do not compare; the switches inside one session do.

Tools added: `tools/chain.sh` (what each SPU thread runs), `tools/cold.sh` (boot without the savestate: the
opening leads into the same fight after five minutes), `tools/patchtry.sh` (a game patch, cold boot, does it reach
the fight), `tools/uptest.sh` (upload heap statistics), sampler files `.threads` (program per SPU thread, SPURS
workload per SPU), `.spurs` (workload priorities, ready counts), `.prio` (PPU call chains when a priority table
changes).

## Running it

`launch.py` in this folder starts the game on the fork build with the real profile, `play-config.yml` (RPCS3's
defaults with `SPU Block Size: Mega`, fullscreen, frame limit Auto) and a cache of its own (`~/.cache/rpcs3-gow3`).
Started once on the real profile, to the first compilation screen; the first start compiles for about ten minutes.
`--fast` and `--scale N` as described in its help. `~/.config/rpcs3/input_configs/BCES00510/Default.yml` is a copy of the inFamous 2 controller profile.

## Not done

- Only the main menu, the opening sequence and cutscene, and the first fight were seen. Nothing after it.
- The GPU anti-aliasing was compared with the game's by eye, on the menu, at 1280x720.
- Other regions and versions: the job is recognised by its bytes, the picture's address is taken from the game's
  blit, so another build of the same engine may work; none was tried.
