"""Compile/run the offline lighting shaders and compare hardware output."""
import json, os
from pathlib import Path
import struct
import subprocess
import numpy as np
import ours
from validate import compare

ROOT = Path(__file__).resolve().parent
GPU = ROOT / 'gpu'
MAX_LIGHTS = 256  # MASK_WORDS*32 in gpu/common.glsl


def pack_input(A, B, params, records):
    # The exact shaders in gpu/ need 1280x720; the emulator's passes (fast_validate.py) take any size
    if A.ndim != 3 or A.shape[2] != 4 or B.shape != A.shape or A.dtype != np.uint8 or B.dtype != np.uint8:
        raise ValueError('Byte G-buffers of one size expected')
    if len(params) != 256 or any(len(r) != 48 for r in records):
        raise ValueError('Malformed parameter block or light record')
    if len(records) > MAX_LIGHTS:
        raise ValueError(f'Prototype has a {MAX_LIGHTS}-light limit')
    header = np.zeros(32 + 16*MAX_LIGHTS, dtype=np.uint32)
    header[:16] = np.frombuffer(params[16:80], '>u4')
    header[16] = struct.unpack_from('>I', params, 0x98)[0]
    header[17] = len(records)
    for i, record in enumerate(records):
        kind = struct.unpack_from('>I', record, 20)[0]
        if kind not in (1, 2):
            raise ValueError(f'Unsupported light kind {kind}')
        row = np.zeros(16, dtype=np.float32)
        row[:3] = np.frombuffer(record[:12], '>f4')
        row[4:7] = np.frombuffer(record[12:18], '>f2')
        row[7:11] = np.frombuffer(record[32:40], '>f2')
        row[11] = np.frombuffer(record[46:48], '>f2')[0]
        row[12:15] = np.frombuffer(record[40:46], '>f2')
        header[32+16*i:48+16*i] = row.view(np.uint32)
        header[32+16*i+3] = kind
    lut = np.concatenate([np.asarray(ours.LUT[key], np.uint32) for key in
        ('spu_frest_fraction_lut', 'spu_frest_exponent_lut',
         'spu_frsqest_fraction_lut', 'spu_frsqest_exponent_lut')])
    header[20] = len(header)
    # Preserve the SPU's refined tile-coordinate ratios. Division by constants
    # may otherwise become multiplication by a rounded reciprocal in GLSL.
    coords = np.concatenate([np.arange(1, 41)/40, np.arange(18)/18]).astype('f4').view(np.uint32)
    header[21] = len(header) + len(lut)
    header[22] = len(header) + len(lut) + len(coords)
    clip = np.frombuffer(params[80:144],'>u4').astype(np.uint32)
    header[18] = header[22] + len(clip)
    # Image size, its inverse and the pixel offset, as VKNativeLighting.cpp sets them
    size = np.array([A.shape[1], A.shape[0]], 'f4')
    header[24:26] = A.shape[1], A.shape[0]
    header[26:28] = (np.float32(1) / size).view(np.uint32)
    header[28:30] = (np.float32(0.5) - np.float32(0.5) * size / np.array([1280, 720], 'f4')).view(np.uint32)
    header[19] = header[18] + A.shape[0]*A.shape[1]
    words = np.concatenate([header, lut, coords, clip, np.frombuffer(A.tobytes(), '>u4'), np.frombuffer(B.tobytes(), '>u4')])
    return words.astype('<u4').tobytes()


def run(A, B, params, records):
    (GPU / 'input.bin').write_bytes(pack_input(A, B, params, records))
    subprocess.run([str(GPU / 'runner'), str(GPU / 'input.bin'),
                    str(GPU / 'tiles.spv'), str(GPU / 'lighting.spv'), str(GPU / 'output.bin')], check=True)
    words = np.fromfile(GPU / 'output.bin', '<u4')
    return words.astype('>u4').view(np.uint8).reshape(2, *A.shape)


def main():
    # VULKAN_INCLUDE: directory with the Vulkan headers when they are not installed system-wide
    include = ['-I' + os.environ['VULKAN_INCLUDE']] if 'VULKAN_INCLUDE' in os.environ else []
    subprocess.run(['c++', '-std=c++17', '-O2', *include, str(GPU / 'runner.cpp'), '-lvulkan', '-o', str(GPU / 'runner')], check=True)
    for name in ('tiles', 'lighting'):
        subprocess.run(['glslc', '-I'+str(GPU), str(GPU / (name+'.comp')), '-o', str(GPU / (name+'.spv'))], check=True)
    inputs = ours.inputs()
    output = run(*inputs)
    reference = ours.lighting(*inputs)
    oracle = np.load(ROOT / 'frame-emu.npz')
    result = dict(hardware_vs_reference={name: compare(o, ref) for name, o, ref in zip(('A','B'), output, reference)},
                  hardware_vs_interpreter={name: compare(o, oracle[name].reshape(o.shape)) for name, o in zip(('A','B'), output)},
                  cases={})
    A, B, params, records = inputs
    p = ours.positions(B, np.frombuffer(params[16:80], '>f4').astype('f4').reshape(4,4))
    direction = p[205,859] - np.frombuffer(records[5][:12], '>f4').astype('f4')
    direction /= np.linalg.norm(direction)
    spot = bytearray(records[6])
    spot[:12] = records[5][:12]
    spot[32:36] = records[5][32:36]
    spot[40:46] = direction.astype('>f2').tobytes()
    cases = [('spot_centre', A, B, [bytes(spot)]), ('point_and_spot', A, B, [records[5], bytes(spot)])]
    v = direction + np.array([0, 0.55, 0], dtype='f4')
    v /= np.linalg.norm(v)
    spot[40:46] = v.astype('>f2').tobytes()
    cases.append(('spot_falloff', A, B, [bytes(spot)]))
    cases.append(('no_lights', A, B, []))
    a = A.copy()
    a[200:240,832:848,0] = 0
    a[200:240,848:864,0] = 255
    cases.append(('gloss_extremes', a, B, [records[5]]))
    for name, a, b, lights in cases:
        gpu = run(a, b, params, lights)
        ref = ours.lighting(a, b, params, lights)
        result['cases'][name] = {channel: compare(o, r) for channel, o, r in zip(('A','B'), gpu, ref)}
        print(name, result['cases'][name], flush=True)
    groups = [result['hardware_vs_reference'], result['hardware_vs_interpreter'], *result['cases'].values()]
    result['passed'] = all(v['max_error'] <= 1 for group in groups for v in group.values())
    (ROOT / 'gpu-validation.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
