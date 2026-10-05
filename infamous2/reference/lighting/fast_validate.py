"""Build the lighting passes of the emulator source tree (VKNativeLightingShaders.hpp) with the offline io, run them
and the exact shaders (gpu/) on the captured frame and the synthetic cases, compare both with the reference, and time
them on the GPU: python fast_validate.py [iterations]

The passes also run on the captured frame enlarged to other sizes, as with a resolution scale. Nothing computes a
reference there, so those results are compared with the reference of the 1280x720 frame enlarged the same way; the
enlarged pixels sit at slightly different places, so that comparison is close but not exact."""
import json, os, re, subprocess, sys
from pathlib import Path
import numpy as np
import ours
from validate import compare
from gpu_validate import pack_input

ROOT = Path(__file__).resolve().parent
FAST, GPU = ROOT / 'offline-io', ROOT / 'gpu'
PIXELS = 1280 * 720
HEADER = ROOT.parents[2] / 'rpcs3/Emu/RSX/VK/VKNativeLightingShaders.hpp'
# Buffers after the input: block bounds, tiles, lights, output / output, positions, tiles
FAST_BUFFERS, EXACT_BUFFERS = f'{3600 * 32},{720 * 16 * 4},{256 * 64}', f'{PIXELS * 8},{PIXELS * 16},{720 * 4 * 8}'
SCALES = (1.3, 1.5, 2.0)


def passes(width, height):
    # Work groups per pass, as dispatched by VKNativeLighting.cpp
    return {'lights': (4, 1, 1), 'bounds': (40, 90, 1), 'cull': (12, 1, 1), 'shade': ((width + 7) // 8, (height + 7) // 8, 1),
            'clear': ((40 * height + 63) // 64, 1, 1)}


def build():
    # VULKAN_INCLUDE: directory with the Vulkan headers when they are not installed system-wide
    include = ['-I' + os.environ['VULKAN_INCLUDE']] if 'VULKAN_INCLUDE' in os.environ else []
    subprocess.run(['c++', '-std=c++17', '-O2', *include, str(FAST / 'runner.cpp'), '-lvulkan', '-o', str(FAST / 'runner')], check=True)
    texts = dict(re.findall(r'inline constexpr const char\* (\w+) = R"GLSL\((.*?)\)GLSL";', HEADER.read_text(), re.S))
    head = '#version 450\n' + texts['common'] + (FAST / 'io_offline.glsl').read_text()
    for name in passes(1280, 720):
        source = FAST / f'{name}.comp'
        source.write_text(head + texts[name])
        subprocess.run(['glslc', '-O', str(source), '-o', str(FAST / f'{name}.spv')], check=True)


def run(which, A, B, params, records, iterations=1):
    (FAST / 'input.bin').write_bytes(pack_input(A, B, params, records))
    if which == 'fast':
        height, width = A.shape[:2]
        steps = [f"{FAST / (name + '.spv')}:{g[0]}:{g[1]}:{g[2]}" for name, g in passes(width, height).items()]
        buffers, output = f'{FAST_BUFFERS},{width * height * 8}', '4'
    else:
        steps = [f"{GPU / 'tiles.spv'}:12:1:1", f"{GPU / 'lighting.spv'}:{(PIXELS + 63) // 64}:1:1"]
        buffers, output = EXACT_BUFFERS, '1'
    out = subprocess.run([str(FAST / 'runner'), str(FAST / 'input.bin'), str(FAST / 'output.bin'), str(iterations), buffers, output, *steps],
                         check=True, capture_output=True, text=True).stdout
    ms = next((float(l.split()[1]) for l in out.splitlines() if l.startswith('ms_per_iteration')), None)
    words = np.fromfile(FAST / 'output.bin', '<u4')
    return words.astype('>u4').view(np.uint8).reshape(2, *A.shape), ms


def cases():
    A, B, params, records = ours.inputs()
    yield 'captured_frame', A, B, params, records
    p = ours.positions(B, np.frombuffer(params[16:80], '>f4').astype('f4').reshape(4, 4))
    direction = p[205, 859] - np.frombuffer(records[5][:12], '>f4').astype('f4')
    direction /= np.linalg.norm(direction)
    spot = bytearray(records[6])
    spot[:12] = records[5][:12]
    spot[32:36] = records[5][32:36]
    spot[40:46] = direction.astype('>f2').tobytes()
    yield 'spot_centre', A, B, params, [bytes(spot)]
    yield 'point_and_spot', A, B, params, [records[5], bytes(spot)]
    v = direction + np.array([0, 0.55, 0], dtype='f4')
    v /= np.linalg.norm(v)
    spot[40:46] = v.astype('>f2').tobytes()
    yield 'spot_falloff', A, B, params, [bytes(spot)]
    yield 'no_lights', A, B, params, []
    a = A.copy()
    a[200:240, 832:848, 0] = 0
    a[200:240, 848:864, 0] = 255
    yield 'gloss_extremes', a, B, params, [records[5]]
    # Every captured light several times over, to load the per-pixel loop
    yield 'lights_x8', A, B, params, list(records) * 8


def enlarge(image, scale):
    # Nearest pixel of the 1280x720 image under the centre of each pixel of the enlarged one
    rows = ((np.arange(round(image.shape[0] * scale)) + 0.5) / scale).astype(int)
    columns = ((np.arange(round(image.shape[1] * scale)) + 0.5) / scale).astype(int)
    return np.ascontiguousarray(image[rows][:, columns])


def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    build()
    result = {}
    for name, A, B, params, records in cases():
        exact, _ = run('exact', A, B, params, records)
        fast, _ = run('fast', A, B, params, records)
        entry = {'lights': len(records)}
        if name != 'lights_x8':
            reference = ours.lighting(A, B, params, records)
            entry['fast_vs_reference'] = {c: compare(o, r) for c, o, r in zip('AB', fast, reference)}
        entry['fast_vs_exact'] = {c: compare(o, r) for c, o, r in zip('AB', fast, exact)}
        # Alternate the two so that both see the same GPU clock
        times = {'exact': [], 'fast': []}
        for _ in range(3):
            for which in times:
                times[which].append(run(which, A, B, params, records, iterations)[1])
        entry['ms'] = {k: round(float(np.median(v)), 3) for k, v in times.items()}
        result[name] = entry
        print(name, json.dumps(entry), flush=True)
    A, B, params, records = ours.inputs()
    reference = ours.lighting(A, B, params, records)
    for scale in SCALES:
        big = enlarge(A, scale), enlarge(B, scale)
        fast, ms = run('fast', *big, params, records, iterations)
        entry = {'lights': len(records), 'size': [big[0].shape[1], big[0].shape[0]],
                 'fast_vs_enlarged_reference': {c: compare(o, enlarge(r, scale)) for c, o, r in zip('AB', fast, reference)}, 'ms': {'fast': round(ms, 3)}}
        result[f'captured_frame_x{scale}'] = entry
        print(f'captured_frame_x{scale}', json.dumps(entry), flush=True)
    (ROOT / 'fast-validation.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
