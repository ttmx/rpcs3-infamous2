# Lighting job: reverse-engineering notes

Working notes from reconstructing the game's tiled deferred lighting job, kept as written. They follow the order of
the work: an offline NumPy reconstruction, a standalone Vulkan prototype that reproduced the SPU arithmetic bit for
bit, an opt-in shadow pass inside RPCS3, and finally the replacement. The shadow and dump modes they mention were
removed from the emulator once the faster passes in `rpcs3/Emu/RSX/VK/VKNativeLightingShaders.hpp` replaced the
exact ones; `../README.md` says what still runs from this folder.

Run `python validate.py` to check the NumPy reference against the captured full
frame and 12 synthetic cases evaluated by the original SPU program. Run
`python gpu_validate.py` to build the two exact compute shaders and their standalone
runner, execute them on the GPU, and compare five additional GPU cases.
The runner requires glslc, a C++ compiler, libvulkan, and the Vulkan headers
(`VULKAN_INCLUDE` when they are not installed system-wide). It skips CPU Vulkan
implementations and does not alter hardware policy. Results are `validation.json` and
`gpu-validation.json`.

Recovered details:

- The 32x40 tile bounds exclude d24=0xffffff. Collapsed bounds on any of the
  three axes produce entirely zero output, rather than encoded black (A=90).
  The dock has 226 active tiles out of 720; these agree with the interpreter.
- Record kind is a big-endian u32 at +0x14. Kinds 1 and 2 are point and spot.
  Position xyz is float32 at +0x00; colour RGB is float16 at +0x0c; inner/outer
  radii are float16 at +0x20/+0x22.
- Spots additionally use half values +0x24 (negative cone gain), +0x26 (outer
  cosine), direction xyz at +0x28..+0x2c, and +0x2e (minimum distance). With
  `delta=light-position`, `angle=dot(delta,-direction)*rsqrt(dot(delta,delta))`,
  cone attenuation is clamp((angle-outer_cos)*(-gain),0,1). Multiply radial
  attenuation by this term before squaring; suppress it at/below min distance.
- Specular is computed from the material byte using the game's polynomial
  approximation, its halfway-vector lobe, and colour magnitude. B byte 0
  receives encoded sqrt(specular); bytes 1..3 are zero in the tested job path.
  Changing input B byte 3 throughout a tile did not alter either output.
- Preserve SPU FREST/FRSQEST + FI LUT arithmetic and incremental position
  reconstruction. GLSL `precise` and explicitly rounded tile-coordinate ratios
  are needed; rounded reciprocal multiplication introduced a two-byte spot
  error before this was corrected. GPU positions now match NumPy exactly.

Results for the captured frame:

| Comparison | A | B |
|---|---:|---:|
| NumPy vs SPU interpreter, identical bytes | 99.9999729% | 99.9955783% |
| NumPy vs SPU interpreter, maximum error | 1 | 1 |
| NumPy vs captured game, maximum error | 4 | 1 |
| NumPy vs captured game, pixels within 1 | 99.9940321% | 100% |
| GPU vs NumPy, identical bytes | 100% | 100% |

All 12 SPU synthetic cases pass with maximum error <=1. They cover point,
overlapping point, spot centre/falloff/outside/near cutoff, mixed point+spot,
no lights, collapsed and sky tiles, input B byte 3, and material extremes.
The five GPU cases also pass with maximum error <=1. These are correctness
results for one captured camera/frame and directed cases, not gameplay FPS.

Original checklist before live testing (status updated below):

1. Completed for the live shadow pass: read the actual GPU G-buffer images.
   The standalone runner continues using captured SSBO inputs. The live pass
   still stores a full position buffer and removes no readback/upload.
2. Establish frame-consistent matrix/light-table capture with shadow rendering
   before substituting outputs. Shader compilation and result freshness must
   fail back to the original job. Match exact supported game/program bytes;
   buffer addresses alone are insufficient admission guards.
3. Cover preliminary per-tile frustum/spot-volume rejection from P071. Current
   AABB/per-pixel tests match this capture and the directed cases, but do not
   prove all camera/light configurations equivalent at tile boundaries. Other
   light kinds currently raise an error in the offline host packer.
4. Validate more real frames, including moving camera, character and lightning
   scenes. Then preserve queue bookkeeping while skipping P071 work; remove
   the G-buffer readbacks only after every consumer is accounted for.
5. Measure focused matched gameplay and power with the same camera and
   hardware policy. There is no new live FPS claim from this continuation.


## Live shadow testing, 2026-10-04

The live test directory (`../../bench/` in this repository) contains the isolated launcher/profile, exact
binary provenance, source backups, and four successful 16-frame captures:
`dock-shadow-04`, `dock-shadow-motion-01`, `dock-shadow-freshness-01`, and
`dock-shadow-lightning-01`. The two latter runs record light-table bytes again
when the game consumes its lighting result. `analyze_live.py` compares sampled
GPU inputs/output, the NumPy reference, and the game's output. Each directory
has `comparison.json`, an analysis log, and per-frame arrays/raw dumps.

The diagnostic source is `VKNativeLighting.cpp/.h` plus generated
`VKNativeLightingShaders.hpp`. Enable only with
`RPCS3_NATIVE_LIGHTING_SHADOW=1` and an existing
`RPCS3_NATIVE_LIGHTING_DUMP_DIR`. `generate_live_shaders.py` produces the header
and shader files from the standalone sources. Hooks pair the full-size A/B
G-buffer DMA blits, sample the original GPU images directly, and record results
without replacing them. Default execution retains the original path.

Live motion fails the admission criterion: the shader agrees with NumPy within
one byte value, but some real SPU outputs exceed that tolerance. In the first
run, 13 stationary captures have >99.99% A pixels within 1 and all B pixels
within 1; rotation then exposes three active-tile disagreements in frame 013
and diffuse errors up to 29 in later frames. Spot tile 5 in frame 013 is zero
in the prototype but encoded black (A byte 0 = 90) in both the original
interpreter and game. Its spot sphere overlaps the tile AABB, while no actual
pixel passes the current cone test: selecting active tiles by pixel cone hits
is stricter than the game's preliminary volume selection.

`live_oracle.py` replays selected failing tiles. Frames 014/015 tile 212 agree
with the interpreter within 1 but disagree with the game up to 28/29 (A) and 8
(B); fused FMA replay does not explain these larger differences. Parameters
are unchanged at consumption. Latest additional captures also check light-table
freshness. The next useful probe is the actual live SPU DMA input/parameter
snapshot for one failing tile, rather than assuming that GPU image readback and
the replay's dispatcher state reproduce every live input.

The tile oracle was corrected to resize the initial captured DMA list's light
transfer for the requested light count; it previously always fetched the
original eight records. It now waits for four output PUT lists to ensure the
selected tile's two buffers have been written during pipelined dispatch. All 12 existing directed cases
were rerun and pass (`validation-corrected.log`, `validation.json`).

No guarded substitution, job stub, readback removal, or speedup was promoted.
Shadow rendering/file dumps are diagnostic overhead, so GUI FPS values from
these runs are not matched optimization results. GPU policy remained `auto`;
normal saves were hash-verified unchanged. The isolated launcher seeds only
serialized guest SPU bytecode into each exact-binary cache, not native objects.
A missing VFS-root trailing slash caused the first launch failure; a raw SPU
bytecode cache was needed to resume the existing state reliably. Savestate SFO
metadata reports APP_VER 01.00 despite the loaded update executable, so future
substitution admission must check exact loaded job bytes rather than this field.


## Native culling and isolated fuzzing continuation

The target is matching lighting behavior with small floating-point differences,
not byte-perfect instruction emulation. The SPU interpreter runs only as the
isolated test oracle. Production remains native geometry and per-pixel GLSL.

Recovered and ported the tile's four frustum planes, its AABB/sphere bounds, and
sphere-versus-cone intersection. A separate pixel rejection step determines
whether a light is processed. Crucially, spotlight pixel selection tests
`distance < outer radius` and the cone angle, **not** the near-distance cutoff.
The cutoff suppresses shading afterward. Pixels inside that cutoff can therefore
make a tile active with encoded-black output; the old selector incorrectly
zero-filled it. The frustum normals were checked against the original SPU's
arguments rather than inferred from visual matches.

- `fuzz_isolated.py`: deterministic mutations, original SPU oracle, saved
  counterexamples, and optional hardware validation. It never launches RPCS3.
  `fuzz-native-02/results.json`: 120 cases pass; `fuzz-native-03/results.json`:
  another 420 pass, with 42 actual Radeon GPU cases. Maximum A/B error is 1;
  no active/zero tile disagreements in the accepted runs. The earlier
  `fuzz-native-01` retains the plane-orientation counterexample.
- `fuzz_light_counts.py`: 14 point/mixed cases at counts 1,31,32,33,59,63,64
  test both words of the light mask against the SPU oracle and the GPU.
  All pass (`fuzz-light-counts.json`). The dense 31-light trial exceeded the
  oracle's instruction budget; the boundary tests use rejected lights plus
  active lights in the high mask bits. This tests the mask boundaries without
  turning the oracle run into a long benchmark.
- `gpu-native-culling-validation.log`, `gpu-native-64-validation.log`: full
  captured frame and existing directed GPU cases pass. GLSL and the packer
  now support 64 lights with two mask words per tile. A live lightning input
  reached 59 lights; 32 was not an adequate supported limit. Larger tables
  continue to reject the shadow pass rather than truncate them.
- `probe_selection.py` compares native predicates with the original SPU's
  culling call arguments. `live_oracle.py` also supports a diagnostic JIT
  numerical variant (fused FMA and fixed-point FI) for investigating the
  historical live differences. It does not add an emulator to production.
- `analyze_spu_inputs.py` compares completed live SPU LS GET data with the
  paired GPU image. The initial index formula was invalid after a rejected
  sample; frame IDs now come from the actual capture metadata/log. Corrected
  `dock-shadow-inputs-01/spu-input-comparison.json` compares 327,680 bytes:
  **zero input, parameter, or light-table differences** for the two probed
  tiles over 16 captured pairs. This is scoped to those sampled tiles.

## Work required to remove the G-buffer copies

Current addresses have two roles in a frame: A `0x37400b80` holds normals and
material before lighting, then diffuse light; B `0x37784b80` holds packed depth,
then specular light. They are each 3,686,400 bytes. The existing round-trip
trace shows both CPU-side buffers rewritten before their lighting upload.

| Current consumer/copy | Evidence | Required change |
|---|---|---|
| GPU G-buffers to guest A/B | `VKTextureCache.cpp: cached_texture_section::dma_transfer` | Keep the original GPU images as frame-owned inputs; omit staging/shuffle/host copies only after both guest readers are bypassed. |
| P071 lighting GET/PUT lists | `spu-inventory/job-io-map.txt`, hash `01f9aa7f` | Replace pixel work and suppress its image transfers; retain descriptor/light-table reads, atomics, queue claims, tags, counters and completion. |
| SSAO dispatcher GET lists | same map, hash `8f2e59a2`; `SPUThread.cpp` | GPU SSAO currently returns from its five pixel kernels but still performs dispatcher G-buffer/intermediate transfers. Bypass those data transfers or restructure the dispatcher. |
| SSAO intermediate storage and AO PUTs | SSAO map: `0x37b08000..0x37c22480`, `0xcf800000..0xcf8e1000` | Leave bookkeeping live, suppress now-dead image GET/PUT work. Current AO skip covers only its output range, not the G-buffer inputs. Audit both bulk and per-element paths. |
| Guest lighting/AO texture uploads | `VKDraw.cpp: load_texture_env` | Choose native textures **before** `m_texture_cache.upload_texture`; both current hooks occur after that call, so substitution alone still pays stock upload/copy work. Preserve remap, format, sampler and validity metadata. |
| Unknown CPU or unsupported-job consumer | historic maps cover the two known SPU jobs only | Record additional accesses and provide lazy materialization of the correct current image phase on unexpected CPU reads/writes. A two-job trace is not proof that no PPU consumer exists. |

The existing live compute trigger is itself inside `dma_transfer`. Removing
readback demand would stop that hook from firing. Move acquisition/dispatch to
an RSX-side point where the final G-buffer image versions are known, independent
of a CPU read fault: latch the producing framebuffer/blit images and their
render generation, then schedule SSAO/lighting before the final consumer.
The matching job descriptor/light table must be captured immutably for that
same generation. Guest addresses alone are not a frame identity.

Introduce a bounded GPU ownership record for input/output phases, pending GPU
completion and resource lifetime. Preserve all image and compute barriers;
retain buffers/images/descriptors until their submission completes. Admission
must check the loaded P071/SSAO program bytes, supported dimensions/formats,
light kinds/count, matrices, and recognized job/consumer sequence. The diagnostic
first-code hash is an attribution aid, not sufficient replacement admission.
On a mismatch, materialize the required phase and run stock jobs before exposing
normal CPU-readable memory again. Never return stale data, leave pending tags,
or skip the dispatcher entry `0x43d0` wholesale.

Both `do_list_transfer` and `do_list_transfer_diagnostic` have bulk fast paths
that execute before per-element handling; generic/direct DMA and reservation
operations also need review. An address-range early return in one memcpy path
will neither eliminate all copies nor preserve the job protocol automatically.
The current AO `g_native_ssao_gpu_active` flag also needs frame-specific
admission: a persistent global true value is insufficient for future fallback.

At 1280x720, the two image readbacks total **7,372,800 bytes/frame** and their
lighting uploads another **7,372,800**. Eliminating the AO upload adds **921,600**,
for 15,667,200 bytes/frame of those GPU/host transfers alone (~940 MB/s at 60 FPS).
This excludes repeated CPU/SPU/intermediate copies, staging/shuffle work and
synchronization; it is a byte-count opportunity, not a predicted FPS gain.
The prototype additionally writes a full 14,745,600-byte world-position buffer.
A performant production implementation should use parallel tile reductions and
recompute/retain positions locally rather than adding that off-chip pass.

Suggested implementation order: guarded native texture substitution with stock
jobs/readbacks retained; then skip lighting pixel work while keeping its job
protocol; then remove SSAO dispatcher image transfers; finally remove the two
readbacks and stock lighting/AO uploads once the phase/fallback mechanism and
all consumers are validated. Measure matched focused gameplay after correctness,
with diagnostic dumps disabled and the same quality/hardware policy.


## Matched live-job validation (2026-10-04)

The user allows small float differences; byte equality is not an admission
requirement. Production uses native vector lighting/culling formulas. The SPU
interpreter is only an isolated test oracle; it is not called by the GPU path.

Six-image shadow dumps now include the actual stock uploaded textures sampled
by the game, in addition to native outputs and original G-buffer images. GPU
completion is signalled after both consumer-image copies, with transfer/host
visibility and submission lifetime retained. Each sample writes its own pair
metadata, avoiding index shifts after rejected samples or overwritten logs.

`dock-shadow-consume-inputs-01` has 16 stationary samples with zero active-tile
disagreements and A/B maxima 5/1 against uploaded textures. Guest snapshots and
those uploaded images are byte-identical. During motion, however, completed
SPU GET probes sometimes contain different inputs or job matrices/light counts
from the readback-time shadow snapshot. Even unchanged consume-time VM blocks
are not proof that a job used those parameters. Frame-pair-based comparisons
(`actual-tiles-comparison.json` included) can still mismatch generations; they
must not be presented as kernel correctness or replacement admission.

The diagnostic probe now captures GET **and PUT** LS bytes for tiles 59, 212,
and 502 around each sampled readback pair. `analyze_job_tiles.py` associates
same-thread transfers with the same parameter block, light records and tile,
and requires all 40 rows of both inputs and both outputs. This compares native
formulas with the output of the actual matching live SPU job, independent of
speculative GPU/guest frame pairing:

- `dock-shadow-job-inputs-01`: **153 matched stationary tiles**, no unmatched
  groups or active-tile disagreements; diffuse/specular maxima **2/1**.
- `dock-shadow-job-motion-inputs-01`: **153 matched camera/lightning tiles**,
  no unmatched groups or active-tile disagreements; maxima **4/1**. Light
  counts include 8,10,11,12,13,15,38,39,56,57. Every pixel is within four.
- `validate_job_gpu.py` replays all **153 moving-job tile cases** through the
  actual Vulkan shaders in the standalone runner, using the captured job's
  inputs and parameters. Results: no active-tile disagreements; maxima
  **4/1** (`job-gpu-comparison.json`). This is hardware evidence in addition
  to the NumPy reference and isolated oracle fuzzing.

Latest diagnostic binary SHA256:
`416f2bf1bd9e61673577923e66abde1a6833f6a9a2f5f6fa81e2572570e29ac2`.
The first job-probe run was stationary because its input automation attempted
an unavailable xdotool; it remains valid stationary evidence. The motion run
used the existing verified-window keyboard helper and reached 57-light jobs.
All processes were stopped by verified identity. Normal savedata's 12 hashes
remain unchanged. No new savestate or quality/hardware-policy change was made.

**Remaining integration work:** replace readback-time VM parameter snapshots
with immutable descriptors/light tables associated with the exact guest job
and RSX input generation. The native kernels pass the sampled matched-job
checks, but the current shadow dispatch does not have that ownership model.
Do not promote it to texture substitution or remove copies on the strength of
these results alone. The copy-removal plan above needs this association as its
first correctness requirement, then consumer/lifetime/fallback validation.


## GPU lighting replacement (2026-10-04, late night)

`RPCS3_NATIVE_LIGHTING` (bits: 1 sample GPU images, 2 dumps, 4 stub the SPU tile function, 8 drop its image PUTs)
is implemented and described in the handoff document, section "lighting on the GPU, opt-in". The "remaining
integration work" above (immutable job parameters tied to the right frame) is done by taking the parameters on
the SPU thread at each job start. New here: `fuzz_real_frames.py`, `stub_probe.py`, `analyze_replace.py`,
`replace_oracle.py`. `validate.oracle` takes a `budget` and maps the RSX label page for the last tile.


## G-buffer readbacks removed (2026-10-05)

Done with `RPCS3_NATIVE_LIGHTING` bit 16; see the handoff section of the same date for the mechanism and numbers.
Relative to the table in "Work required to remove the G-buffer copies":

| Item | State |
|---|---|
| GPU G-buffers to guest A/B | Not issued while both jobs are stubbed; any other reader still faults and gets them. |
| P071 GET lists | Skipped (`native_gbuffer_list_skip`); PUTs were already skipped by bit 8. |
| SSAO dispatcher GETs, scratch GET/PUT | Skipped. Scratch C1 turned out to be a texture the game samples: now the GPU `z_half` image. |
| Lighting/AO texture uploads | No longer happen (guest memory is not written); lookups still go through the texture cache, except C1. |
| Trigger inside `dma_transfer` | Moved to the completing NV3089 blit in `vk::texture_cache::blit`. |
| Per-frame admission | Decided at each lighting job start (`g_native_gbuffer_unread`); the occlusion pass has no dynamic fallback. |
| Remaining | The two blits, the private latch copies, the position buffer; the 64-light limit (frames above it fall back). |

`skip_get_probe.py` is the offline evidence that the stubbed lighting job ignores the rows it loads.


## 2026-10-05 night: fast passes are what the emulator runs now

The emulator no longer contains the exact shaders of `gpu/` (`generate_live_shaders.py` would write a header nothing
includes). Its lighting is five compute passes with ordinary GPU arithmetic, kept in
`source/rpcs3/Emu/RSX/VK/VKNativeLightingShaders.hpp`. `fast_validate.py [iterations]` reads the pass texts from that
header, adds `offline-io/io_offline.glsl` (G-buffer words and results in storage buffers instead of images), runs them
and the exact shaders through `offline-io/runner.cpp` on the captured frame and the synthetic cases, compares both with
the reference, and times both on the GPU (`fast-validation.json`). Change a pass in the header, run the script.
