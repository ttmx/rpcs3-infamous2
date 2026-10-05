# inFamous 2 on RPCS3: investigation log

This is the working log of the optimization project, in the order things were found, lightly edited for publication
(session bookkeeping, process ids and personal paths removed). It records measurements, dead ends and corrections as
they happened, so later sections supersede earlier ones; the README at the top of the repository is the summary.

Reading notes:
- It was written in a private workspace. Paths such as `B/claude-work/...`, `B/geom-testing/...` or
  `B/playable-v14` name directories there that are not in this repository: evidence folders, captures, savestates and
  the numbered launcher packages. `B/playable-v14` corresponds to `infamous2/launch.py`; the benchmark scripts are in
  `infamous2/bench/` and the offline references in `infamous2/reference/`, some under new names.
- "The launcher" in sections before the last one set a GPU clock floor as well. In this repository that part is
  separate (`infamous2/hardware/`).
- All numbers are from one machine: Ryzen 7 8845HS, Radeon 780M (RADV), Arch Linux, game at 1280x720.

## Objective and constraints

The goal is the best practical performance on this laptop, ideally 60 FPS, including lightning effects and the particularly slow opening boss/other demanding areas. Preserve current image quality, game correctness, saves, and controller configuration. No resolution/effect reductions or new GPU clock/power-driver changes are authorized.

Prioritize substantial native replacements of **measured rendering-critical guest PPU PowerPC routines or SPU kernels**, or coherent algorithmic restructuring. Distinguish guest work from already-native host RSX/Vulkan work. Rank candidates using time-aligned rendering dependencies, not just CPU sample percentages. Use captured real inputs, differential correctness checks, guarded fallback paths, and repeated focused matched gameplay tests.

An earlier local SPU improvement with flat gameplay FPS is evidence that its block was not limiting that workload; it does not rule out all native lifting. Available FLOPS/bandwidth are a headroom hypothesis, not proof of 60 FPS.

## Current result, honestly scoped

- Best retained controlled checkpoint result: **50.40386 FPS**, from an earlier scripted checkpoint benchmark with a 1280×720 client. This is not the opening boss and not a universal result.
- New demanding area: **17.2247 FPS mean, 17.4 median, 14.39–19.0 range**, in a focused 30-second passive capture. All 150 focus samples were focused. Screenshot shows the character on a wooden dock by fire/water and a damaged bridge.
- User reports that a controller-disconnected warning changes the displayed FPS to approximately 40. That is a different workload/state, not an optimization gain.
- No new guest kernel has yet demonstrated a proven rendering-critical gameplay improvement in this new area.
- No code/configuration changes were made during the latest passive lag-area profiling and FPS-counter audit.

Do not conflate these different scenes, client sizes, binary revisions, or popup/focus states. The earlier mixed/unfocused 43.9/44.3 FPS sequence was invalid as an A/B benchmark.

## Machine and game

- AMD Ryzen 7 8845HS; Radeon 780M integrated GPU, RADV PHOENIX.
- Arch Linux, GNOME Wayland with X11/XWayland RPCS3 window, DISPLAY `:0`; recorded Mesa 26.2.4-arch1.1.
- Game: `~/Games/PS3/inFamous2`, BCES01143 v1.04; PS3 firmware 4.93.
- Current pinned quality: 1280×720 guest resolution, 100% scale, High shader precision, Bilinear output scaling. Current fullscreen client was 3840×2400; older checkpoint client was 1280×720.
- The launcher uses an existing application performance-profile hold through `powerprofilesctl`; this is not permission for additional GPU driver writes.

## Workspace and runnable winner

**Newest packaged build (2026-10-05): `B/playable-v14/launch.py` (v13 + fast GPU lighting, cleaned-up lighting and occlusion code; see the last section; not on the desktop entry). Before it: `B/playable-v13/launch.py` (v12 + descriptor set reuse). **This is the desktop build since 2026-10-05 night** (previous entry, v11, in `playable-v13/infamous2.desktop.previous`). `B/playable-v12` (texture and polygon offset changes inside fast runs) is the one before.**

**Previous desktop build: `B/playable-v11/launch.py` (v10 + pipeline reuse; `geom-testing/gbuffer-implementation/README.md`).**

**Previous desktop build: `B/playable-v8/launch.py`, with the tested
256-light GPU replacement binary.** The original `playable-best` paths/hashes
below are historical baseline information. The latest packaging details and
rollback are in the final section of this document.

Path abbreviations used below:

```text
P = <workspace>
N = P/native-investigation
B = N/build-prep
S = B/source
```

All shorthand evidence paths below are relative to these definitions.

Pinned playable build:

```text
B/playable-best/bin/rpcs3
SHA256 e71e7a57316a953cdea13a2f34577825dd3a49a95f7ed2024a72237c811329c1

B/playable-best/play-config.yml
SHA256 ad1b59bd68ee79a5728cf5786ac621921d8079d2089d69c7c87aaec6dff12b24

B/playable-best/launch.py
Desktop: ~/.local/share/applications/infamous2.desktop
Desktop name: inFamous 2 (Optimized)
```

The launcher selects the pinned configuration explicitly. The global RPCS3 configuration differs in several settings; do not silently substitute it or assume it describes the pinned run. The launcher strips inherited experimental RPCS3 flags/preload and enables these eight winner flags:

```text
RPCS3_EXPERIMENT_SPU_INSTCOMBINE=1
RPCS3_VK_READBACK_ADMISSION=1
RPCS3_SPU_NATIVE_RWV=1
RPCS3_VK_READBACK_SHARED_HITS=1
RPCS3_VK_READBACK_COMPRESSED_HITS=1
RPCS3_VK_SEMAPHORE_PIPELINE_PREFETCH=1
RPCS3_VK_READBACK_STREAM_COPY=1
RPCS3_VK_READBACK_OOP=1
```

Original profile: `~/.config`. Pinned cache: `B/variant-caches/instcombine`.

Current diagnostic build differs from the playable binary:

```text
B/build/bin/rpcs3
SHA256 1f188c17b3afd1447c1df9517e274de87ba6cc497ce45e66296b3ffec52f61a4
Fresh diagnostic cache:
B/diagnostic-caches/ordered-index-1f188c17b3af
```

**Do not disassemble the diagnostic binary to interpret pinned-process instruction addresses. Do not reuse/import old pinned native objects into the newer binary's cache.** `PPUThread.obj` differs. The diagnostic cache has a fresh exact-binary marker/admitted receipt and no native imports. Do not edit an old seed verifier's expected SHA merely to permit imports.

## Implemented and retained improvements

The playable winner includes earlier readback admission, LLVM/SPU work and readback-hit handling, plus the following measured host improvements:

| Change | Matched measured result | Qualification |
|---|---:|---|
| Semaphore pipeline prefetch | +5.938% | Submits recorded rendering earlier during qualifying semaphore waits; done before the user's overnight absence. |
| CPU streamed readback copy | +1.6305% | New overnight rolling matched comparison; accelerates CPU retirement after GPU completion. |
| GPU out-of-place fused swap/copy | +0.8625% | Repeated matched comparisons; 32 real readback shadow planes, approximately 117 MB, exact correctness PASS. |

These are separate rolling matched tests and must not be added as a universal combined percentage. Older session history also recorded readback admission around 37.15→39.08 FPS (~5.2%) at a different checkpoint; consult its original evidence before citing a new combined baseline.

The genuinely new overnight gain was modest, around the separately measured 1.63% and 0.86% changes, not the earlier semaphore gain. Do not inflate older work as new progress.

Primary historical evidence:

- `B/final-handoff.json`
- `B/continuation-state.json` — **historical overnight closure, not the latest live state**.
- `B/playable-best-final-handoff-audit.json`
- `B/overnight-matrix-20261004-080610/independent-combined-oop-assessment.json`
- `N/final-inventory/candidate-inventory.json`
- `N/final-inventory/independent-inventory-review.json`
- `N/final-inventory/post-lastdraw-source-assessment.json`

## Rejected or unproven experiments: do not repeat blindly

- Ordered texture index: 36,001 differential/lifecycle checks and 128 real stock-shadow checks passed, but clean ABBA/BAAB tests were negative: control 52.62275 vs index 52.47379 FPS, **−0.283%**. Flag remains off. Evidence: `B/index-clean-20261004-0941/independent-clean-matrix-assessment.json`.
- Float16 cast shader substitutions failed captured directed correctness tests (hundreds of thousands of lanes); not adopted. Integer guarded half-conversion model passed its restricted domain but had no established profitable GPU implementation.
- A cached-texture lock experiment had no eligible real texture hits.
- Existing interpolation/native SPU candidate local gains did not establish gameplay gains. Old pNv local ~8.4% improvement had flat FPS; do not keep polishing it without new critical-path evidence.
- Previously captured GPU draw spans are not whole-frame shader-active time or necessarily removable time. Last-draw parent interval ~2.968 ms, before-last-draw end ~2.214 ms, after ~0.753 ms; final 32 child sum ~0.021 ms/envelope ~0.392 ms. Gaps do not establish idle GPU time.
- Old focus/window-cover comparisons and popup FPS increases are not clean matched optimization benchmarks.

## Latest focused lag-area evidence

Directory: **`B/live-lag-area-20261004-focused`**.

Contains `provenance.json`, `fps-focus.json`, `start.png`, `perf.data`, `perf-script.txt`, `jit.map`, hardware telemetry and independent analyses:

- `guest-sample-analysis.json`
- `host-cpu-summary.json`
- `hardware-analysis.json`
- `ancillary-spu-analysis.json`
- `fps-counter-source-audit.json`

Capture: 30 seconds, 99 Hz user-cycle sampling; 200 ms FPS/focus logging. Verified window activation only; no controller input, restart, or settings changes. JIT map came from a preceding same-process capture, so newly compiled code can remain unmapped.

Weighted user-cycle findings (not elapsed frame-time percentages):

| Category/function | Share of all sampled user cycles |
|---|---:|
| Guest SPU JIT | 36.74% |
| Guest PPU JIT | 7.67% |
| `vk::wait_for_event` on primary SPUs | 21.611% |
| `vkGetEventStatus` | 1.692% |
| `spu_thread::do_list_transfer` | 4.506% |
| Readback stream copy | 1.855% |
| `spu_thread::get_ch_value` | 1.743% |
| Existing `spu_native_rwv` | 1.665% |
| pNv/cx044a8 guest SPU group | 4.304% |
| sxE/cx042c0 guest SPU group | 2.253% |

Primary SPUs each spend approximately 26–35% of their sampled cycles in GPU-event polling. These are **already-native host waits invoked by guest DMA faults**, not expensive PowerPC arithmetic routines. This is a strong dependency clue, but it does not say all that sampled work is removable or prove the GPU's exact limiting interval.

The pNv group is a four-sample gather/interpolation/transpose-packing kernel; sxE has vector float/reciprocal helpers. Cached IR exists. Symbol hash/chunk names do not directly identify high-level game functions. No newly dominant coherent PPU routine appeared; the top individual PPU symbol was only ~0.307% of all sample weight.

### Ancillary SPU is a different case

The ancillary SPU thread (`SPU[0x0000200]`) used ~91.76% of one logical CPU (peak 98.25%), but that is not proof of useful compute saturation. Its own weighted cycles were:

- Guest JIT 42.87%; native/runtime 57.06%.
- `get_ch_value` 27.72%, `notify_all` 5.36%, clock reads 5.36%, `get_events` 4.04%.
- **GPU-event polling 0%** — unlike the five primary workers.
- Pinned-binary disassembly places dominant `get_ch_value` samples around MWAITX reservation waiting and 128-byte reservation-data comparison. Sampling skid/wait accounting prevents calling all of this removable compute.
- ryrn guest group contributes 13.30% of this thread and contains substantial vector computation plus DMA completion checks. It is not simply a trivial guest spin loop.

### Host RSX and hardware observations

- RSX ~49.65% of one logical CPU; main PPU ~36.46%; whole process ~3.69 logical cores.
- RSX accounts for 12.427% of all sampled cycles. Within RSX: native RPCS3 ~52.36%, unsymbolized RADV ~25.08%, libc ~18.40%. No single dominant native RSX compute routine or RSX CPU saturation was established.
- `run_FIFO` mixes dispatch and empty/self-jump handling; samples are not all productive work.
- GPU busy ~69–70%; automatic GPU clock 800–1574 MHz; memory clock 400–937 MHz; edge temperature 61–67°C; socket power averages ~29.39–34.57 W.
- SPU last-CPU policy frequency median ~4588.6 MHz is **not effective executing MHz**.
- Scheduler runqueue statistics disabled (`sched_schedstats=0`): runqueue waiting is unavailable. Runtime accounting was populated.
- Effective clocks, CPU/hotspot temperature, throttle indicators and time-aligned GPU frame queries were not all available. These observations cannot prove GPU headroom or exclude thermal/power limits.

## Readback dependency and likely next measurement

Source inspection:

```text
S/rpcs3/Emu/RSX/VK/VKTextureCache.h:334  wait_for_gpu_readback
S/rpcs3/Emu/RSX/VK/VKTextureCache.h:345  imp_flush
S/rpcs3/Emu/RSX/VK/VKTextureCache.cpp:488  GPU event signal after transfer/transforms
S/rpcs3/Emu/RSX/VK/VKGSRender.cpp:928+  access violation handling
```

Confirmed call route in the subsequent DWARF capture:

```text
guest SPU MFC/list transfer
→ protected guest-memory access / signal handler
→ handle_access_violation
→ VKGSRender::on_access_violation
→ texture-cache invalidate / flush_all / flush_set
→ cached_texture_section::flush / imp_flush
→ vk::wait_for_event
```

The fence is signaled after image-to-buffer transfer and required conversion, byte shuffle or tiling. Prior rendering/queue dependencies may delay it. Admission serialization can rotate ownership among five workers; aggregate samples do not establish five simultaneous GPU readbacks.

Follow-up stack analysis counted 719 wait-containing samples: all included the SPU list-DMA/fault/flush route above. Within their weighted cycles, 78.21% used `wait_for_gpu_readback` and 21.79% used `imp_flush`. These are **popup-workload route statistics**, not an attribution of active-game frame time.

Current OOP optimization only covers untiled BGRA8 1280×720, 3,686,400-byte readbacks at `0x37400b80` or `0x37784b80`. Other resources take the stock path. Stream copy runs after completion. Semaphore prefetch applies only to qualifying semaphore/open-renderpass states; it does not necessarily advance every foreign-fault readback.

Next useful experiment: correlate fault address/context, primary submission request and execution, secondary submission, GPU transfer timestamps, fence completion, memory retirement and next guest rendering submission. Distinguish delayed submission, prior GPU work, transfer/transform cost and cache churn before proposing replacements. Obtain active-game callchains/timelines in the same focused laggy area.

Existing instrumentation appears sufficient for a bounded diagnostic run: `VKFrameIntervalTrace` (`RPCS3_VK_FRAME_INTERVAL_TRACE=1` plus `RPCS3_VK_FRAME_INTERVAL_TRACE_PATH`) records `emu_flip`, completed flips and presents. Readback CHAIN instrumentation records actual fault addresses/ranges, event cookies, command generations and submission/semaphore dependencies. Inspect its source for exact environment names and arming format before use. Both hooks require startup configuration; they cannot be enabled retroactively through another process's environment. The tail-demand fault hook targets one fixed address and is unsuitable for discovering arbitrary scene readbacks.

**Qualification:** `B/live-lag-area-20261004-callchains` is a 15-second DWARF capture taken during the controller popup, mean ~42.35 displayed FPS and only 18.67% focused samples. It establishes stack routes but is not a valid active-17-FPS timing comparison. Do not promote its FPS into benchmark evidence.

## Controller-popup FPS question

Latest user observation: “Wireless controller not connected” warning in front gives ~40 FPS instead of ~17.

Popup capture: `B/live-lag-area-20261004-popup`. Screenshots show dimmed game scene and text: “Wireless controller not detected. Please check connection and battery levels for the wireless controller.” Runtime log recorded SDL disconnect at emulated elapsed ~21:51. The exact text was not found in RPCS3 source; that alone is not definitive ownership proof.

Source audit establishes a genuine FPS-definition limitation:

- `gs_frame.cpp:834` increments title frames for every `m_frame->flip`, without filtering guest/native flips.
- `VKGSRender.cpp:2124–2130` creates native UI flip info with `emu_flip=false`.
- `VKPresent.cpp:475/526/999` invokes frame callbacks without filtering that flag.
- Performance overlay also increments on UI updates unless forced; native redraws are not universally excluded.
- **`RSXThread.cpp:2727–2729` increments `performance_counters.sampled_frames` only when `info.emu_flip=true`.** This is the useful separate guest-flip counter.

Therefore displayed 40 FPS does not prove 40 new game-world frames or simulation updates. The popup may also reduce/pause expensive guest work. We have **not** proved which mechanism explains this observation, nor quantified the native redraw share. Two subtly different screenshots with different PNG encoding do not establish frozen simulation. Separate guest flips/native UI redraws/simulation progress in a controlled capture; do not claim a HUD bug explains all slowdown.

## Savestate crash and recovery

The user crashed while attempting an RPCS3 savestate. Log reported:

```text
Failed to savestate: failed to lock SPU threads execution.
```

Failure happened in preparation before new savestate serialization. The old `.SAVESTAT.zst` was dated October 3, not a recovery of the failed new attempt. A normal in-game autosave existed from October 4 11:21:48, ~4 minutes before the 11:26 crash. We backed up normal savedata, the old state and plain crash log, terminated only the verified frozen emulator, and relaunched the pinned winner using normal game saves. User is now playing and confirmed a normal checkpoint near the new area.

Recovery evidence: **`B/savestate-recovery-20261004-112826`**, including `recovery.json`, `savedata-backup`, `optimized-RPCS3.log`, old state copy and restart evidence. Savedata copies were hash verified and PARAM.SFO headers valid.

**Use normal in-game saves; do not repeat savestate attempts.** Savestate-compatible SPU mode was not enabled because it changes performance/recompilation behavior. No conclusion that our optimization patches caused the crash is supported.

## Automation, cache and concurrency cautions

- Helpers: `P/hardware-investigation/fast_benchmark.py`, `thread_telemetry.py`, and `P/frame-pacing/x11_hotkey.py`.
- Live `/proc`, X11 and perf access have required host/escalated execution; sandbox process namespace may differ. Validate what namespace you are inspecting.
- Passive perf: 99 Hz user cycles, 30 seconds, own-process sampling; DWARF stack capture adds overhead and is not a clean speed benchmark. Record focus, scene, popup/controller status and binary/config identity.
- `/tmp/perf-643088.map` was generated from live JIT objects. ASMJIT `.objects` is a sparse ~4 GB archive under the pinned cache. Avoid copying the whole archive casually; record current map provenance.
- Existing diagnostic benchmark drivers restore an **old frozen checkpoint and send inputs**. Do not run them unchanged for the new user checkpoint. Adapt an isolated profile/save copy and preserve the original saves.
- `B/performance-lane.lock`: shared lock for builds/syntax; exclusive for live/offline timing. One coordinator owns shared source application/build/game control. Read-only analysis can run in parallel.
- Bounded font-cache link cleanup exists in `N/fullscreen-output-test/run-output-matrix.py`; do not delete arbitrary cache links/targets. Some profiling scripts do not invoke cleanup automatically.
- Apply source changes with fresh modification times; `copy2` previously preserved timestamps and caused Ninja to miss rebuilds.
- Stop only verified owned test processes and wait for disappearance before parsing shutdown logs. Never kill the compositor wrapper.
- The overnight owned sleep inhibitor was released; cleanup evidence is `B/owned-inhibitor-release-result.json`. No current overnight sleep hold should be assumed.
- If workspace requirements fail again, report the actual error and re-establish workspace rather than guessing.

## Recommended order at that point

1. Verify current session state and preserve all evidence. If user is actively playing, start passively and avoid unnecessary restart/input changes.
2. Separate guest flips from UI redraws to resolve the popup observation; this is also necessary for trustworthy FPS benchmarks.
3. Capture a focused active-game dependency timeline at the new saved area, especially guest DMA faults → GPU readback completion → subsequent guest rendering submission.
4. Rank coherent guest kernels only after showing their wall-time contribution on that dependency chain. Keep host GPU waiting distinct from guest arithmetic.
5. Implement the most promising bounded change with real-input differential checks, exact semantics/guards and fallback. Then use repeated matched focused A/B gameplay with unchanged quality and consistent output size. Reject flat/negative results and retain them.

No evidence currently supports promising 60 FPS everywhere. The strongest new clue is the primary SPUs' host GPU-readback dependency in the demanding area, together with a separate reservation/event-heavy ancillary SPU. Resolving their timing is more useful than another small generic tweak or assuming all sampled busy time is arithmetic that can be lifted.

## 2026-10-04 afternoon update: periodic submit (supersedes "Current result")

New runnable winner: `B/playable-periodic/launch.py` (desktop entry now points here; old entry saved at `B/claude-work/infamous2.desktop.bak`, old winner untouched in `B/playable-best`). Binary SHA256 prefix 5966e80ff791a286, built from `S` as of this date.

Finding: in the heavy dock scene each ~55 ms frame was ~26 ms of RSX-thread recording followed by ~29 ms where RSX, all five SPURS workers and the main PPU thread waited for the GPU before the per-frame 1280x720 readback. Evidence: `perf record --switch-events` timeline (works unprivileged with perf_event_paranoid=2).

Changes:
- `RPCS3_VK_PERIODIC_SUBMIT_US=<us>` (VKGSRender::maybe_periodic_submit, called after each draw in VKDraw.cpp): flushes the command queue when the last submit is older than the interval. `RPCS3_VK_PERIODIC_SUBMIT_CTL=<file>` re-reads the interval about once a second for live A/B. Dock scene, GPU auto, same session: off 17.5/17.7/17.6 FPS; 4000us 25.8; 2000us 27.2; 1000us 27.7; 500us 27.3; 250us 26.5. Pier (mission start): 46.8 -> 49.4.
- Strict Rendering Mode off (`play-config.yml`; strict copy kept as `play-config-strict.yml`): pier 49.4 -> 52.8 with periodic on. Screenshots matched at the pier only; NOT visually checked at the burning dock.
- Multithreaded RSX: pier 49.7 vs 49.4, not adopted.
- GPU policy `high`: alone 19 -> 27.4 at the dock (two trials, screenshots in `B/claude-work/gpu-clock-trial-screenshots.png`), but only +0.5-1.3 FPS once periodic submit is on; left on auto. Helper installed: `sudo -n /usr/local/bin/rpcs3-gpu-perf high|auto` (sudoers rule `/etc/sudoers.d/zz-rpcs3-gpu-perf`).

Remaining limiter at the dock (~28 FPS): RSX thread ~78% on-CPU: upload_vertex_data ~20% (dma_manager::copy), create_temporary_subresource/update_image_contents ~15% (strict-mode RTT copies; re-measure with strict off), texture and render-target loads from guest memory ~17%, RADV ~28% overall.

Benchmarking: `B/claude-work/run.py` (launch/stop/key/fps/shot) and `bench.sh <cfg> <label>` load the autosave at the pier by keyboard (ProfilingKeyboard) and measure. Results log: `B/claude-work/results.jsonl`. Reaching the dock needs the user to play ~several minutes of combat. Blind key presses at boot rewrite the Autosave slot; pre-session saves are in `B/claude-work/savedata-backup-20261004-1300`.

## 2026-10-04 evening update: RSX-thread work (supersedes playable-periodic)

Runnable winner is now `B/playable-v3/launch.py` (desktop entry points here). Binary SHA256 prefix f88edd5e1c45a787. Env: playable-best flags + `RPCS3_VK_PERIODIC_SUBMIT_US=1000`, `RPCS3_VK_LIVE_CTL=playable-v3/live.ctl` ("1000 65536 1 1"), `RPCS3_EXPERIMENT_BLIT_COMPLEMENT=1`, `RPCS3_FIFO_INLINE_CACHE=1`, `RPCS3_EXPERIMENT_LAST_IMAGE_VIEW=1`; config Strict Rendering off.

Live control file (VKLiveCtl.hpp, polled in queue_swap_request): index 0 periodic submit us; 1 minimum bytes for streaming (non-temporal) dma_block::load; 2 same-frame multi-block vertex cache (flat table in VKGSRender.h, keyed like vertex_batch_reuse::key, no memcmp, purged per frame like the stock weak cache); 3 prefetch mask (1 vertex copies, 2 complement upload, 4 streaming loads).

Once periodic submit removed the GPU wait, the RSX thread became the limiter (94-97% on-CPU at the pier), so RSX-side changes previously shelved as "no gain" now pay. Pier (mission start, loaded by `claude-work/bench.sh`), FPS:
- original pinned build 46.8; + periodic submit 49.4; + strict off 52.8-53.6; + streaming DMA loads 55.9; + vertex cache (unordered_map) 55.85 vs 55.2 off
- shelved flags, each alone on top: BLIT_COMPLEMENT 59.1, FIFO_INLINE_CACHE 56.7, LAST_IMAGE_VIEW 57.0
- The game caps itself near 60 with Vblank Rate 60, so use `claude-work/cfg-bench-vb120.yml` (Frame limit Off, Vblank 120; benchmark only) to see differences: all flags 58.3; + vertex prefetch 58.7 / 59.4; + flat-table vertex cache 60.7.
- Final with the real play config: 59.5 vs 49.9 with the four live-control items off.

Rejected: Multithreaded RSX (55.5 vs 55.9); RSX FIFO Fetch Accuracy Fast (game froze in a menu); pinning RSX to an exclusive core (no change); prefetch for complement/streaming loads (no change); GPU `high` (see above).

Bench caveat: `bench.sh` only passes the three flags when EXTRA_ENV is exported in the same shell call. `ready.py` checks for the minimap before measuring; a flat 59.97 means it is stuck in a menu.

Not measured: the burning dock with any of the post-periodic changes (needs the user to play there). Remaining pier RSX costs are diffuse: vertex memcpy ~7%, program lookup/bind ~14%, texture descriptors ~11%, RADV ~18%, FIFO parsing ~10%.

## 2026-10-04 late afternoon: savestates fixed, dock benchmark loop

Savestate creation crashed because of `RPCS3_EXPERIMENT_SPU_INSTCOMBINE=1`: pausing SPU threads segfaults in the SPU runtime trampolines (same fault with stock Compatible Savestate Mode and with RPCS3_SPU_NATIVE_RWV=0). Creating a state works with INSTCOMBINE=0 + NATIVE_RWV=0 + `Compatible Savestate Mode: true` (`claude-work/cfg-nostrict-compat.yml`); Ctrl+S can be sent with `frame-pacing/x11_hotkey.py WIN s --state 4 --activate`. States made that way load fine under the normal fast config (instcombine on, compat off). My RPCS3_SPU_SAVABLE_EVENT_WAIT patch did not help and is unused; the retry count in try_lock_spu_threads... was raised 15 -> 60.

States: `claude-work/states/dock.SAVESTAT.zst` (burning dock, the heavy scene) and `pier-kb.SAVESTAT.zst`. `claude-work/dbench.sh <bin> <cfg> <label> "<ctl>"...` boots the dock state with keyboard input, waits for the minimap and measures; GPU=high|auto, EXTRA_ENV, SETTLE, SECS. About 10 s to gameplay.

Dock numbers (some taken on battery and at 100 C CPU, see below): v3 build 37.6 auto / 47.6 high; v4 ~48.5 high; v6 (fingerprint vertex cache, periodic check every 8th draw; `claude-work/bin-v6`) ~50 high, 41.2 auto. Vertex-copy offload (live control 4 + RPCS3_RSX_COPY_OFFLOAD=1) and Multithreaded RSX were both slower at the dock.

Measurement hazard found at the end: the charger was unplugged (ACAD online=0, ~63 W from battery) and k10temp read 100 C with cores at ~3.4 GHz during the GPU-high dock runs, with FPS drifting down 2-4 within a session. Re-measure on AC and watch k10temp before trusting small differences. GPU `high` adds heat that can throttle the CPU, which is the limiter; a mid GPU floor may be the better trade.

The desktop launcher still points at `playable-v3` (v4 binary, GPU auto). v6 is not packaged yet.

## 2026-10-04 evening: dock results on AC, launcher v4

Runnable winner: `B/playable-v4/launch.py` (desktop entry points here): binary `claude-work/bin-v7`, live.ctl "1000 65536 1 0 0 0 0", flags as v3, Strict off, and a GPU clock floor of 1600 MHz set through `sudo -n /usr/local/bin/rpcs3-gpu-perf floor-1600` and restored to auto on exit (helper accepts high | auto | floor-1000..2400 step 200).

Dock (state `claude-work/states/dock.SAVESTAT.zst`, on AC, v7), FPS / package W: auto 43.0 / 40.3; floor-1200 46.0 / 42.0; floor-1400 48.4 / 43.4; floor-1600 49.8 / 44.3; floor-2000 51.6 / 46.7; high ~54.7 / 53.5 (CPU reaches 100 C and throttles within a minute under high). floor-1600 is the best FPS per watt. Morning baseline at the dock was 17.6.

IMPORTANT for A/B at the dock: with a floor of 1600 the scene is partly GPU-bound, so RSX-thread savings do not show; use GPU=high for RSX-side comparisons (RSX thread 93-98% busy there) and keep runs short because of heat.

v7/v8 changes: vertex cache uses a 128-bit fingerprint in a flat table and disables itself for 120 frames when under half of the draws repeat (at the dock the always-on cache cost ~1 FPS; at the pier it gains ~2%); periodic-submit clock read only every 8th draw; live controls 5 (SPU reservation-checker MWAITX spin cap) and 6 (usleep addend +1).

Rejected at the dock: vertex-copy offload thread and Multithreaded RSX (slower); SPU checker spin cap 100-2000 (no FPS or power change; SPU[0x200] load is a guest polling loop); usleep addend +50/+200 us (-0.3 to -0.9 W, up to -1.5 FPS); source prefetch for vertex copies, streaming loads and the complement band, including a row-aware version (no change even RSX-bound).

Open item: `copy_unmodified_block::copy_mipmap_level` for the 737 KB complement band is ~4% of the RSX thread; the loop is AVX-512 and stalls on its loads, yet prefetching does not help. Cause not established.

User preference (stated 2026-10-04): prefer software changes that make the game lighter over hardware tuning; hardware tuning is acceptable only where the hardware behaves pathologically (the GPU governor not boosting). Power draw and heat matter.

## 2026-10-04 late evening: zero-copy and vertex-cache upper bounds (both negative)

Zero-copy (passthrough DMA) on amdgpu: the kernel accepts memfd/shm-backed user pointers; only libdrm's hardcoded AMDGPU_GEM_USERPTR_ANONONLY flag (0xe) refuses them (`scratchpad userptr_test.c`: anon ok, memfd with 0xe EPERM, memfd with 0xc ok, read-only mapping EPERM). `claude-work/shim/userptr_shim.so` (LD_PRELOAD, interposes drmCommandWriteRead and clears the flag) plus `RPCS3_VK_FORCE_PASSTHROUGH_DMA=1` (VKGSRender.cpp) makes RPCS3's passthrough DMA run on RADV and render correctly (`claude-work/pt-pier.png`). It is a net loss: pier 35 FPS with periodic submit at 1000 us, 42.8 with it off, versus ~58-60 normally; the RSX thread spends ~40-65% of its time in the kernel because amdgpu re-validates every userptr page on each command submission. Not adopted. A zero-copy design that avoids this would have to back guest memory with GPU-allocated (dma-buf) memory instead.

Vertex upload: `interleaved_range_info::calculate_required_range` already caches its result, so there is no duplicate work to remove. Upper bound of a cross-frame vertex cache, measured with a deliberately unsafe no-validation mode (live control 2 = 3, `claude-work/bin-v10`), dock, GPU high: 55.26 vs 54.9 without any cache (+0.6%). The game writes fresh vertex data to new addresses every frame, so caching cannot help; the remaining vertex cost is the copy itself.

Launcher unchanged (`playable-v4`, bin-v7). bin-v8..v10 only add experiment switches that are off by default.

Remaining per-draw candidates, each expected to be worth 1-2%: fragment program lookup hashes and then compares the whole microcode on every program change (ProgramStateCache.h search_fragment_program); texture descriptor lookups; FIFO parsing.

## 2026-10-04 night: the SPU round trip is deferred lighting

Upload trace at the dock (RPCS3_VK_UPLOAD_TRACE): every frame the RSX thread DMA-loads 0x37400b80 (3.6 MB), 0x37784b80 (3.6 MB) and 0xcf800000 (0.9 MB). `RPCS3_VK_ROUNDTRIP_DIAG=1` (VKDMA.cpp, diagnostic only) shadows what the GPU read back into each 1280x720 buffer and compares at upload time: 720/720 rows changed, with flush count == load count, so the SPUs rewrite both buffers in full every frame. `RPCS3_VK_ROUNDTRIP_DUMP_DIR` dumps before/after; images and raw dumps are in `claude-work/roundtrip/` (sheet-a.png, sheet-b.png; bytes are A,R,G,B).

- 0x37400b80 before: surface normals in RGB, a material-like mask in byte 0. After: a light accumulation image (orange fire light on the nearby geometry, black elsewhere).
- 0x37784b80 before: depth packed across the colour bytes. After: all zero in this scene.

So the game renders a G-buffer (normals + depth), reads it back, computes deferred lighting on the SPUs, and uploads the light buffers for the final pass. The pNv "four-sample gather/interpolation" and sxE kernels profiled earlier belong to this job. It is a pure function of (two G-buffer images + light list) -> (two light images), which makes offline differential testing possible. Replacing it with a GPU pass would remove the readback, the SPU lighting work and both uploads; that needs the light list format and lighting model reverse-engineered.

Other per-draw findings this round: fragment program lookup does a hash pass plus a compare pass per program change (~2.5% of the RSX thread); a 3.4% texture upload under load_texture_env is one of the two light buffers above.

## 2026-10-04 night: Stage 1 start, static SPU inventory

User decision: replace the SPU deferred-lighting job incrementally with native code (Stage 1), handling never-seen kernels only when gameplay reaches them; every replacement keyed to exact bytes with fallback to the original SPU code.

Tooling and data in `B/claude-work/spu-inventory/` (see its README). Findings:
- Only zlib and Bink are SPU ELF files. The game's own jobs are raw SPURS job binaries stored back to back in the executable (file ~0x7fa000-0x918000), mostly loading at LS 0x4030.
- About 39 game programs; strict byte matching gives ~243k instructions of which ~230k (94%) have already executed after playing into mission 2. 652 of 3937 cache blocks are not found in the executable (code loaded from elsewhere or blocks with relocated words).
- Hot lighting kernels map to two adjacent programs of identical size (0x5C78 bytes each): P071 at file 0x865540 (LS 0x4040; holds 1w2k/07170, MJW/08ae8, ns7n, 8Yjs) and P075 at file 0x86b1b8 (LS 0x4038; holds pNv/044a8, sxE/042c0, W81t). Never-executed code: P071 ~1056 instructions, dominated by one 911-instruction range at LS 0x7bc4; P075 ~266 instructions in 15 small ranges.
- The ryrn kernel (SPU[0x200] thread) is in P193-P195, not part of lighting.
- Program boundaries are approximate; cache blocks store their entry address but data may start lower (lower_bound), so matching is by content.

Not done yet: runtime log of which SPU program issues MFC transfers to 0x37400b80 / 0x37784b80 (to confirm P071/P075 are the only ones touching the lighting buffers); per-kernel share of SPU time on the current build; first native kernel under the Stage 1 scheme.

## 2026-10-04 night: Stage 1 measurements at the dock (bin-v12)

Buffer access (`RPCS3_SPU_BUFFER_ACCESS_DIAG=1`, hooks do_dma_transfer and the list elements in do_list_transfer; program = FNV-1a of LS 0x4040..0x4140): only two programs touch the lighting buffers through MFC lists.
- P071 (hash 01f9aa7f): GETs and PUTs the whole depth buffer 0x37784b80 in 128-byte list elements (about a million tiny transfers per second), call sites pc 0x707c (GET) and 0x6ea4 (PUT).
- P075 (hash 8f2e59a2): GETs the normals buffer 0x37400b80 (640-byte elements) and the depth buffer twice over (about 1128-byte elements), pc 0x6004. No list PUT to 0x37400b80 was seen from any program, yet that buffer is fully rewritten each frame; the writer is not identified (the recompiler handles constant MFC_PUT inline, which the hook does not see).
P071 and P075 are different programs (54 of 5918 words equal at the same offset); the equal sizes come from how boundaries were inferred.

CPU use at the dock, floor-1600, ~51 FPS (`claude-work/cpu.sh`): SPURS workers 3.38 cores, other SPU threads 1.01, RSX 0.96, PPU 0.77, total 6.13.
Worker time by symbol (perf with JIT map from the ASMJIT .objects archive): guest SPU code 74%; pNv/044a8 10.8%, sxE/042c0 5.3%, MJW/08ae8 3.6%, native RWV 3.4%, 1w2k/07170 3.0%, Z7np/0d410 2.2%, ehgy 1.8%, then a long tail under 1.5% each. MFC plumbing (do_list_transfer 7.0%, process_mfc_cmd 2.2%, do_dma_transfer 1.8%, locks ~3%) is about 14%.
Enabling the existing shelved native kernels RPCS3_SPU_NATIVE_SXE=1 and RPCS3_SPU_NATIVE_07170=1 changed nothing measurable (workers 3.37 cores, 44.3 W, 51.7 FPS).

Implication: the named lighting kernels are roughly 1 core of 6.1; a one-for-one native rewrite of already-JITed SIMD code is unlikely to save much. The tiny-element list transfers are an emulator-side cost (~0.5 core) that could be reduced generically.

## 2026-10-04 late night: list-transfer path closed, lighting RE starting point

Tiny list transfers: the hot instructions in spu_thread::do_list_transfer are the list-element loads and the data loads themselves (memory stalls on freshly read-back buffers), not bookkeeping. Next-batch source prefetch for GET lists (live control 7 = bytes per element, `claude-work/bin-v13`) did not reduce worker CPU from fresh dock loads: 3.33-3.34 cores off, 3.36 at 128 bytes, 3.40 at 640 bytes. Left compiled in, off. (An earlier in-session drift from 51 to 59 FPS was the user moving the camera, not the savestate changing.)

Decision (user, 2026-10-04): do not rewrite lighting kernels one-for-one; instead reverse-engineer what P075/P071 compute so the pass can be moved to the GPU (Stage 2), and look at never-seen kernels only when gameplay reaches them.

Existing material to build on, from the earlier sessions (they analysed kernels structurally without knowing the buffers were normals/depth -> light):
- `P/frame-pacing/hottest-spu-guest.asm` and `hottest-spu-partial-decompile.txt`: annotated disassembly and pseudocode of pNv/044a8 (732 instructions; gathers four samples from an LS grid and interpolates, one output quadword per iteration, no DMA inside).
- `N/042c0-lift`, `N/07170-lift`, `N/07170-static`, `N/08ae8-lift`, `N/08ae8-cross-pixel` (captured inputs, reference and native implementations, bench), `N/dma-tile-job`, `N/890-job-mapping`, `N/deferred-copy-lifecycle`, `N/findings.txt`.
No SPU disassembler is installed; RPCS3 only logs disassembly for blocks it builds in a session.

Next for Stage 2: (1) establish the job-level data flow per frame: what P071 does to the depth buffer, what P075 reads besides the two buffers (the light list / grid in LS), and who writes the light image to 0x37400b80; (2) capture one frame's complete inputs and outputs (both buffers before and after, plus the job descriptors) as a test vector; (3) write an offline reference that reproduces the output image from the inputs, then port it to a compute shader.

## 2026-10-05: lighting RE, first structural results

Tools (in `claude-work/spu-inventory/`): `spudis.py` (SPU disassembler generated from RPCS3's SPUOpcodes.h; checked against the earlier pNv listing), `jobs.py` (enumerates two container formats, writes jobs.json).

Container formats found in the executable:
- Type D, magic da7aba5e at +0x10: 48-byte header loaded at LS 0x4000 (16-byte id, +0x14 code offset 0x30, +0x18 image size), code from LS 0x4030. Five of these sit back to back at file 0x86d500-0x870d90: sizes 196, 740, 484, 740 and 1348 words. The 1348-word one (file 0x86f880) contains pNv: its 732-instruction loop starts at file 0x86fd28 = LS 0x44a8.
- Type C, magic c0dec0de at +0x18: 8-byte id, +0x08 code size, +0x0c total size, +0x14 header size 0x30, +0x20 load address 0x4000, code at +0x28. Three found (file 0x818c08, 0x821f88, 0x846108). Other programs use header variants not parsed yet, so the earlier coverage-by-content inventory remains the complete list.
So what was called P075 is really one job (file 0x86b1b8, runtime hash 8f2e59a2, pc 0x6004 when it GETs the buffers) followed by five small kernel binaries; P071 (file 0x865540, hash 01f9aa7f) is a separate job.

Buffer semantics at the dock (`claude-work/roundtrip/channels.png`; rows: 0x37400b80 before/after, 0x37784b80 before/after; columns: byte 0..3):
- 0x37400b80 before: byte 0 material mask, bytes 1-3 normal. After: byte 0 is a blocky tile map (about 32-pixel tiles) set where lights apply, bytes 1-3 light colour.
- 0x37784b80 before: bytes 0-2 depth (high to low), byte 3 a character mask. After: byte 0 a faint intensity image near the lit area, bytes 1-3 zero.
This is tiled deferred lighting; output is produced per tile.

Transfers (diag now also hooks do_putlluc/do_putllc, `bin-v14`): P071 GETs both buffers and PUTs 0x37784b80, all in 128-byte list elements covering the whole buffer each frame; the 8f2e59a2 job GETs both buffers. No PUT, PUTLLUC or PUTLLC to 0x37400b80 from any SPU program was observed, so the writer of the light colour image is still unidentified (PPU code or a GPU path are the remaining candidates).

## 2026-10-05: the two SPU passes identified (corrects the previous section's open question)

Correction: the "no SPU write to 0x37400b80" statement above was a log-filtering mistake. Hardware write breakpoints (`perf record -e mem:<host addr>/8:w`, guest EA + 0x300000000 for the normal view and + 0x400000000 for the sudo view) on a lit pixel show exactly two writers: the readback stream copy, and `spu_thread::do_list_transfer`.

Full per-job I/O map (`RPCS3_SPU_BUFFER_ACCESS_DIAG=2`, `claude-work/spu-inventory/job-io-map.txt`):

Job 1, tiled deferred lighting = P071 (file 0x865540, hash 01f9aa7f; listing `listings/lighting-P071-file0865540.asm`, 5918 words, ~1200 float ops):
- GET and PUT of both 0x37400b80 and 0x37784b80, whole buffers, 128-byte list elements (GET pc 0x707c, PUT pc 0x6ea4): lighting is computed in place.
- one 256-byte GET from 0x00a93c80 and one 384-byte GET from the table 0x37d6d690-0x37d735e0 per job run (pc 0x1ffc): job parameters / light data, to be decoded.
- PUTLLC on 0x00a93a00-0x00a93c80 (pc 0x95b8) and the shared 0x00a40a80 / 0x00b01d00 lines: job queue bookkeeping.

Job 2, screen-space ambient occlusion = dispatcher at file 0x86b1b8 (hash 8f2e59a2; `listings/ssao-dispatcher-file086b1b8.asm`, 2258 words, almost no float ops, 90 channel ops) plus the five type-D kernels it loads by GET from 0x0087d500-0x00880d90 (pc 0x6234), which are pure LS-to-LS math with no channel ops (`listings/kernel-03..07-*.asm`: 184, 728, 472, 728, 1336 words; kernel-07 holds pNv):
- GET of 0x37400b80, 0x37784b80 (twice over) and a third region up to 0x37c22480, elements 112-5120 bytes (pc 0x6004).
- PUT to an intermediate buffer 0x37b08b80-0x37c21f80 (~1.1 MB, elements 80-2560 bytes) and to 0xcf800000-0xcf8e1000 in 160-byte elements (pc 0x5e2c).
- 0xcf800000 is a 1280x720, 1 byte per pixel ambient-occlusion image (`claude-work/roundtrip/frame2/ao-1280x720.png`; sky value 100, creases dark). This is the 900 KB per-frame upload seen earlier.

Rough worker-time shares at the dock from the named kernels: SSAO at least 17-19% (pNv 10.8, sxE 5.3, W81t 1.3, ...), lighting at least 9% (MJW 3.6, 1w2k 3.0, 8Yjs 1.2, ns7n 1.0) plus most of the 128-byte transfer cost.

Test data captured from one dock frame: `claude-work/roundtrip/frame2/` (both G-buffers before and after lighting, the AO image, screenshot). The A/B dumps and the AO dump are each taken at that buffer's 240th load, so they may be one frame apart.

Stage 2 implication: both passes must move to the GPU to remove the readback; either alone still needs it. SSAO is the simpler first target (one output image, kernels are pure functions, inputs are just depth and normals).

## 2026-10-05: lighting job work-unit shape

Verbatim transfer window (`RPCS3_SPU_BUFFER_ACCESS_DIAG=2`, `claude-work/spu-inventory/job-io-seq.txt`, `bin-v17`; the diag now also reports LS addresses) for P071 on one worker:
1. GET 256 bytes from 0x00a93c80 to LS 0xa000 and 384 bytes from the table at 0x37d6d690.. to LS 0xa100 (pc 0x1ffc): the work item.
2. GET 40 elements x 128 bytes from the normals buffer, element EAs stepping by 0x1400 (one 1280-pixel row), i.e. a 32-pixel-wide, 40-row block, into LS 0x12b00..; then the same block from the depth buffer (EA + 0x384000) into LS 0x15300.. (pc 0x707c).
3. A second block 384 bytes (96 pixels) further along the row, same pattern, into LS 0x13f00.. / 0x16700...
4. PUT of the same four blocks back from LS 0x10000.. / 0x17b00.. / 0x11580.. / 0x19080.. (pc 0x6ea4), with a PUTLLC on 0x00a93a00 between phases.
So the lighting tile is 32 x 40 pixels (40 columns x 18 rows = 720 tiles per frame), a work item covers two tiles, and all tiles are read and written every frame whether lit or not. The 384-byte record is the next thing to decode (expected: tile coordinates and the lights affecting them). The trace window contained no SSAO-job rows; rerun to get its sequence.

## 2026-10-05: SSAO pipeline, offline reference started (`claude-work/ssao-ref/`)

Images are being sent to the user as progress (00-inputs-and-target, 01-v0-generic-guess, 02-v1-two-of-three-stages). `common.py`/`v0.py`/`v1.py`/`stage3.py` are the scripts; `f3-*.npy` is the frame-consistent test vector from `roundtrip/frame3/` (dump taken when the AO image is uploaded: C buffers and AO are consistent with the readback copies `rt-*-before.raw`; `mem-37400b80/37784b80` at that moment already contain lighting output, so lighting finishes before the AO upload).

SSAO job transfer sequence (`spu-inventory/ssao-io-seq.txt`): intermediates live in guest memory right after the depth buffer:
- C1 = 0x37b08b80, 640x360 big-endian float32 linear depth (921600 bytes)
- C2 = 0x37be9b80, 640x360 one byte per pixel occlusion (230400 bytes)
Stages seen: (a) GET 49 rows x 512 bytes (128 floats) from C1 -> PUT 45 rows x 128 bytes to C2; (b) kernel 0x87ec80 (the fourth type-D kernel, 2960 bytes) loaded to LS 0x6c00, then per work unit GET 32x112 bytes from C2, 32x352 bytes from C1, 60x640 bytes from the full depth buffer -> PUT 60 rows x 160 bytes to 0xcf800000 (1280 bytes per row). In the traced window the SSAO job read only depth-derived buffers, never the normals buffer.

Recovered so far:
- Stage 1 (full depth -> C1): top-left pixel of each 2x2 block, 1/z = 0.100000347 - 5.96042646e-09 * d24 (near ~10, far ~1.0e6; sky z ~1.003e6). Matches the game's C1 to ~1e-6 relative.
- Stage 3 (C2 + C1 + full depth -> AO): joint bilateral 2x upsample, bilinear weights times 1/(eps + |z_half - z_full|/z_full); with the game's own C2 this gives mean error ~2.2/255, 78% of pixels within 1 level, 94% within 8 against the game's AO. Exact weight function not yet taken from the code.
- Pixels whose G-buffer byte 0 (material) is 0 (sky, water) mostly get the constant 100/101 instead of occlusion.
- Stage 2 (C1 -> C2) is NOT explained by simple depth-curvature or depth-difference models (R^2 ~0.03). The stage reads only 2 extra rows above and below each 45-row band, which hints at a sweep-style or multi-pass method rather than a wide sampling kernel. Needs the code.

Plan for stage 2: write a small SPU interpreter in Python (the five kernels use 58 mnemonics and no channel ops) and run the real kernel on captured entry state. The existing one-shot capture (`RPCS3_EXPERIMENT_SPU_CAPTURE_DIR`, `RPCS3_EXPERIMENT_SPU_CAPTURE_HASH`, default the pNv block) writes pc, all 128 GPRs and the full LS at a selected block's entry; it needs extending to capture at kernel entry and exit. Unresolved: pNv's profile symbol is at LS 0x44a8 (kernel image based at 0x4000) while the dispatcher loads kernels at 0x6c00, so which job runs the largest kernel, and at what base, is still to be pinned down.

## 2026-10-05: SSAO is five stages; offline SPU interpreter validated

Correction to the previous section: there are five stages, not three, and "stage 2" there was the column blur, not the occlusion computation. Per frame the SSAO job (dispatcher, a type-C program loaded at LS 0x4000) loads one kernel at LS 0x6c00 per stage, in this order (`spu-inventory/ssao-stage-trace.txt`):
1. 0x87d500 (kernel-03, 784 bytes): GET full depth rows (5120 bytes) -> PUT C1 rows (2560 bytes). Downsample + linearise.
2. 0x87f880 (kernel-07, 5392 bytes, contains pNv): GET normals (640 bytes), depth (416), C1 (416) -> PUT C2 in 80-byte pieces. Raw occlusion; input windows are 104 wide for 80 output pixels (12-pixel margin).
3. 0x87d880 (kernel-04, 2960 bytes): GET C1 rows 2592 bytes, C2 rows 672 bytes -> PUT C2 rows 640 bytes. Full-row (horizontal) depth-aware blur.
4. 0x87e480 (kernel-05, 1936 bytes): GET C1 49 rows x 512 bytes, C2 128 bytes -> PUT C2 45 rows x 128 bytes. Banded (vertical) depth-aware blur, works in place on the band fetched from C2.
5. 0x87ec80 (kernel-06, 2960 bytes): GET depth 640, C1 352, C2 112 -> PUT 0xcf800000 in 160-byte pieces. Upsample.
Kernel EAs are executable file offset + 0x10000. Lighting runs after SSAO: at stage starts 1-4 the depth and normals buffers hold this frame's readback; at stage 0 they still hold the previous frame's lighting output (the readback happens lazily on the first SPU read).

Tools (`claude-work/spu-inventory/`): `spuemu.py`, a Python SPU interpreter (about 146 mnemonics, RPCS3 interpreter semantics incl. FREST/FI lookup tables in `spu_luts.json`). Validated: a column-blur band computed entirely inside the emulated window is 100% byte-identical to the game's.
Captures: `RPCS3_SPU_SSAO_CAPTURE_DIR` (pre/post states around the column blur) and `RPCS3_SPU_SSAO_STAGE_CAPTURE_DIR` (SPU state plus all buffers at the first kernel load of seven consecutive stages; `claude-work/ssao-ref/stages/`). Both must be run with `cfg-spu-interp.yml` (SPU Decoder: Interpreter (static)), because with the LLVM recompiler the in-memory GPRs are not all current at a transfer.
`ssao-ref/stage_emu.py <n>` emulates a whole stage single-threaded from a stage-start capture (MFC lists, GET/PUT, GETLLAR/PUTLLC against the snapshots; stops when the next kernel is requested or the job idles) and compares with the next stage's snapshot. Column blur: 36 of 40 bands processed (4 already claimed by other threads at capture time), 5.1M instructions, resulting C2 98.16% byte-identical to the game's. Per-stage ground truth extracted as `ssao-ref/s-*.npy` (A, d24, z, raw, blurh, blurv, final); `03-the-five-stages.png` shows them.

Whole-stage emulation results (`ssao-ref/stage-emu-log.txt`, outputs `stage<N>-emu-mem.npz`):
- stage 0 downsample: 1.06M instructions; comparison invalid because the stage-0 snapshot's depth buffer is the previous frame's lighting output (rerun with the depth/normals from the stage-1 snapshot).
- stage 1 raw occlusion: 35.7M instructions (81 s in Python), 64 output units; C2 78.3% byte-identical and 100% within 1 level of the game's raw occlusion (remaining differences look like last-bit float rounding in the interpreter).
- stage 2 row blur: 5.2M instructions; C2 98.4% identical.
- stage 3 column blur: 5.1M instructions; C2 98.2% identical.
- stage 4 upsample: 19.8M instructions; AO output 99.3% identical, 100% within 1 level.
Images sent to the user so far: 00 inputs/target, 01 v0 generic guess, 02 v1 (our stage 1 + upsample around the game's middle), 03 the five stages, 04 v2 (game code run offline vs game).

Next: derive our own implementation of each stage (numpy first, then a compute shader), using the interpreter as a black box to probe each kernel with synthetic inputs (impulse responses for the two blurs, planes/steps for the occlusion kernel) and the listings in `spu-inventory/listings/` for the exact arithmetic. `kernel_run.py` shows how to call a kernel directly from a saved entry state (it needs the output buffer pre-filled for the in-place blur kernels).

## 2026-10-05: four of five SSAO stages reimplemented (`claude-work/ssao-ref/ours.py`)

`probe.py` runs any stage in the interpreter on substituted buffers (z, ao, d24, normals). Findings from probes, then checked on the captured frame (test vector `s-*.npy`; note `s-d24.npy`/`s-A.npy`/`s-B.npy` now come from the stage-1 snapshot, the stage-0 one is stale):
- Both blurs: 5 taps, weights 1,6,10,6,1; each neighbour's weight is multiplied by clamp(1 - 30 * |z_n - z_c| / z_c, 0, 1); result = floor(sum(w*ao)/sum(w)). Row blur alone 91.7% identical / 99.9% within 1; column blur alone 95.6% / 100%; chained from the game's raw occlusion 91.1% / 99.8%.
- Upsample: 2x bilinear with half-pixel centres (impulse response 9/16, 3/16, 1/16), each half-res sample weighted by clamp(1 - 40 * |z_half - z_full| / z_full, 0, 1), truncated. 99.9% within 1 level of the game's final image (64% identical with plain floor; the game lands one level lower on about a third of pixels, floor(v - 0.001) gives 86%).
- Downsample: top-left pixel, 1/z = 0.100000347 - 5.96042646e-09 * d24. Relative error against the game's float buffer is ~2e-5 median but up to 7.7e-3 near the far plane; the game's reciprocal is an estimate plus refinement in float32. Exact arithmetic still to be taken from kernel-03 (184 instructions).
- All four of ours chained around the game's raw occlusion vs the game's final image: 58% identical, 99.5% within 1 level, 99.93% within 4 (image 05; the caption sent to the user said 99.8%, the measured figure is 99.5%).

Raw occlusion stage (kernel-07) so far: tiles of 80x45 output pixels; per tile it GETs a 104x63 window of half-res depth (12-pixel horizontal and 9-row vertical margin) and the tile's full-res normals (160x90); about 155 instructions per output pixel. Probe (flat wall z=600 with single pixels at 0.9*z, normals byte pattern 30,128,128,255): background value 145, the near pixel itself goes to 255, and about a dozen pixels 2-9 rows below it darken by 7-10 (`probe-ao-impulse.pkl`, image 06). So samples lie in a hemisphere whose orientation comes from the normal, and the normals are evidently not in view space with +z toward the camera (a camera-facing wall with that normal reads as half occluded); the kernel gets transform data from the dispatcher (param block at LS 0x3f730 / initial vector registers r8..r18, see `P/frame-pacing/hottest-spu-partial-decompile.txt`). Still to recover: the sample set, the transform, the occlusion falloff, and how the per-frame parameters reach the kernel.

Raw occlusion kernel, additional facts (entry state saved as `ssao-ref/ao-kernel-entry.pkl`):
- Kernel entry is LS 0x6c30, called from dispatcher 0x56b4 with r3 = pointer to a small argument block (LS 0x3f730): +0x00 pointer to the job descriptor copy at LS 0x6380, +0x0c scratch, +0x10 normals tile in LS, +0x14 output tile in LS, +0x1c depth window in LS.
- The descriptor at LS 0x6380 holds the kernel EA and size, and per-buffer entries (EA base, pitch, tile width/height, margins): C1 0x37b08b80 pitch 0xa00 tile 80x45 margins 12/9; normals 0x37400b80 pitch 0x1400 tile 160x90; C2 0x37be9b80 pitch 0x280 tile 80x45. It contains no camera matrices, so the kernel is a pure function of (104x63 depth window, 160x90 normals tile) plus constants inside its own image (the LQR targets at image offsets 0x13f0-0x1470 noted in the earlier decompile).
- Normal encoding as seen by the kernel: byte 2 is "toward the camera" and byte 3 is "screen up" (a flat camera-facing wall given bytes 128,128,255 reads as half occluded with all samples above the pixel; real dock planks are ~(137,164,249), pillars are high in byte 2).
- Idea for recovering the maths without hand-reading 1336 instructions: the main loop is branch-free, so trace one iteration in the interpreter and record the dataflow per output byte as an expression graph in terms of the inputs and constants.

## 2026-10-05: raw occlusion recovered; all five SSAO stages are ours (`ssao-ref/ours.py`)

Method: `ssao-ref/tracer.py` (dataflow tracer on the interpreter) + `aok.py` (direct call of the occlusion kernel on any tile, matches the game within 1 level) + `tr.py`/`cone.py` (print the expression graph behind an output value).

Raw occlusion (kernel-07), per half-res pixel (x, y) with linear depth zc and world-space normal n = bytes 1..3 of the full-res G-buffer texel (2y, 2x) * (2.007843/256) - 1:
- Constants come from the job parameter block fetched from guest memory around 0x00a94990 and stored at LS 0x8110..0x8208: 12 sample unit vectors (SX, SY, SH; SX^2+SY^2+SH^2 = 1, sample 0 = (0,0,1)); a 4x4 float matrix at guest 0x00a94a20 (LS 0x81a0) whose columns are camera right * S, camera up * S, camera forward * R in world space; then 140.86 (= R * sum(SH)), S = 5589.43, 5589.42, R = 15; -1/60, -0.75, 1050.52, 2101.04; 1.2, -0.2. The matrix changes with the camera every frame; the rest looked constant.
- zcl = clamp(zc, 1050.52, 2101.04); s = zc / zcl; c0, c1, c2 = n . matrix columns 0, 1, 2.
- sample i texel: xs = floor(x + 0.5 + (S*SX[i] + c0) / zcl), ys = floor(y + 0.5 + (S*SY[i] + c1) / zcl); nearest texel, no interpolation.
- h_i = SH[i] * R * s; zref = zc + s * c2; d = 0.5 * (z[ys, xs] - zref + h_i)
- contribution = clamp(d, 0, h_i) + h_i * clamp(-d / (60 s) - 0.75 s, 0, 1)
- vis = sum / (140.86 * s); byte = floor(clamp(1.2 * vis - 0.2, 0, 1) * 255). There is no separate mask rule; sky/water come out as 100/101 from the formula.
Against the game's raw buffer: 91.7% identical, 100% within 1 level (with the game's half depth); with our own downsample 91.0% / 99.8%.

Whole pipeline ours (downsample -> raw occlusion -> row blur -> column blur -> upsample) from depth + normals + matrix vs the game's final AO image: 56% identical, 99.3% within 1 level, 99.93% within 4, mean abs error 0.46/255 (image 07). Test vector: `s-d24.npy`, `s-A.npy`, `s-matrix.npy`, targets `s-z/raw/blurh/blurv/final.npy`.

Remaining to reach "SPU program replaced by a GPU one": (1) GLSL passes in RPCS3's Vulkan backend producing the 1280x720 R8 image from the depth and normal render targets; (2) feed the game's sampling of the texture at 0xcf800000 from that image; (3) stop the SPU job doing the work (kernels and its transfers), keeping its job-queue bookkeeping; (4) read the per-frame matrix from guest memory 0x00a94a20; (5) in-game visual and performance check.

## 2026-10-04 (evening): SPU ambient occlusion replaced by a GPU pass — launcher v5

(The section above headed 2026-10-05 was written the same evening; its date is wrong.)

What exists now
- `rpcs3/Emu/RSX/VK/VKNativeSSAO.{h,cpp}`: five full-screen passes (downsample+linearise -> R32F 640x360; raw occlusion; row blur; column blur -> R8 640x360; upsample -> R8 1280x720). Shaders are a direct port of `ssao-ref/ours.py`.
- Trigger: `cached_texture_section::dma_transfer` (VKTextureCache.cpp) calls `native_ssao::on_dma_blit`. The game blits two 1280x720 B8G8R8A8 colour targets to 0x37784b80 (depth packed as A,R,G = high..low byte of d24) and 0x37400b80 (A = material mask, RGB = world normal); when both have been seen in a frame the passes run on those images, on the RSX thread.
- Camera matrix: 16 big-endian floats read from guest 0x00a94a20 at that moment.
- Use: `VKGSRender::load_texture_env` (VKDraw.cpp) swaps the view of the texture at 0xcf800000 (R8, component map from the original view) for the GPU image.
- SPU side (SPUThread.cpp): in `do_dma_transfer`, loads of the five kernels (EA 0x87d500/0x87d880/0x87e480/0x87ec80/0x87f880 to LS 0x6c00) get `BI $lr` written at entry +0x30, so the dispatcher still runs its bookkeeping and transfers but no pixel work; list PUTs into 0xcf800000..+0xe1000 are skipped. Both only once the renderer reports the GPU image is being sampled (`g_native_ssao_gpu_active`), so other renderers/failures fall back to the SPU version.
- `RPCS3_NATIVE_SSAO` bit mask: 1 sample GPU image, 2 validation dumps (`RPCS3_NATIVE_SSAO_DUMP_DIR`, compare with `ssao-ref/gpucmp.py`), 4 idle the SPU job, 8 darken the result (proves which image is on screen). Unset/0 = original behaviour.

Validation (same frame, GPU image vs the SPU image in guest memory)
- Dock, still camera: 98.6% of pixels within 1 level, 99.93% within 4 (48% identical; the SPU's own rounding is one level lower on many pixels).
- Dock, camera rotating: 96.4-98.8% within 1, 99.1-99.9% within 4 per frame. The slightly lower frames may be the matrix being read a frame early/late; not investigated.
- Screenshots original vs GPU at dock and pier look the same (`ssao-ref/09-*.png`, `10-*.png`).

Performance (bin-v24, Strict Rendering off, live.ctl "1000 65536 1 0 0 0 0")
| scene / GPU policy | original | GPU occlusion |
| dock, auto | 44.0 FPS, 44 W | 56.4 FPS, 46.5 W |
| dock, floor-1600 (launcher) | 52.0 FPS, 47.8 W | 59.3 FPS, 47.1 W |
| dock, high | 55.0 FPS, 54.3 W, 6.56 cores | 59.9 FPS, 52.4 W, 5.76 cores |
| pier, auto | 59.9 FPS, 49.7 W | 60.0 FPS, 43.9 W |

Packaging: `build-prep/playable-v5` (= v4 + bin-v24 + `RPCS3_NATIVE_SSAO=5`); desktop entry "inFamous 2 (Optimized)" now points at it. v4 kept as fallback; or set the flag to '0' in `playable-v5/launch.py`.

Not done / ideas
- Only tested at the pier and dock. Addresses (buffers, kernel EAs, matrix 0x00a94a20) are for BCES01143 v1.04 and a 1280x720 G-buffer; anything else falls back to the SPU job automatically only if the blits do not match, not if the matrix address differs.
- The stubbed job still copies its inputs/outputs between guest memory and SPU local store. Skipping those too, and the lighting job (P071), are the next candidates; with lighting on the GPU the two G-buffer readbacks could go away entirely.
- With the dock near the 60 FPS cap on `auto` + ~1300 MHz, the floor-1600 clock helper may no longer be worth its power; re-evaluate.

## 2026-10-04 (late): lighting job (P071) — offline reference working, formula partly read (IN PROGRESS)

Stopped mid-task. Nothing in the launcher changed; playable-v5 is still GPU SSAO only.

Done
- Capture hook `RPCS3_SPU_LIGHT_CAPTURE_DIR` (SPUThread.cpp, `spu_light_capture`): one frame = pre/post dumps of both G-buffers + SPU state (incl. srr0) at every job start. Data: `claude-work/light-ref/cap/` (dock, 91 runs, taken with cfg-spu-interp.yml).
- Every run gets the same inputs: 256-byte record at 0x00a93c80 (+0x10..0x4f 4x4 matrix, +0x90 light count, +0x94 light table pointer, +0x98 intensity scale 1.6) and the light table (8 x 48 bytes at 0x37d6d690; a second table at 0x37d73460 is used on other frames). Tiles are handed out by an atomic counter at 0x00a93a00 (u16 flag, taken, done, total=720).
- `light-ref/light_emu.py frame` emulates the whole frame offline (277 job restarts, 38M instructions, 150 s): A 99.83% bytes identical to the game, B 99.98%, all within 4 levels. Needed in spuemu: ORX, ROTH, BRASL, ORHI, IRET, and SPU float semantics copied from RPCS3's interpreter (exponent-255 values are ordinary numbers in FCGT/FCMGT, FM/FMA corner cases; the job seeds min/max with 0x7fffffff). The SPURS service call at LS 0x1760 is skipped.
- `light-ref/light_trace.py <tile>` + `lcone.py`: dataflow trace per tile with named leaves (A/B pixel, L<n>+offset, P+offset).

Formula read so far from pixel (205,859), one point light (to be verified by a numpy reimplementation):
- world position: homogeneous = M-columns combined with pixel x/1280, y/720 and d24 (P+10.. matrix; d24 from B bytes 0..2), divided by w.
- n = normalize(A bytes 1..3 * 2.00784/256 - 1).
- light record: +00 position xyz (float); colour as half-floats (seen 0x3c00, 0x3486, 0x2caf near +0a/+0e/+12) times P+98; inner/outer radius as half-floats at +20/+22 (655.5 / 1748).
- per light: d = L - pos, dist = |d|, att = saturate((outer - dist)/(outer - inner)), ndl = max(0, n.d/dist), rgb += colour * 1.6 * ndl * att^2.
- encoding of A: v = sqrt(rgb); m = max(max(v) * 0.353553, 0.353553); byte0 = min(255, floor(m*255)); bytes 1..3 = min(255, floor(v * 0.353553 / (byte0 * 1.00392/256) * 255)).
- Not read yet: B byte 0 (specular), lights 6/7 (different record type: +0c differs, likely spot/other), the tile culling test, the character mask (B byte 3).

Next: numpy reimplementation vs `frame-emu.npz`/`cap/post-*`, then GLSL pass + substitute the textures at 0x37400b80/0x37784b80, stub P071, and finally drop the two readbacks.

## 2026-10-04 continuation: lighting reference validated; standalone GPU prototype

Resumed the unfinished P071 reverse engineering. **Playable-v5, emulator source,
game saves, controller configuration and hardware policy were not changed. No
new live FPS result is claimed.** No emulator was running when inspected.

Work and instructions: `B/claude-work/light-ref/README.md`.

- Completed `light-ref/ours.py` for point and spot lights, diffuse/specular
  output and tile zero-fill. The main missing rule was sky exclusion from
  world-space tile bounds and rejection of any collapsed axis. The captured
  frame has 226 active tiles; predicted activity matches all 720 tiles.
- Spot parameters are half values: +0x24 negative cone gain, +0x26 outer
  cosine, +0x28..+0x2c direction, +0x2e minimum distance. Cone attenuation
  multiplies radial attenuation before squaring. Specular uses the captured
  material polynomial and halfway-vector lobe. The tested P071 path ignores
  input B byte 3; this was checked by changing it across a whole tile.
- `python validate.py`: whole captured frame vs the SPU interpreter is
  99.9999729% identical A bytes / 99.9955783% B bytes; **all pixels within 1**
  in both outputs. Against the actual game's captured outputs, A max error 4
  (99.9940321% pixels within 1), B max error 1. All 12 synthetic SPU cases pass
  with max error <=1. Evidence: `light-ref/validation.json`.
- Added `gpu/common.glsl`, `gpu/tiles.comp`, `gpu/lighting.comp`, the standalone
  `gpu/runner.cpp`, and `gpu_validate.py`. Ran on the actual Radeon 780M RADV
  PHOENIX: **GPU output is byte-identical to NumPy for both full-frame buffers**.
  Five additional synthetic GPU cases pass with max error <=1. GPU position
  reconstruction also matches NumPy exactly. Evidence:
  `light-ref/gpu-validation.json`, `gpu-validation-run.log`, `provenance.json`.
- Important arithmetic finding: GLSL constant division can become multiply by
  a rounded reciprocal; this caused a 2-byte spot error in a synthetic case.
  Explicitly rounded tile-coordinate ratios plus `precise` fixed it. Preserve
  SPU reciprocal/rsqrt LUT + FI arithmetic when porting these shaders.

**Still unfinished:** the prototype uses offline uploaded SSBO inputs and is
not integrated into RPCS3. It does not remove readbacks/uploads or idle P071.
Before live substitution, port input access to actual G-buffer images and
establish frame consistency of both images, matrix and light records using
shadow validation. Admission must match supported exact game/program bytes
and keep the original SPU fallback. Preliminary tile frustum/spot-volume
rejection needs broader coverage; the current AABB/per-pixel tests match the
capture and directed cases but do not prove every camera/light boundary case.
The prototype supports kinds 1/2 and up to 32 lights, not arbitrary new jobs.

Next concrete step: integrate **shadow-only** GPU lighting with aligned input,
parameter and output dumps, validate multiple moving-camera/character/lightning
frames, then enable guarded texture substitution and skip pixel work. Drop
the two G-buffer readbacks only after all remaining guest consumers are covered.


## 2026-10-04 live lighting testing: shadow integration; replacement withheld

User explicitly authorized live testing. Added opt-in Vulkan lighting shadow
integration in `S/rpcs3/Emu/RSX/VK/VKNativeLighting.cpp/.h`, generated shader
header, and hooks in VKTextureCache/VKDraw/VKGSRender plus the CMake source list.
The pass uses actual GPU G-buffer samplers. It does **not** substitute textures,
skip P071 work, or remove readbacks; the flag is off by default. Existing source
changes and playable-v5 were preserved.

Evidence: `B/live-light-testing/summary.json`, the four `dock-shadow-*` run
directories, and `B/claude-work/light-ref/README.md`. Latest diagnostic binary:

```text
SHA256 54cf1faa9556e54c3f9994ddc39805bea1e184cc846d1d83712c5372373ae918
```

- Captured **64 full frames**, including stationary dock, camera rotation,
  aiming/firing lightning. Live GPU/reference maximum error is <=1 in A/B
  across all frames (most are identical). Parameters were unchanged at
  consumption in all 64. Additional consume-time light-table snapshots in
  32 frames are byte-identical to the compute inputs.
- Thirteen initial stationary frames have >99.99% A pixels within 1 of the
  game's output and 100% B pixels within 1, but rare A errors reach 5.
  Movement fails stricter admission: frame 013 in `dock-shadow-04` disagrees
  on three active tiles (maximum A error 90, zero vs encoded black); later
  frames have A/B maxima 29/8. Lightning also has errors beyond tolerance.
- Original SPU interpreter replay confirms the tile-5 spot-culling failure:
  the prototype uses actual pixel cone hits to select active tiles, whereas
  the game's preliminary volume selection retains an encoded-black tile.
  Exact preliminary frustum/spot-volume selection remains unfinished.
- Frames 014/015 tile 212 agree with the interpreter within 1, but the real
  game differs up to 28/29 and 8. Fused FMA replay does not explain it. Stable
  consume-time matrices/light tables narrow the next probe to actual live
  SPU DMA inputs/dispatcher state, rather than claiming freshness is solved.
- Corrected `validate.py`'s replay DMA light-table byte count: it always used
  the original eight-light transfer before, invalid for live 30-light tables.
  Reran all 12 directed cases successfully. `live_oracle.py` retains selected
  failing-tile replays, including fused/unfused comparison.
- Full native build passed; Python helpers compile and touched tracked-file
  whitespace checks pass. The wider pre-existing dirty tree has unrelated
  CRLF whitespace warnings. No changes were made to those files.

Tests used `live-light-testing/profile` and exact-binary caches, with only raw
serialized guest SPU bytecode seeded to resume the existing dock savestate.
No native cache objects were imported. Initial launch had a VFS slash error;
the next fresh SPU cache stalled in ancillary code before shadow work ran.
Guest bytecode seeding resolved the latter. Both failures and provenance are
retained. Savestate metadata reports APP_VER 01.00 despite the update code;
future replacement must match **exact loaded job bytes**, not metadata alone.

All test emulator processes were stopped through verified identities. Normal
savedata's 12 files hash-match the original (`save-verification.json`). No new
savestate, controller change, quality reduction, or hardware-policy write was
made; GPU policy remained `auto`. **No performance gain is claimed:** diagnostic
file dumping adds overhead, and replacement did not pass correctness admission.

Next concrete work: trace one failing live tile at its SPU DMA GET to compare
its real bytes and dispatcher state to the GPU snapshot, reconstruct exact
preliminary spot/frustum selection, then repeat moving-camera/lightning shadow
checks. Only after these pass, add exact-job admission and guarded texture
substitution, retaining queue bookkeeping before skipping pixel work. P071's
main tile function is LS 0x4ac8; the job dispatcher entry 0x43d0 must keep its
work-claim/queue operations. Neither entry was stubbed in this continuation.


## 2026-10-04 native lighting fuzzing and G-buffer copy audit

User clarified that small float drift is acceptable; do not reproduce the SPU
instruction stream in production to pursue byte equality. This continuation
uses native geometric and shading formulas, with the interpreter confined to
isolated differential tests. Playable-v5 remains the user launcher; native
lighting replacement and copy removal remain disabled.

Fixed frustum-plane orientation, tile bounding-sphere/spot-cone rejection, and
the near-spot selection rule: minimum distance suppresses shading but does not
exclude an otherwise selected encoded-black tile. Expanded shaders and live
packing from 32 to **64 lights**, with two mask words per tile; lightning was
observed with 59 lights. Regenerated the live shader header and built RPCS3.

Evidence in `B/claude-work/light-ref/`:

- `fuzz-native-02/results.json`: 120 isolated cases pass.
- `fuzz-native-03/results.json`: 420 isolated cases pass, including 42 hardware
  shader cases. All have maximum channel difference <=1 and no structural
  zero/active disagreement. Earlier failed orientation counterexample retained.
- `fuzz-light-counts.json`: 14 point/mixed boundary cases at 1,31,32,33,59,63,64
  pass SPU/native/GPU checks. These exercise both mask words, with mostly
  rejected lights; the dense 31-light oracle trial exceeded its bounded
  instruction budget. Do not call this a dense-64-light oracle proof.
- Full captured-frame and directed shader validation pass (`gpu-validation.json`).

Live comparison now captures actual stock consumer images as well as shadow
outputs/inputs. It writes explicit per-sample metadata and signals completion
only after both consumer-image transfers. Initial corrected input probes
showed no changes in two tiles for 16 accepted pairs, but moving captures
established that readback-time VM blocks can differ from the parameters and
G-buffer bytes used by the actual SPU job. Stable consume-time VM snapshots
alone cannot establish matching job/frame ownership.

The diagnostic-only `RPCS3_NATIVE_LIGHTING_SPU_INPUT_DIR` probe in SPUThread.cpp
captures matching LS GET/PUT bytes for tiles 59,212,502. It preserves normal
transfers, uses no guest register dependency and is off by default.
`analyze_job_tiles.py` matches thread/tile/parameters/light table and all rows:

- `T/dock-shadow-job-inputs-01`: 153 stationary tiles; zero unmatched groups,
  zero active disagreements, diffuse/specular maximum 2/1.
- `T/dock-shadow-job-motion-inputs-01`: 153 camera/lightning tiles; zero
  unmatched groups, zero active disagreements, maximum 4/1, counts up to 57.
- `validate_job_gpu.py` hardware replay of all 153 moving cases also has zero
  active disagreements and maximum 4/1. Every sampled pixel is within four.

These are matched-job kernel checks, **not proof that the current shadow hook
selects the correct generation in every moving frame**. Raw pair-based
comparisons and `actual-tiles-comparison.json` retain mismatched-generation
examples. The next integration step is an immutable job-descriptor/light-table
snapshot linked to the RSX input generation, rather than reading one fixed VM
block at readback. No substitution or P071 skip was enabled.

Copy-removal requirements are documented in detail in
`B/claude-work/light-ref/README.md`, section “Work required to remove the
G-buffer copies.” Both G-buffers are read by P071 and the SSAO dispatcher.
GPU SSAO mode 5 bypasses its five pixel kernels but its dispatcher still copies
G-buffer/intermediate data. Both native hooks currently run inside CPU-driven
readback, so removing that demand also removes their trigger. Move scheduling
to RSX producers, create per-generation input/output ownership with fallback
materialization and completion/lifetime tracking, preserve SPURS claims/tags/
counters, bypass SSAO dispatcher image DMA, then admit native textures before
stock upload. Audit bulk, per-element, direct DMA and unexpected CPU accesses.
Known job traces do not exclude other CPU readers.

The two G-buffer readbacks and lighting/AO uploads total **15,667,200 bytes per
frame** at 1280x720 (~940 MB/s at 60 FPS), excluding CPU/SPU staging/intermediate
traffic and synchronization. The prototype also adds a 14,745,600-byte GPU
position buffer; replace that with local/recomputed positions and parallel
tile reductions before performance evaluation. No FPS gain is claimed.

Latest tested diagnostic binary SHA256:
`416f2bf1bd9e61673577923e66abde1a6833f6a9a2f5f6fa81e2572570e29ac2`.
All test processes stopped through verified identities. Savedata's 12 files
still hash-match the original. No new savestate, controller/quality change or
GPU-policy write; tests used the isolated profile and policy `auto`.


## 2026-10-04 (late night): lighting on the GPU, opt-in (`RPCS3_NATIVE_LIGHTING`) — works at the dock, not in the launcher yet

Playable-v5 and the desktop entry are unchanged. Everything below is in the diagnostic build `B/build/bin/rpcs3`,
SHA256 `62b9b5dbd1d7a1acbafad1734857094eafc2cf43a280a7576e47d9531dfaee8c`, off unless the flag is set.

What exists now
- `RPCS3_NATIVE_LIGHTING` bit mask (VKNativeLighting.h): 1 = the game samples the GPU diffuse/specular images instead of
  the textures at 0x37400b80/0x37784b80, 2 = validation dumps (`RPCS3_NATIVE_LIGHTING_DUMP_DIR`), 4 = the SPU job skips
  its pixel work, 8 = its PUTs into the two images are dropped as well. `5` is parity with `RPCS3_NATIVE_SSAO=5`.
- Parameters come from the job itself: `do_list_transfer` recognises the job manager's input list (first element
  0x00a93c80, 256 bytes) and calls `native_lighting_job_start` with the parameter block and the light table of the
  list's second element. This replaces reading the block at readback time, which was the cause of the moving-camera
  mismatches. One parameter set per frame, about 20 job starts per frame, in every dumped frame.
- RSX side: at the two G-buffer readbacks the source images are copied to private images (the game may reuse the
  targets); the compute passes run when the game first samples either lighting texture, using the latest job
  parameters; the result buffer is copied into two B8G8R8A8 images that are swapped in in `load_texture_env`.
  Shaders are the fuzzed ones, unchanged.
- SPU stub: at each job start the tile function at LS 0x4ac8 gets `BI $lr` (restored when the frame is not
  supported: more than 64 lights, unknown light kind, non-finite values). Checked offline in the interpreter
  (`light-ref/stub_probe.py`): tile claims, row transfers, atomics and the completion label at 0x40300870 are
  unchanged, about 7.5k instead of 45k-245k instructions per tile. Identity = 16 code bytes at 0x4ac8 plus the
  0x4040..0x4140 hash 0x01f9aa7f.

Fuzzing redone on the current shaders (all isolated, `claude-work/light-ref/`)
- `fuzz-native-04`: the existing fuzzer, 420 cases, real GPU on every case: all pass, max difference 1.
- `fuzz_real_frames.py` / `fuzz-real-02`: 105 distinct captured frames (48 cameras, 8..57 lights), 16 tiles each
  = 1680 tiles (570 distinct, 846 lit) against the SPU interpreter: all pass, max 1, no lit/unlit disagreement;
  whole-frame GPU vs NumPy max 1. The oracle needed 0x40300000 mapped: the last tile writes the completion label.
- `validation.json`, `gpu-validation.json`, `fuzz-light-counts.json` regenerated, all pass.

Live results (isolated profile, dock savestate, GPU policy auto; one run each, not a matched A/B)
| `RPCS3_NATIVE_LIGHTING` | still | firing lightning + turning | emulator CPU |
| 0 | 52.7 FPS, 41.5 W | 58.0 FPS | 5.4 cores |
| 5 | 59.7 FPS, 44.6 W | 58.9 FPS | 5.2 cores |
| 13 | 60.0 FPS (cap), 43.1 W | 59.6 FPS | 4.9 cores |
The GPU clock sat near 2200 MHz in the GPU-lighting runs against 1230 MHz in the baseline. 1800 frames per run:
0 frames without a new job start, 0 left to the SPU. Screenshots in `B/live-light-testing/dock-bench-mode*/`.

Open point: sparse pixels in dense lightning frames
- Mode 3 dumps (`dock-replace-0*`, `light-ref/analyze_replace.py`): with a still or rotating camera and up to ~13
  lights the GPU image is within 1 of the game's on >99.98% of pixels, max 2-8. In frames with 20-60 lightning
  lights 10-200 pixels (of 921,600) differ by more than 4, up to 150+, mostly far-away geometry edges.
- Ruled out: frame pairing (`dock-replace-09-seq`: frame F gives 6-121 pixels over 4, F-1/F+1 give 20,000-350,000);
  inputs (`dock-replace-08-rows`: rows as GET by the job are byte-identical to the latched images in all 32
  frames); parameters/light table (one set per frame, list table = parameter-block table); the kernel
  (`dock-capture-lightning-02`: SPU-side pre/post capture of a 31-light frame, reference vs game max 2, 0 pixels
  over 4); XFloat accuracy (`-04-xfloat-accurate` still differs).
- On those pixels the GPU equals the offline SPU interpreter exactly (`light-ref/replace_oracle.py`); the game's
  own output is the one that deviates. Not explained. Next probe: SPU-side post capture and replacement dump of
  the same lightning frame, and a look at tiles in flight when a job yields at LS 0x44bc.

Not done
- Only the dock (and the pier in passing). Nothing but the GPU path was exercised: the unsupported-frame fallback
  never triggered live.
- The stock readback and (in mode 5) the stock upload still happen; the private G-buffer copies and the 14.7 MB
  position buffer are extra GPU work. Removing the readbacks is the next stage (see "Work required to remove the
  G-buffer copies" in `light-ref/README.md`); both GPU passes are still triggered by the readback.
- Launcher: a v6 would be v5 + this binary + `RPCS3_NATIVE_LIGHTING=13`. Not packaged; the binary also carries
  the diagnostic probes.

Tools added: `B/live-light-testing/replace_run.sh` (motion/lightning capture), `replace_bench.sh` (FPS/CPU),
`live_test.py` now takes `NAME=VALUE` and `CFG:setting=value` arguments; `RPCS3_SPU_LIGHT_CAPTURE_MIN_LIGHTS`
delays the SPU-side capture until a frame has that many lights; `RPCS3_NATIVE_LIGHTING_DUMP_SEQUENCE=first,count`.
The `dock-replace-*` dump directories total about 7.5 GB and can be deleted once the open point is settled.
Savedata: the 12 files match `original-save-hashes.json` in both the real and the test profile. No savestate,
controller, quality or GPU-policy change; all test emulators stopped.

Uncapped numbers (same night): `replace_bench.sh <label> <mode> "CFG:Frame limit='Off'" "CFG:Vblank Rate='120'"`
(the game syncs to vblank, so Frame limit Off alone stays at 60; benchmark only, game speed at 120 not checked).
Dock, GPU policy auto, two runs each, interleaved:
| `RPCS3_NATIVE_LIGHTING` | still | firing lightning + turning | GPU clock | power |
| 0 | 54.5 / 54.4 FPS | 65.0 / 65.7 | ~1260 MHz | 42 W |
| 13 | 70.0 / 70.4 FPS | 69.2 / 70.0 | ~2245 MHz | 49 W |
The auto governor leaves the GPU near 1260 MHz in the baseline and raises it to ~2245 MHz with GPU lighting, so
part of the difference is clock, not work removed.

Same uncapped bench with the GPU held at `high` (2700 MHz, 54 W) for both modes, two runs each, interleaved;
policy restored to auto afterwards:
| `RPCS3_NATIVE_LIGHTING` | still | firing lightning + turning | emulator CPU |
| 0 | 62.1 / 63.1 FPS | 67.6 / 68.4 | 5.9 cores |
| 13 | 73.1 / 71.3 FPS | 74.2 / 73.5 | 5.8 cores |
At equal clock GPU lighting is worth about 10 FPS still (+15%) and 6 FPS under lightning (+9%); the rest of
the 16 FPS seen on `auto` was the governor raising the clock.

## 2026-10-05: playable-v6 packaged; G-buffer readbacks removed (`RPCS3_NATIVE_LIGHTING` bit 16) — playable-v7

Launchers
- `B/playable-v6` = v5 + binary `62b9b5db…` + `RPCS3_NATIVE_LIGHTING=13`. Desktop entry "inFamous 2 (Optimized)" points at
  v6 (one-line change of the `Exec` path to go back to v5).
- (Desktop entry switched to v7 on request, later the same day.)
- `B/playable-v7` = v6 + binary `639d07c1ad1b21352314f306acf3f6e45f857369d917519a676451eb92f01c86` +
  `RPCS3_NATIVE_LIGHTING=29`. Initially not wired to the desktop entry; subsequently selected, then superseded by v8. Both launchers accept `RPCS3_PLAY_PROFILE=<dir>` to run
  against another profile (used for the smoke tests; the real profile was not started).
- Both smoke-tested through `launch.py` on the isolated profile (real boot, swamp pier scene): 60 FPS, GPU passes active.
  `--no-gui` does not exit on SIGTERM within 20 s; the tests end with SIGKILL after the identity check.

How the G-buffers actually travel (corrects earlier notes that called them surfaces at 0x37400b80/0x37784b80)
- The game renders them to local-memory targets (0xc0e3c000 normals, 0xc0010000 depth) and copies each to main memory
  with two NV3089 blits: 1024x720, then the remaining 256x720 (dst +0x1000). In RPCS3 that is a `blit_engine_dst`
  section; the texture cache predictor then reads it back right after the second blit, on the RSX thread.
- The lighting job's 128-byte rows and the occlusion job's 640/5120-byte rows are list GETs. The occlusion job's scratch
  buffers start exactly at the end of image B (0x37b08b80), i.e. in the same page: its first scratch PUT faults and forces
  a readback of B even when nobody reads B.
- **The game samples the occlusion job's half-resolution linear depth (C1, 0x37b08b80, 640x360 X32_FLOAT) as a texture
  when lightning/particles are on screen.** With the job's kernels stubbed (v5, v6) that buffer is no longer produced.
  Lightning screenshots of v6 against stock show no obvious damage (`live-light-testing/shots-stock`, `shots-v6`), but the
  data was wrong. The new build gives the game the GPU pass's `z_half` image instead (any `RPCS3_NATIVE_SSAO` with bit 1).

What changed in the source
- Trigger: `vk::texture_cache::blit` (VKTextureCache.cpp) calls `native_ssao::on_gbuffer` / `native_lighting::on_gbuffer`
  (renamed from `on_dma_blit`) when the blit piece that completes a 1280x720 image at either address arrives. The passes
  no longer depend on a readback happening. `dma_transfer` only counts readbacks now (`native_lighting::on_readback`).
- Bit 16 (needs bits 4 and 8 and `RPCS3_NATIVE_SSAO` bit 4): `native_gbuffer_list_skip` (SPUThread.cpp, called at the top
  of `do_list_transfer`) leaves out list elements of the lighting job (GET, the two images) and of the occlusion job
  (GET and PUT, images plus scratch up to 0x37c22480). Jobs are recognised by the 0x4040..0x4140 hash. The flag
  `g_native_gbuffer_unread` is decided at every lighting job start, so an unsupported frame goes back to real transfers,
  faults, and gets a normal on-demand readback. While the flag is set the speculative readback after the blit is skipped.
- Half-res depth: `native_ssao::substitute` covers 0x37b08b80; `load_texture_env` (VKDraw.cpp) reuses the descriptor of the
  first substituted binding so that later bindings do not go through `upload_texture` (which would read the shared page).
- `native_lighting_job_start` logs why a frame is left to the SPU (first 8).
- Offline: `light-ref/skip_get_probe.py` — the stubbed lighting job's transfers, instruction count and non-image memory are
  identical with real, random, zero and 0xff G-buffers (6 tiles), so it does not look at the rows it loads.

Checks
- New trigger, SPU still doing the work (mode 3, `dock-blit-01/analysis.txt`, 23 frames, 6-49 lights): same picture as with
  the old trigger — frames up to ~11 lights within 1-6 levels, dense lightning frames with the known sparse outliers.
- Mode 29, dock: 4 readbacks during start-up, then none standing still; one more when lightning first shows the half-res
  depth texture; about 24 MB of SPU transfers skipped per frame. Screenshots while firing look like stock
  (`shots-stock`, `shots-nocopy`).
- Frames with more than 64 lights fall back to the SPU (69 seen while firing): 10-11 of ~1800 frames in mode 29, 25-32 in
  mode 13 at uncapped speed. Each costs two readbacks. This fallback was reported as "never triggered" before; that was
  only true of the capped runs. Raising the limit means 4 mask words per tile and a wider parameter block, then re-fuzzing.

Performance, dock, uncapped (`Frame limit Off`, `Vblank Rate 120`), GPU held at `high` (2700 MHz, 54 W), interleaved;
policy restored to auto afterwards
| `RPCS3_NATIVE_LIGHTING` | still | firing lightning + turning | emulator CPU |
| 0 (SPU lighting, GPU occlusion) | 62.4 FPS | 69.9 | 5.9 / 6.2 cores |
| 13 (v6) | 73.4 / 72.8 | 75.1 / 74.5 | 5.8 cores |
| 29 (v7) | 85.0 / 84.6 | 87.6 / 87.0 | 6.0 / 6.4 cores |
Capped at 60 on `auto`: mode 29 holds 60.0 still at ~1720 MHz, 45 W, 4.3 cores (mode 13 the night before: 43 W, 4.9 cores,
~2200 MHz); firing averages 58.4-58.5 with a one-off dip at the first bolt (half-res depth binding learned, one readback).

Not done
- Only the dock savestate and the swamp pier boot scene. No unknown reader of the two images showed up there (it would
  appear as a growing readback count in the `GPU lighting:` log line every 1800 frames).
- The blits themselves (GPU to GPU), the private latch copies and the 14.7 MB position buffer remain.
- The occlusion matrix is still read from guest memory when the G-buffer arrives (unchanged; see the 2026-10-04 evening note).
- Sparse-pixel question in dense lightning frames: unchanged, still open.

Tools: `live-light-testing/build.sh` (fails loudly instead of leaving a stale binary), `lightning_shots.sh`,
`LIVE_TEST_BINARY=<path>` for `live_test.py`, start timeouts in `replace_run.sh` / `replace_bench.sh`.
Savedata: 12 files match `original-save-hashes.json` in the real profile. The launcher smoke tests autosaved into the
isolated profile's slot 0; those two files were restored from the real profile and match again. No savestate,
controller or quality change; no emulator running; GPU policy `auto`.


## 2026-10-05: tested 256-light GPU program integrated — playable-v8

User confirmed they had tested the output, accepted it, and requested integration.
The 256-light program was already implemented in the source and diagnostic build;
the desktop was still running the 64-light `playable-v7` binary. Packaged the exact
previously tested binary as `B/playable-v8/bin/rpcs3` and switched the existing
**inFamous 2 (Optimized)** desktop entry to `B/playable-v8/launch.py`.

- Binary SHA256: `8efa6ac5245166dd415056822b28f6964e02d32690c2d4e9410beee855de4097`.
- `VKNativeLighting.cpp`: `max_lights=256`, parameter storage scales to 256 records,
  and the tile-mask buffer holds eight 32-bit words per tile.
- `light-ref/gpu/common.glsl` and generated `VKNativeLightingShaders.hpp`:
  `MASK_WORDS=8`; culling and shading use all eight words. This supersedes the
  historical 64-light limit and fallback notes above. Counts above 256 and
  unsupported records still use the existing SPU fallback.
- Launcher retains `RPCS3_NATIVE_LIGHTING=29` and `RPCS3_NATIVE_SSAO=5`, GPU
  lighting/occlusion, skipped image transfers, and GPU half-resolution depth.
  The v7 configuration, live controls, profile/cache selection and hardware-policy
  behavior are preserved. Packaging performs no hardware-policy changes.
- `B/playable-v8/verified-runtime.json` records binary/config hashes, settings and
  evidence. The v8 launcher checks its binary hash before starting the emulator.

Existing evidence (not new tests): `B/live-light-testing/dock-256-lights/provenance.json`
identifies this exact binary. Its `replace-comparison.json` has seven real frames,
65–72 lights, zero active-tile disagreements, and GPU-versus-native-reference
maximum differences of 1/0 for A/B. It retains the previously documented sparse
GPU-versus-SPU outliers (aggregate maxima 189/48); user visual acceptance does not
mean byte-perfect equivalence. `B/claude-work/light-ref/fuzz-light-counts.json`
records passing oracle/GPU cases at counts 1,31,32,33,63,64,65,127,128,129,255,256.

Packaging verification: copied binary matches the tested SHA256; both game
configuration files and `live.ctl` are byte-identical to v7; launcher syntax and
desktop target verified. No gameplay session was started or restarted for this
integration; saves and controller files were not touched. The next launch from
**inFamous 2 (Optimized)** uses v8. No new FPS claim.

Rollback: restore the desktop `Exec` path to `B/playable-v7/launch.py` (the full
previous desktop entry is retained as `B/playable-v8/infamous2.desktop.previous`).


## 2026-10-05: first city slowdown profile and savestate crash evidence

Evidence: `B/city-profile-20261005-020637/` (`findings.txt`, `comparison.json`,
`stationary-city/`, `faster-city/`, `rsx-callchains.json`, crash log/core metadata,
normal savedata backup).

At the same character location, user selected a slow city view and then a faster
camera view. Two focused 30-second captures: **43.86 vs 59.95 FPS**; RSX rendering
thread **98.92% vs 75.91% of one logical CPU**. GPU busy ~69.7/69.5%, clocks
~1995/1675 MHz. This is a scene comparison, not an optimization gain or matched
clock benchmark. Title FPS is sampled; individual frame-time tails were not measured.

Both GPU replacements active; through 10800 frames, **0 lighting fallback frames,
0 missing job starts, 4 total G-buffer readbacks**, unchanged since startup.
The sampled city slowdown is not explained by the old 64-light fallback or
recurring G-buffer readbacks. Main PPU and five primary SPUs were not saturated.

Strongest lead: host CPU draw preparation/submission. A 15-second RSX call-chain
follow-up places ~73.8% under draw-end processing, ~22.1% under vertex upload,
~12.4% under texture setup, ~11.3% under vertex memory copies (inclusive,
overlapping percentages). Next target: per-frame draw/upload volume and cache
reuse/layout/setup work; no single guest kernel is established as the cause.
No optimization/configuration changes made; game left running for user.

Savestate attempt did crash: the prior process was **v7**, with SIGSEGV on SPU
thread immediately after the logged savestate selection. Normal save write
completed shortly beforehand; no newly completed savestate found. Unsymbolized
JIT core trace does not establish root cause. Preserved crash log and core metadata,
backed up normal saves, reopened v8 normally, and made no further savestate attempt.

Save check: slot-0 `STATE` changed during the reopened session (game autosave);
all other savedata hashes match. Prelaunch backup retained, no saves restored.


## 2026-10-05: remaining city draw/vertex/texture profile

Evidence: `B/city-profile-20261005-020637/remaining-findings.txt`,
`remaining-profile-summary.json`, `detailed-slow-city/`, `overlay-slow-city/`,
`live-control-comparison/`, `repeat-submit-comparison/`. Same v8 process,
user returned to slow view. Focused 30-second RSX DWARF capture: **44.62 FPS,
98.79% of one logical CPU on RSX**. Through 52200 frames: zero lighting fallbacks,
zero missing job starts, four startup G-buffer readbacks.

Eight sampled overlay frames: **10259 draw calls/frame**, 24 submits; CPU vertex
preparation **5.17 ms**, draw setup **3.20 ms**, texture phase **2.83 ms**, draw
execution **1.29 ms**, submit/flip **0.26 ms**. Vertex phase includes layout/index
work, not only copies. **5872 texture requests with only 1 CPU texture upload**;
vertex cache **37.0% hits**, program lookup elision ~43.4%. These are sampled
internal CPU phases, not whole GPU timings or a continuous frame trace.

Call paths: vertex/index preparation 19.9%, vertex environment/layout 9.1%,
texture handling 14.6%, program/pipeline setup 8.9% of RSX user-cycle samples.
Inclusive details: vertex data writing 10.3%, input-layout analysis 3.0%,
layout-state filling 2.6%, program binding 7.3%, fragment search 2.9%.
Inclusive values overlap; FIFO/local-task samples include active processing.

Short live controls, original values restored:
- Forced same-frame multiblock cache ABBA: 44.916 -> 45.367 FPS, +1.00%; within
  comparable baseline/scene variation. Not promoted.
- 2 ms periodic submission ABBA: 44.317 -> 45.308 FPS, +2.24%; reversed BAAB:
  44.371 -> 45.001 FPS, +1.42%. Small directional lead; broader motion/image
  validation required before promotion. Launcher retains **1000 us**.

First code target: guarded reuse of decoded vertex-input layouts/templates.
`VKDraw.cpp::emit_geometry` currently forces full layout analysis for each first
subdraw. Separate stable format/layout from changing address/range parameters;
retain full invalidation and original fallback. Analysis alone has a small ceiling;
repeated copy-byte identity in the city is not yet measured. Existing range-result
cache and earlier negative dock cross-frame cache results remain relevant.

Next: trace stable texture/descriptor identities and resource generations to
reduce repeated lookup/set-update work; existing pipeline/descriptor bind caches
already suppress exact unchanged state. Fragment-address memo and submission
spacing are smaller candidates. Larger gains require reducing per-draw work or
identifying a GPU-resident geometry producer; no specific guest kernel proved yet.
Approximately 5.7 ms must leave the limiting path to take 44.6 FPS to 60;
these tuning comparisons do not demonstrate that reduction.

No restart/savestate, quality, controller or hardware-policy change in this
follow-up. User turned debug overlay off; binary/config hashes unchanged,
`live.ctl` restored byte-for-byte. Game left running.


## 2026-10-05 (midday): static geometry kept on the GPU across frames — playable-v9 (packaged, desktop entry still v8)

Tools, sessions and the file list are in `B/geom-testing/README.md`. New savestate: `claude-work/states/city.SAVESTAT.zst`
(the user's city checkpoint, made on the isolated profile with the compat settings). The slow street view of the previous
two sections is about 60 degrees to the right of the spawn camera (`geom-testing/heavy.sh`).

What the city draws (`RPCS3_VK_GEOM_TRACE`, `geom-testing/city-boot-01`, `city-trace-02`, 6 frames each)
- 8,700-10,700 draws per frame; about 5,700 are depth-only shadow-map draws (target 0xcf2f0000) with a 6-byte position stream;
  median 46 vertices per draw. 11.7 MB of vertex data and 3.75 MB of index data are requested per frame.
- Against the previous frame: 83% of the vertex bytes (65% local memory, 18% main memory) and 8,291 of 8,711 index buffers
  are at the same address with the same bytes. About 17% (2 MB, local memory) is new every frame.
- So in the city the copies were mostly redundant. The dock is different: its geometry is rewritten constantly (this is
  why the unsafe cross-frame bound measured there on 10-04 was +0.6%).
- The command stream: one PUT update per frame, ~220 calls and ~275 jumps per frame, 3 semaphore acquires, no jump-to-self.

What was built (`RPCS3_VK_GEOMETRY_CACHE` / live control 8: 0 off, 1 on, 2 on + content check of every reuse)
- A vertex source (the ordered guest ranges of a draw, up to 2 blocks) or index source (range + type, primitive, restart)
  that is requested again in a later frame is copied once more into a persistent buffer (64 MB vertex with a texel view,
  32 MB index) and used from there. Index entries also keep min/max/count, so the conversion pass is skipped.
- Validity comes from kernel write tracking: userfaultfd asynchronous write-protect plus `PAGEMAP_SCAN` (Linux 6.7+,
  works unprivileged), on both guest views (base and sudo), 1 MiB chunks, only where something was promoted. Measured in
  a standalone test: 0.6 us per first write to a page, ~50 us to scan 256 MB, kernel-mode writes are tracked too.
  Memory mapped at more than one guest address is refused; a remapped chunk fails the scan and is dropped.
- Scans are lazy: on the first lookup after the RSX thread passed a point where it waits for the guest (new PUT, empty
  FIFO, jump-to-self, semaphore acquire, before and after the wait). About 2 scans per frame, 0.3-0.45 ms per frame.
- Memory that keeps changing is left alone: 3 invalidations stop a source for 600 frames, 8 invalidations in 600 frames
  stop a 64 KiB block for 1800 frames. A full persistent buffer is replaced (old one through deferred disposal), at most
  every 300 frames. Without the kernel feature nothing is promoted and behaviour is the previous same-frame reuse.
- Also changed: `rsx::thread::do_local_task` tests the backend-interrupt bit before the locked clear (no measurable effect).

Results (AC, GPU policy auto, playable-v8 settings, isolated profile, interleaved live switching, 10 s arms)
| scene | cache off | cache on | |
| city, slow street view, uncapped (Frame limit Off, Vblank 120) | 46.1 FPS, 43.4 W | 51.6 FPS, 47.0 W | +12% |
| same view, normal 60 cap | 46.6 FPS, 43.1 W | 51.8 FPS, 46.7 W | +11% |
| city, spawn view, uncapped (earlier build of the day) | 52.5 | 57.9 | +10% |
| dock, uncapped | 82.2 FPS, 51.6 W | 82.8 FPS, 51.9 W | +0.7% |
In the city 96-98% of vertex and index requests are served from the persistent buffers (12-16 MB per frame not copied).
The RSX thread stays at 99% of a core in both arms, and the emulator uses about 0.4 core more with the cache on (more
frames for the SPU/PPU side). The extra 3.5 W goes with the extra frames and a higher GPU clock.

Checks
- Mode 2 compares a hash of the current guest bytes with the hash taken at copy time, on every reuse. Final build:
  city roam ~110 s (about 67 million reuses) and dock with lightning ~40 s: 0 mismatches. Earlier builds on AC: 0 in ~8 min.
- On battery, earlier the same morning, 3 mismatches in about 85 million reuses (one 20 KB vertex source, two 36-byte
  index buffers). The two that were classified were late tracked writes: the guest rewrote the memory after the last
  synchronisation point while a submitted draw still referenced it, and the next scan saw the write. In that situation
  the cache draws the older bytes for that draw and stock draws whatever is in memory at draw time. Cause not established
  (looks like the game recycling a mesh's memory while the RSX thread is more than a frame behind); it did not recur on
  AC, including a run with the post-wait semaphore point disabled, so that addition is not what removed it.
- Not checked: image comparison against stock (NPCs move; only the hash check was used), long play sessions, areas other
  than this part of the city and the dock, the persistent-buffer replacement under heavy streaming (seen once per ~1-5
  minutes while roaming, no visible effect in screenshots), kernels without the feature (code path only).

Packaging
- `B/playable-v9` = v8 + binary `1483c046263b151f9ad9f08c591c35c5e0c960aaebb6b7a664d560bc4d32c780` +
  `RPCS3_VK_GEOMETRY_CACHE=1`. Same config and live.ctl as v8. Smoke-tested through its `launch.py` on the isolated profile
  only as far as the intro logos (cache active, GPU policy back to auto on exit). The desktop entry still points at v8;
  switching is the one `Exec` line (current entry saved as `playable-v9/infamous2.desktop.previous`).

What is left on the RSX thread (flat profile at the slow view with the cache on, `geom-testing/perf-view60-b`)
No single item above 5%: FIFO parsing ~12% in total, cache lookups 4%, `do_local_task` 4.4% (one load that stalls;
possibly a cache line shared with another thread), texture descriptor lookups ~8%, layout analysis and vertex env ~6%,
program lookup ~4%, RADV 13%. About 10,700 draws at 1.8 us each. Reaching 60 at this view needs roughly another 15%:
that means fewer draws reaching the per-draw path (for example recognising the shadow-map pass, whose 5,700 draws
differ only in vertex source, index range and matrix, and submitting it with per-draw data in one buffer), not a
faster copy.

State at the end: no emulator running, GPU policy auto, real savedata hashes equal to `geom-testing/real-save-hashes-before.txt`.
The isolated profile's slots 0 and 4 now hold copies of the real ones.


## 2026-10-05 (afternoon): shadow-map pass — fast repeat draws, playable-v10 (packaged; desktop entry is v9)

The desktop entry was switched to `playable-v9` on request (previous entry in `playable-v9/infamous2.desktop.previous`).

Question asked: reimplement the shadow-map pass "in a shader" to save draw calls. What the measurements say first.
- Per-pass RSX-thread time (`RPCS3_VK_PASS_TIMING=1`, `VKPassTiming.hpp`; slow street view, cache on, about 17.5 ms per frame):
  shadow maps (depth-only target 0xcf2f0000, three cascades) 5.3-5.7 ms for ~5,700 draws = 0.94-1.0 us per draw;
  G-buffer (0xc0ab8000) 8.4 ms for ~2,200 draws = 3.8 us per draw; 0xc11c0000 1.6 ms for ~550 draws.
- Inside a shadow draw: FIFO parsing 0.30, state/program/environment 0.11, vertex+index lookup 0.29, layout entry and
  binds 0.10, Vulkan draw call and bookkeeping 0.19 (each figure includes ~0.025 us of timer). The draw call itself is the
  small part, so replacing 5,700 draw calls with one indirect call would not have paid by itself.
- Bound: dropping the depth-only draws after parsing (live control 9 = 1, experiment only) takes that view from ~50 to
  ~61-63 FPS, i.e. everything after FIFO parsing in the shadow pass is worth at most about 25%.
- 95% of the shadow draws use one vertex program and a 6-byte position stream. Between two draws the command stream is
  almost always: [SET_TRANSFORM_CONSTANT_LOAD=0x100 + 16 words] [SET_VERTEX_DATA_ARRAY_OFFSET] SET_INDEX_ARRAY_ADDRESS+DMA,
  BEGIN(triangles), DRAW_INDEX_ARRAY x1..3, END (`geom-testing/fifo_shapes.py`, `shadow_stats.py`, `shadow-trace-02`).
- RPCS3's generated vertex shader already reads all per-draw data from a `draw_parameters[]` buffer entry selected by one
  push constant, so the game's own shader can be reused as is; no hand-written shadow shader was needed.

What was built (`RPCS3_VK_FAST_DRAWS` / live control 9: 0 off, 2 on, 3 = off but checked; code at the end of `VKDraw.cpp`)
- After a draw has gone through the complete path, `fast_draw_batch` reads the following commands from the FIFO itself.
  Setup commands from a short list (vertex array offsets, index array address/DMA, transform constant load and values) are
  applied the way their handlers apply them outside BEGIN/END. A BEGIN..END with only DRAW_INDEX_ARRAY inside is then
  drawn with the program, pipeline, descriptors, render pass and dynamic state that are already bound, using the regular
  vertex/index upload (geometry cache) and layout-entry code. Any other command ends the run with the FIFO on that command.
- A draw is only taken when nothing is pending: no dirty state bits other than ones a draw does not act on (list and
  reasons in `fast_draw_blocker`), same command buffer, render pass open, no dynamic-state reload, no conditional
  render or pending occlusion task, FIFO flattener off, no instancing or programmable blending, sampled textures plain
  (not render-target backed, not cyclic) and not dirty. If the upload flushes or a ring buffer is replaced mid-draw, that
  draw goes through the complete path.
- Related small changes: `load_program_env`'s transform-constant block is now `update_transform_constants_buffer`;
  `FIFO_control::peek/fast_forward`; `rsx::thread::end` sets the backend-interrupt bit only when clear; the geometry cache
  prefetches the slots that last frame's lookups used (`sequence_t`).

Results (AC, GPU auto, playable settings, interleaved live switching, 10 s arms; view = slow street view)
| scene | v8 behaviour | + geometry cache (v9) | + fast draws (v10) |
|---|---|---|---|
| city slow view, uncapped | 46.4 FPS, 46.4 W | 51.2 FPS, 49.7 W | 56.3 FPS, 52.9 W |
| city slow view, 60 cap | 46.7 FPS, 46.0 W | 50.6 FPS, 48.2 W | 55.3 FPS, 51.0 W |
| dock, uncapped | 82.7 FPS | 85.2 FPS | 87.3 FPS |
About 6,400 of ~9,600 draws per frame take the fast path at that view, in ~3,200 runs: a run lasts two draws on average.
Runs end mostly at texture setup (0x1a00.., G-buffer pass), polygon-offset toggles (0xa68, ~700 per frame in the shadow
pass), texture semaphore releases (0x1d6c) and vertex format changes (0x1740). CPU reaches 84-85 C in these runs.

Checks
- Mode 3 leaves everything on the complete path and, for each draw the fast path would take, compares program, pipeline
  properties, command buffer / framebuffer / render pass, environment offsets and buffers, sampled texture views, state
  bits and the dynamic-state flag before and after: city roam ~4,000 qualifying draws per frame and dock ~1,600-2,200 per
  frame, 0 differences of any kind (`fast-verify-02`, `fast-verify-dock`). An earlier run flagged command-buffer changes;
  that was the check sitting after the periodic submit, moved since.
- Geometry content check (cache mode 2) together with fast draws, city roam: 67 million reuses, 0 mismatches (`soak-01`).
- Screenshots alternating off/on at one view (`fast-compare`): off-vs-on differs as much as off-vs-off (moving characters,
  clouds, HUD); static geometry and shadows are black in the difference images.
- Not checked: the fast path's own register writes against the handlers by an automatic comparison (they are few and were
  read side by side); scenes with occlusion queries, conditional rendering or MSAA (the fast path refuses the first two);
  long sessions; other areas of the game.

Packaging: `B/playable-v10` = v9 + binary `f1abd7d8a3e04a8ef83c9d07308367e2b4122ce621a754136d2608738271f8c4` +
`RPCS3_VK_FAST_DRAWS=2`. Smoke-tested through its `launch.py` on the isolated profile to the title screen (fast draws
active, GPU policy back to auto). Desktop entry unchanged (v9).

What would come next, by size
- G-buffer pass: ~8.4-10 ms per frame at 3.2-3.8 us per draw, of which FIFO parsing ~1.0 and texture/program state ~0.9-1.1.
  Texture descriptor revalidation runs for every sampled texture of every draw (`load_texture_env`), with ~5,900 requests and
  one real upload per frame.
- Shadow pass: letting the fast path handle the polygon-offset toggles would roughly double run length there; per fast
  draw the remaining costs are the two cache lookups, input-layout analysis and the 168-byte layout entry.
- `load_program` never clears `pipeline_config_dirty` when re-evaluation finds the same pipeline, so the full pipeline
  state is re-derived for every draw of the complete path from then on. Clearing it there looks right but changes stock
  behaviour for every game; not touched.


## 2026-10-05: G-buffer state setup — playable-v11 (installed)

Details and exact patch: `B/geom-testing/gbuffer-implementation/README.md`,
`gbuffer-optimization.patch`, `source-sha256.json`; city/dock verification logs in the sibling
`gbuffer-verify-city`, `gbuffer-pipeline-verify-city`, `gbuffer-final-ab-city`, `gbuffer-verify-dock` folders.

Enabled `RPCS3_VK_PIPELINE_REUSE=1` (live control 11): after a complete state decode proves the pipeline
properties unchanged, clear pipeline_config_dirty so following complete-path draws can reuse the decoded
state. Shader-invalidated and interpreter paths retain previous behavior. Mode 2 re-decodes every clean
reuse and compares properties. City + dock: 6,671,861 pipeline comparisons, zero mismatches.

Also implemented a guarded cache of completed compressed-material image/sampler bindings
(`RPCS3_VK_MATERIAL_BINDINGS`, live control 10; 1 reuse, 2 full lookup + checks), bounded to 4096 entries
and expired by texture cache, section destruction, surface cache and sampler generations. Cyclic/deferred/
render-target descriptors are excluded. Sampler references are retained and released correctly.
32,856,385 reported material comparisons across city and dock runs, zero mismatches. Dock also checked
18,196,482 geometry reuses, zero mismatches. Material reuse is **off by default**: no reliable FPS benefit.

At the heavy view (city save, roughly 60 degrees right, fixed camera within arms), pipeline-only ABBA/BAAB
with four 10-second arms each measured 56.4275 FPS off vs 56.675 on (+0.44%, within variation); GPU auto,
AC, uncapped, v10 fast draws and geometry cache active. Earlier material-only comparison +0.34%, at a
different boot/camera alignment. No demonstrated speedup or new 60 FPS claim.

`B/playable-v11` binary SHA256 `29780f3691e9dbdac7471fbc53977c4182dd97fa957df957d70c5c6b5fa0c01a`;
configs/live.ctl byte-identical to v10. Exact launcher rendered intro logos with the isolated profile;
that smoke process required forced test termination, so that check did not validate normal game exit.
Optimized desktop entry switched from v10 to v11; previous entry in `playable-v11/infamous2.desktop.previous`.
Real savedata hashes unchanged, isolated test controls restored, no emulator running, GPU policy auto.
Long sessions, other regions/titles and forced allocation failures remain untested.


## 2026-10-05 (later): texture and polygon offset changes inside fast runs — playable-v12 (packaged; desktop entry is v11)

Starting point: v11 (pipeline reuse on, material bindings off) measured +0.4%, inside variation, and its log shows the
fast-draw counts of v10 unchanged (about 6,460 draws in 3,100 runs per frame). Runs were ended per frame by: fragment
texture setup of slot 0 (0x1a00) 1,670, polygon offset enable (0xa68) 750-780, texture read semaphore (0x1d6c) 250-280,
vertex format (0x1740) 160, other texture slots 170. Each ended run sends the next draw through the complete path.

What was built (end of `VKDraw.cpp`; patch in `geom-testing/texture-fast-path/`)
- `fast_draw_batch` passes fragment texture commands (the eight words per slot, CONTROL2, CONTROL3) and the three polygon
  offset registers to their regular handlers, exactly as the FIFO loop dispatches them.
- Before a draw with texture state pending, `fast_draw_rebind_textures` runs the regular `load_texture_env`, then
  `get_current_fragment_program`, and compares everything that selects the shader besides the microcode (ctrl,
  texture_state, texcoord mask, two-sided lighting, MRT count). Equal: new texture parameter block
  (`update_fragment_texture_params_buffer`, extracted from `load_program_env`) and the regular `bind_texture_env`.
  Different, a texture that is not a plain uploaded one, a closed render pass or another command buffer: the draw goes
  through the complete path with the state bit set again.
- Polygon offset is dynamic state and the pipeline always has depth bias enabled, so a pending polygon offset bit is
  answered with `set_depth_bias_state` (extracted from `update_draw_state`) right before the draw.
- The run blocker also refuses programs with emulated depth compare and textures assembled per draw (no view of their own).
- Live control 9: 4 = on with a full program lookup after every texture change inside a run, compared with the bound
  program; 5 = on with the earlier scope.

Results (on battery, CPU 92-101 C and thermally limited in every arm; one boot per row, arms switched live, 10 s each)
| slow street view | fast draws off | v10/v11 scope (5) | v12 scope (2) |
|---|---|---|---|
| uncapped, 3 reps | 50.5 FPS, 47.8 W | 55.1 FPS, 51.1 W | 59.0 FPS, 53.9 W |
| 60 cap, 2 reps | 49.4 FPS, 45.1 W | 52.5 FPS, 45.8 W | 56.3 FPS, 45.0 W |
Fast draws per frame at that view: about 8,500 in 1,050 runs (v11: 6,460 in 3,100), of which about 1,500 with textures set
up again and 640 depth bias updates. No draw needed another shader variant after a texture change in any run.
Per-pass timing with mode 4 active (so including the check): G-buffer 3.08 us per draw, earlier 3.64-3.70.
Not measured on AC, and the absolute numbers of this section are not comparable with the earlier AC tables.

Checks
- Mode 4 with geometry content checks and pipeline reuse checks, city roam: about 2.5 million texture-change program
  lookups, 7.2 million geometry reuses, 0 mismatches of any kind (`tex-verify-city`); dock: about 0.9 million lookups,
  3.2 million geometry reuses, 0 mismatches (`tex-verify-dock`).
- Screenshots alternating off/on (`tex-compare`, `tex-compare-view`): off-vs-on differs where off-vs-off does (sky,
  pedestrians, HUD, the hand effect); static textured geometry is black in the difference images.
- Not checked: the texture parameter block and descriptors against the complete path by an automatic comparison (the same
  functions write them); render-target textures inside a run (refused); long sessions; other areas; AC power.

Thermals: Tctl reached 95-101 C in these runs, against 84-85 C in the morning AC runs. The 60-cap arm with fast draws
off was already at 100 C, so this is the state of the machine during the session, not something this change adds.

Packaging: `B/playable-v12` = v11 + binary `e892c1329ecd7f349d3061e7be1ca93d61ac1a7c868cf175885bb68800071a8e`, same settings.
Its launcher was started on the isolated profile and stopped at the loading screen. Desktop entry unchanged (v11).
Real savedata hashes equal to `geom-testing/real-save-hashes-before.txt`; no emulator running; GPU policy auto.

What ends runs now, per frame at that view: texture read semaphore (0x1d6c) 350-460, vertex format (0x1740) 190, shader
program (0x8e4) 55, stencil and colour mask changes. The semaphore release writes a label to guest memory through
`write_gcm_label`, which can defer to the host label writer; it was not taken into the fast path.


## 2026-10-05 (evening): playable-v12 remeasured on AC with better ventilation

Sessions: `geom-testing/v12-ac-uncapped`, `v12-ac-capped`, `v12-ac-timing` (`ab.txt` in the first two, `RPCS3.log` in the
third). Binary `e892c132...` (= `playable-v12/bin/rpcs3`), flags as in the v12 launcher, isolated profile, GPU auto, AC,
slow street view, one boot per row, arms switched live, 3 reps of 10 s. Tctl 87-89 C in every arm (battery run: 92-101 C).

| slow street view | fast draws off | v10/v11 scope (5) | v12 scope (2) |
|---|---|---|---|
| uncapped | 51.8 FPS, 47.8 W | 56.4 FPS, 50.7 W | 59.7 FPS, 52.0 W |
| 60 cap | 51.3 FPS, 47.6 W | 55.5 FPS, 50.4 W | 58.8 FPS, 51.3 W |
Uncapped v12 arms: 58.7, 59.1, 61.3 (the view drifted faster across reps in all arms). The RSX thread is at 99% of a core
uncapped and 97-98% in the capped v12 arms. The camera landed slightly left of the `tex-fast-02` view.

Per-pass RSX-thread time on v12 (`RPCS3_VK_PASS_TIMING=1`, mode 2, uncapped, 56.6 FPS with the timers; another boot):
total 17.7 ms per frame, i.e. the whole frame.
- G-buffer (0xc0ab8000): 9.5 ms, ~3,200 draws, 2.95 us per draw: draw 1.11, fifo 0.85, state 0.51, upload 0.28, bind 0.20.
- Shadow maps (0xcf2f0000): 4.4 ms, ~5,300 draws, 0.83 us per draw: draw 0.45, fifo 0.21, upload 0.12.
- 0xc11c0000: 1.9 ms, ~1,035 draws, 1.82 us per draw. 0xc03b0000 + 0xc0734000: 0.7 ms, 53 draws each at 6.4 us (fifo 3.1).
- Fast draws: 8,433 per frame in 1,132 runs; runs ended by 0x1d6c 435, 0x1740 191, 0x8e4 54, 0x32c 35, 0x330 29.
This boot had about 1,000 more G-buffer draws per frame than the morning timing (2,200), so the per-pass totals are
not comparable with the v9/v10 sections; the per-draw figures are.

State at the end: no emulator running, GPU policy auto, power profile balanced, real savedata hashes equal to
`geom-testing/real-save-hashes-before.txt`. Desktop entry unchanged (v11).


## 2026-10-05 (evening): G-buffer "draw step" taken apart — no new package, v12 remains the newest build

Asked for: work on the G-buffer draw step (1.1 us per draw in the per-pass timing). Sessions and patches are listed at the end
of `geom-testing/README.md`. Test build `5cffb4939cea...` in `build/bin`; AC, GPU auto, slow street view, arms switched live.

What the number was. The fast path has no phase marks of its own, so for a fast draw "draw" is everything after the commands
were read (texture rebind, layout analysis, constants, vertex lookup, binds, Vulkan calls) and "fifo" includes the texture
command handlers. It is not a Vulkan draw call cost.

RSX-thread profile on v12 (`perf-v12-01`, user time): fast path 47-50%, complete path 35-38% (about 1,360 draws), rest 12%.
Inclusive: vertex/index lookup and upload 17%, `load_texture_env` 13.7% (of it `upload_texture` 9%), descriptor set commit
and bind 10%, `load_program` 5%, periodic submit 3.6%, geometry cache probe 5% (self), RADV 13-14%, libc 10.5%.
Kernel time is another 10% of the thread (not in the perf data; `perf_event_paranoid` 2): turning the geometry cache off
removes about 2.5 points (PAGEMAP_SCAN), periodic submit 1000 -> 4000 us changes nothing measurable; the rest is command
buffer submits and fence polling (strace, `sys-01`: per submit about 2.8 SYNCOBJ_WAIT, 2.4 SYNCOBJ_RESET, 2.1 futex wakes).

Tried, in the source tree behind live controls, default behaviour unchanged:
1. Longer fast runs (control 9 = 2 in this build; 6 = v12 scope). The run enders of v12 were followed by more than they
   looked like: the texture read semaphore is followed by a NOP packet carrying 11-24 data words, and many display lists
   are chained by call/return/jump. Now handled inside a run: vertex format registers and the semaphore offset (no handler,
   no state signal), the texture read semaphore release through its handler (only without strict rendering and host labels),
   non-incrementing NOP packets, plain jump/call/return through `rsx::thread::fifo_flow_control` (same steps as `run_FIFO`).
   Runs per frame 1,061 -> 825, fast draws 8,396 -> 8,823. **FPS unchanged** (58.7 vs 58.6; later 59.5 vs 59.1 with the
   material cache on in both). The profile shows why: complete path -2.6 points, fast path +4.3. These draws cost about the
   same on either path; what they spend is in the shared parts (lookups, texture state, binds). Not checked with mode 3/4.
2. Material bindings kept across surface cache generations (control 10 bit 2). The v11 cache was wiped 27 times per frame,
   23 of them because a render target was bound again. Now a bitmap of 64 KiB pages that ever held a surface (533 pages);
   a binding whose texture is away from those pages only expires with the texture cache tags (6 wipes per frame). No
   compressed texture shared a page with a surface in the city or at the dock. Hit rate stays at 72%: the remaining wipes
   (texture cache tag 3 per frame, section release 1 per frame) each cost about 240 materials.
   Check mode (6): city roam about 5 million comparisons, dock 3.1 million, 0 mismatches.
   FPS against off, three boots: 60.6 vs 59.0 (+2.7%, 3 reps), 59.1 vs 58.7 (+0.7%, 4 reps), 67.8 vs 67.7 (+0.2%, 6 reps,
   camera landed on a lighter view). About +1% and not separable from the variation between arms. Left off.

Not done, by expected size: reusing descriptor sets for repeated materials (commit + update + bind is about 10% of the
thread, about 2,500 new sets per frame for about 240 materials; needs care with pool resets and view lifetimes);
per-entry expiry of material bindings (the remaining 28% misses, worth perhaps 1-2%); fewer submits per frame;
libc calls for 8-64 byte compares in `bind_descriptor_sets` and the constants compare in `fast_draw_batch` (about 2% each
in the profile, no live switch to measure them with).

State at the end: no emulator running, GPU policy auto, power profile balanced, real savedata hashes equal to
`geom-testing/real-save-hashes-before.txt`, desktop entry unchanged (v11), `playable-v12` untouched.


## 2026-10-05 (night): descriptor sets reused for repeated materials — playable-v13 (packaged; desktop entry is v11)

Patch and sessions: `geom-testing/descriptor-reuse/`, end of `geom-testing/README.md`. Build `c019de492c60...`.

Before: every draw whose textures differ from the previous draw's got a newly allocated descriptor set, written in full
(about 2,100 per frame at the slow street view, for a few hundred distinct contents).

What was built (`RPCS3_VK_DESCRIPTOR_REUSE` / live control 12, `descriptor_table_t::commit`)
- Each program's descriptor table keeps up to 256 sets it has already written, keyed by the complete contents of all
  slots (unique resource id, view, sampler, layout; buffer, offset, range). The key is compared in full, the hash only
  selects the entry. A commit that finds its contents binds that set again; nothing is allocated or written.
- These sets come from a pool of their own per table (128 sets, doubling up to 2,048 each time it is used up). A used-up
  pool goes through deferred disposal and its entries are forgotten; the regular per-frame pools are not involved.
- Image view and sampler handles can be given to another object after destruction. Destroyed handles are logged
  (`vkutils/descriptor_reuse.h`) and every entry that contains one is dropped before the next lookup. Images, buffers
  and buffer views are told apart by their unique ids. Tables with fewer than 64 commits and tables with image arrays
  (shader interpreter) take the regular path.
- On a reuse the slots that changed stay marked, so the next regular write refreshes them in the write template.

Results (AC, GPU auto, uncapped, arms switched live in one boot, 10 s each, v12 settings otherwise)
| scene | reuse off | reuse on | pairs faster |
|---|---|---|---|
| city slow street view, packaged build | 59.5 FPS, 53.2 W | 60.8 FPS, 54.1 W (+2.2%) | 5 of 5 |
| same view, build `ecb4768e` (fixed 2,048-set pool) | 58.8 | 60.9 (+3.6%) | 6 of 6 |
| city, lighter view (the turn overshot), packaged build | 68.0 | 69.3 (+1.9%) | 5 of 6 |
| dock, packaged build | 88.0 | 88.6 (+0.6%) | 4 of 4 |
At the street view about 2,030 sets per frame are reused and about 100 written; one view is destroyed per frame.
The first version (cache emptied on any destroyed view or sampler, sets from the regular pools) reused only half and
was not faster (`desc-01`); the regular subpools turn over several times per frame.

Checks
- Screenshots alternating off/on at a fixed camera (`desc-final/off*.png`, `on*.png`): off-vs-on differs by 0.3-3.3% of
  pixels, off-vs-off by 1.1-2.5% (traffic, pedestrians, clouds).
- City roam about 80 s and dock roam with lightning about 45 s with reuse on: image correct afterwards
  (`desc-final/after-roam.png`, `desc-dock/after-roam.png`), no log messages that the v12 runs do not have (the FIFO
  error at the city savestate load is there on v12 as well), emulator memory and VRAM figures as before.
- Not checked: no automatic comparison of set contents is possible from the application side and the Vulkan validation
  layers are not installed; long sessions; other areas; other games (the code is generic); the 60 cap.

Packaging: `B/playable-v13` = v12 config and live.ctl + binary `c019de492c60a988851e43f3cc1625d746e9ac93419fde0b04eb0df8ebf799b9`
+ `RPCS3_VK_DESCRIPTOR_REUSE=1`, and `RPCS3_VK_FAST_DRAWS=6` (in this binary 6 is the v12 scope; 2 adds the jump/semaphore
handling of the previous section, which measured nothing). Launcher smoke test on the isolated profile: intro ran 75 s
with reuse active; the process had to be killed after a 40 s SIGTERM timeout, as with v11 and v12.

State at the end: no emulator running, GPU policy auto, power profile balanced, real savedata hashes equal to
`geom-testing/real-save-hashes-before.txt`, desktop entry unchanged (v11).

Desktop entry switched from v11 to `playable-v13` on request (2026-10-05 night); previous entry saved as `playable-v13/infamous2.desktop.previous`.


## 2026-10-05 (night): GPU lighting rewritten for speed, lighting and occlusion code cleaned up — playable-v14 (packaged; desktop entry is v13)

Asked for: make the GPU passes faster where possible (bit-exact output is not required, small floating-point drift is
fine), then clean the code up for a release, and measure. Patch and sessions: `geom-testing/lighting-fast/`, end of
`geom-testing/README.md`. Build `533f0006ab11...`.

Lighting, before: two compute passes that reproduced the SPU job's arithmetic bit for bit (table-based reciprocal and
square-root estimates read from a buffer, `precise` everywhere). The tile pass ran one thread per tile, walking its
1,280 pixels and then again per light; positions went through a 14.7 MB buffer; the result went to a buffer and was
copied into two images.

Lighting, now (`VKNativeLightingShaders.hpp`, five passes, ordinary GPU arithmetic)
- lights: per-light constants once (scaled colour and its length, 1 / (outer - inner radius)).
- bounds: bounding box of the view-space positions per 32x8 block of pixels, one thread per 2x2 pixels, reduced in
  shared memory. (One thread per tile row was 1.4 ms on its own; this is 0.17 ms.)
- cull: per tile, the lights whose volume reaches it (box, tile frustum planes, cone against bounding sphere).
- shade: per pixel, only the set bits of the tile's mask; a light the pixel is out of range of is skipped (its terms
  are zero there); positions are recomputed from depth; results are written to the two images directly.
- clear: the game leaves a tile empty unless one of its pixels is inside the range of a light, and an empty tile is
  not the same bytes as a lit tile that received no light. The shade pass flags tiles that had a pixel in range; this
  pass zeroes the others. Tiles without any candidate light are never written (the images are cleared first).

Offline, on the 780M, captured frame and synthetic cases (`claude-work/light-ref/fast_validate.py`, `fast-validation.json`)
| case | lights | exact | fast | pixels within 1 level (diffuse / specular) | largest difference |
|---|---|---|---|---|---|
| captured frame | 8 | 2.28 ms | 0.45 ms | 99.993% / 99.998% | 3 |
| one spot light | 1 | 1.54 ms | 0.39 ms | 99.996% / 100% | 2 |
| no lights | 0 | 1.17 ms | 0.33 ms | identical | 0 |
| captured lights x8 | 64 | 11.1 ms | 1.26 ms | 99.981% / 99.996% | 7 |
Times include about 0.07 ms of submit and wait per iteration and use buffers for input and output in both columns.

In the emulator (AC, GPU auto, GPU busy 69% in every capped arm: the governor lowers the clock instead)
| scene | before | after |
|---|---|---|
| dock, 60 cap, exact against fast switched live, 6 pairs | 44.6 W, 1,698 MHz | 42.5 W, 1,192 MHz |
| city, 60 cap, switched live, 5 pairs | 48.2 W, 2,227 MHz | 44.4 W, 1,306 MHz |
| dock, 60 cap, v13 against cleaned build, one boot each | 39.9 W, 1,768 MHz | 36.7 W, 1,200 MHz |
| city, 60 cap, one boot each | 46.3 W, 2,142 MHz | 43.1 W, 1,317 MHz |
| city spawn view, uncapped, one boot each | 69.0 FPS, 53.0 W, 2,246 MHz at 80% busy | 70.4 FPS, 49.4 W, 1,680 MHz |
So 2-4 W less at the cap, and the GPU is no longer at its top clock in the city (it was closer to limiting than the
69% busy figure suggested, because the governor holds 69% by raising the clock).

Cleanup
- `VKNativeLighting.cpp` rewritten in the style of the surrounding code around the five passes: job capture, input
  packing, latch of the two G-buffer copies, compute, substitution. Gone: the exact passes and their header, the
  reciprocal tables and tile coordinate table in the shader input, the shadow capture mode, the dump modes
  (`RPCS3_NATIVE_LIGHTING` bit 2, `_SHADOW`, `_DUMP_*`, `_SPU_INPUT_DIR`) with their hooks in `VKDraw.cpp` and
  `SPUThread.cpp`. The file is about as long as before (536 lines against 553), but one statement per line.
- The shader header is written by hand and is what the offline harness reads; nothing is generated into the tree.
- `VKNativeSSAO.cpp`: what the job does and what each constant is, validation dump and debug darkening removed
  (`RPCS3_NATIVE_SSAO` bits 2 and 8), state in a `unique_ptr`, and the game check it lacked (`BCES01143`). The
  occlusion arithmetic is unchanged; its GPU time was not measured.
- Not cleaned: `SPUThread.cpp` still carries diagnostics from the whole project besides the hooks these two features
  need, and both features are switched by environment variables and fixed guest addresses.

Checks: screenshots exact against fast in one boot at the dock (differences as between two exact ones), v13 against
the cleaned build at the dock and in the city (`lighting-fast/v13-left-new-right.jpg`), dock roam with lightning on
the fast passes, launcher smoke test (intro, 80 s, forced stop after the usual SIGTERM timeout).
Not checked: long sessions, other areas, scenes with many lights in the emulator (64 only offline), spot lights in the
emulator beyond what the dock and city contain.

Packaging: `B/playable-v14` = v13 settings + binary `533f0006ab113c52ba5b38e15d786dee85538c10e9405190feccd9168afd699c`.
State at the end: no emulator running, GPU policy auto, power profile balanced, real savedata hashes equal to
`geom-testing/real-save-hashes-before.txt`, desktop entry unchanged (v13).

## 2026-10-05 (later): GPU lighting and occlusion at any resolution scale (source only, no new package)

Before: with `Resolution Scale` other than 100 neither pass switched on (dock, 150%: 3.5 minutes without a
`NativeSSAO`/`NativeLighting` line), so the game's SPU jobs ran: 45 FPS at the dock at 150%.

Cause: with a scale, RPCS3 makes the destination of the two G-buffer blits a render target of the scaled size
(1920x1080 at 150%) instead of a 1280x720 image, and both passes required 1280x720. The image the hook in
`texture_cache::blit` finds is the right one at every scale. The source of the depth blit is not usable in its place:
it is a depth-stencil target (`D32_SFLOAT_S8`), and the destination holds RPCS3's conversion to the ARGB bytes the
passes read.

Changes
- `VKNativeLighting.cpp`: input and output images have the size of the blitted image and are made again when it
  changes; the image size, its inverse and a pixel offset go to the shaders in input words 24-29. The offset is
  `0.5 - 0.5 * scale`, so that the centre of a scaled pixel maps to where the game's 1280x720 grid has it (the job
  takes pixel x as x / 1280); it is 0 at 100%.
- `VKNativeLightingShaders.hpp`: the tile grid stays 40x18, a pixel belongs to tile `x * 40 / W`, `y * 18 / H`, so tiles
  need not be a whole number of pixels (130% gives 41.6). Bounds: one work group per fifth of a tile as before, each
  thread now loops over its share of the block (2x2 pixels at 100%). Clear: one thread per pixel row of a tile.
- `VKNativeSSAO.cpp`: images at the blitted size and half of it, rounded up. The sample offsets of the occlusion
  stage and the tap distance of the blur are in pixels of a 640 wide image in the job and are multiplied by
  `width / 640`; the clamps use the image size. The game's own two textures (1280x720 occlusion, 640x360 depth) keep
  their size and are replaced by the larger images, which it samples with normalized coordinates.
- No change in common code. `readback_unneeded` and the blit hook are as before.

Checks
- 1280x720 offline (`reference/lighting/fast_validate.py`): every case identical to the recorded
  `fast-validation.json`, byte for byte in the comparison figures.
- Scaled offline, new in `fast_validate.py`: the captured frame enlarged to 1664x936, 1920x1080 and 2560x1440 with
  nearest pixels, against the 1280x720 reference enlarged the same way. Pixels within 4 levels: 99.92%, 99.99%, 99.86%
  (diffuse), 99.99% and more (specular). The large byte differences (up to 160) are the shared-scale encoding: decoded
  (colour times alpha) the largest difference is 7 levels at 1.3x; at 2x four pixels, one pixel of the 1280x720 frame
  at a tile corner, are lit where the reference has an empty tile.
- Emulator, dock, one boot each, uncapped (`CFG:Frame limit='Off'`, vblank 120, 3 arms of 10 s):
  | scale | image | FPS | package W | GPU busy |
  |---|---|---|---|---|
  | 100% | 1280x720 | 94.9 | 55.7 | 76% |
  | 150% | 1920x1080 | 73.0 | 57.0 | 99% |
  | 200% | 2560x1440 | 46.4 | 49.5 | 99% |
  100% was 95.7 in the README table (another boot). At the 60 cap, 150% holds 60.0; with both passes off at 150% the
  title showed 44.7-46.2.
- Screenshots at 150%, GPU passes against SPU jobs (`sessions/res150`, `res150-spu`): mean absolute difference 0.84%
  of full scale, against 0.82% between two shots of the GPU passes seconds apart (fire and smoke move).
- Lightning at 150% (`lightning_shots.sh`): the half-resolution depth substitution engages, effects look right.

Not checked: other areas than the dock at a scale, scales below 100%, changing the scale while the game runs (the
images are made again on a size change, but that path was not exercised), MSAA, and the look of the occlusion blur at
scales where its taps skip pixels (200%). The window used for the screenshots is 1152 pixels wide, so they do not show
the added detail, only that the picture is right.
