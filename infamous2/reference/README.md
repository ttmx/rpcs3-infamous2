# Offline references for the two GPU passes

The tools used to work out what the game's ambient occlusion and lighting SPU jobs compute, and to check the GPU
passes against them. They need Python 3 with NumPy; the GPU validators also need `glslc`, a C++ compiler and
libvulkan.

No game data is included, so most scripts need inputs you capture from your own copy: the decrypted executable
(`rpcs3 --decrypt`, as `spu/EBOOT-104.elf`), and frame captures (`lighting/cap/`, `ssao/stages/`, `*.npy`). The
investigation log describes how each capture was taken; some of the capture hooks it names were removed from the
emulator in the final cleanup.

- `spu/`: `spudis.py`, an SPU disassembler generated from RPCS3's opcode table; `spuemu.py`, an SPU interpreter in
  Python following RPCS3's interpreter semantics (`spu_luts.json` holds its reciprocal tables); `inventory*.py`,
  `coverage.py`, `jobs.py`, `detail.py`, which map executed SPU code to the job binaries inside the executable.
- `ssao/`: `ours.py` is the five-stage occlusion pipeline in NumPy, the source of the shaders in
  `rpcs3/Emu/RSX/VK/VKNativeSSAO.cpp`; `stage_emu.py`, `aok.py`, `probe.py`, `tracer.py` and the rest run the game's
  own kernels in the interpreter to compare against and to trace their arithmetic.
- `lighting/`: `ours.py` is the lighting job in NumPy; `validate.py` compares it with the SPU interpreter;
  `gpu_validate.py` builds and runs the bit-exact standalone shaders in `gpu/`; `fast_validate.py` extracts the
  passes the emulator actually uses from `rpcs3/Emu/RSX/VK/VKNativeLightingShaders.hpp`, runs them on the GPU and
  compares them with the reference (run `gpu_validate.py` first, it builds what the comparison needs);
  `fuzz_*.py` generate cases; the `*.json` files are the recorded results; `NOTES.md` is the working log.
