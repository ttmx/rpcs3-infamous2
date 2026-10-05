# SPU program inventory (inFamous 2, BCES01143 v1.04)

Inputs: `EBOOT-104.elf` (decrypted with `rpcs3 --decrypt`), RPCS3's SPU cache (`spu-mega-v1-tane.dat`,
every SPU block that has executed so far), and `ls-lighting-capture.bin` (an SPU local-store dump from the
earlier session).

- `inventory2.py` groups executed blocks into programs by matching raw bytes in the executable -> `inventory.json`
- `detail.py <cache> <program id>...` assigns the named hot kernels to programs and lists never-executed ranges
- `coverage.py <cache>` coverage by file position (looser program boundaries; adjacent programs overlap)

Caveats: the game's jobs are raw SPURS job binaries, not ELF files, so program boundaries are inferred from
where executed code was found. A cache block's stored address is its entry point but its data can start
lower, so `inventory2.py` under-matches some blocks (the 732-instruction lighting kernel at 0x44a8 is one).
Re-run after more of the game has been played to see what new code has executed.

Added 2026-10-05: `spudis.py` (disassembler; `spudis.py <file> <file_offset> <ls_address> <count>`),
`jobs.py` (container enumeration -> jobs.json). The five small type-D kernels at file 0x86d500..0x870d90
load at LS 0x4000; the hottest lighting loop (732 instructions) is at file 0x86fd28 = LS 0x44a8.
