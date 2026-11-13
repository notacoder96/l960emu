# Z80 "SRAM Zero Test" at unit start-up (why "Starting up units: Timed out")

Disassembly: /tmp/claude-0/z80/all.txt (z80dis, script /tmp/claude-0/z80/dis.py).

## 1. Z80OS.PCI function $02F7 = block engine (fill / verify) over banked SRAM
Parameter block at $038E (24 bytes, written by LoadBulk):
| off | field | test value |
|---|---|---|
| $38E | bank (0-3) | 00..03 |
| $38F | chunk length (bytes per callback) | $10 |
| $390-391 | callback address (IY) | $028B = fill, $02A1 = verify |
| $392-393 | start address -> working pointer $3A7 | $4000 (window 0) or $C000 (window 1) |
| $394-395 | total length | $4000 (16 KB) |
| $396-3A5 | 16-byte pattern | all zero |
Code: A = $40 << bank -> write $8B18 (Lexichip B reg $B18; also toggled bit7 by the idle loop and set to $C0 at the end: looks like LEDs/flags, not bank select); bank -> $3B7E (Lexichip A reg $B7E = SRAM bank select; same register is written by LoadBulk $0207, RunSingleFunction $0276 and the idle loop before calling the 4 kernel entry points at $26/$28/$2A/$2C). Then loops chunk-wise calling the callback; stops early when it returns A=0; advances $3A7 by the chunk.
- $028B fill: LDIR pattern($396) -> ($3A7), chunk bytes; returns 1.
- $02A1 verify: compares ($3A7+i) with pattern[i]; on mismatch: $3A7 += i, $3A6 = 1, $3A9 = read byte, $3AA = expected, return 0; on full match $3A6 = 0, return 1.
Result block $3A6..$3AA = {fail flag, address lo, hi, read, expected}.
Our readback 00 00 80 00 00 = PASS (flag 0, pointer ran to $8000 = $4000+$4000). A real card returns the same. Nothing is wrong functionally.

## 2. Host side
- ChamAPI `cChameleonStandardInterface::BootDiags` (0x10003d20, error text "Z80 SRAM Zero Test") walks a table of 16 blocks at 0x10007A78 (24 bytes each): fill banks 0-3 of window $4000, verify them, same for window $C000. For each block (0x10003d80): LoadBulk 24 bytes to $38E on Z80 #0 and #1, RunSingleFunction($2F7) on both, and if callback == $02A1, FetchBulkDump(5 bytes, $3A6) from each Z80.
- Success condition: byte 0 ($3A6) == 0 for both Z80s on every verify block (nonzero -> formatted error with address/read/expected).
- "Starting up units" (LICSDLL 0x100167d0 -> 0x10023f70): for each unit, WaitForThunderEvent(unit+0x2B0, 0xAFC8 = 45000 ms); timeout -> ERRVAL 0x19 "Timed out". The unit thread runs BootDiags (and the rest of unit init) before signalling, so the whole SRAM test must finish well within 45 s.
- Measured: each verify pair takes ~4.5 s, each fill pair ~1 s -> 8*4.5 + 8*1 ≈ 44 s, plus the rest -> over 45 s. Pure emulation speed.

## 3. What to change
1. Speed (the actual failure): verify costs ~265 T-states/byte -> 4.3 M T per 16 KB per Z80. ChamDevice runs only 160 Z80 T-states per 2000 56301 instructions (~2.4 MHz effective at ~30 MIPS DSP). Run the Z80s much faster relative to the DSP, e.g. SliceZ80 = 2000 (or more) T per slice, or let the Z80 run ahead/batch while the DSP is busy-waiting on IRQC/IRQD. The chips z80 core is cheap (tens of ms per 4 M T). Real Z80 clock unknown; with ~10 MHz a 16 KB verify takes ~0.4 s, i.e. the whole test ~6-10 s on hardware.
2. Bank select (correctness, not needed for this zero-pattern test since aliasing passes): Lexichip A write to offset $B7E (Z80 $3B7E) must call setBank(0, v & 3) and setBank(1, v & 3) (window $4000 -> bank v, window $C000 -> bank 4+v). Currently m_bank never changes.
3. Lexichip register notes from the kernel: $B18 (status) bit 6 = DPRAM INTL, active low (0 = command pending; kernel then reads $7FE). Lexichip B $B18 is written ($40<<bank, $C0, bit7 heartbeat toggled each idle pass) -> treat writes as an output latch. Init at $01BB writes Lexichip A $B13=$80, $B14=$40, $B15=3 then 1, $B12=$42, $B10=1, $B11=$57, $B12=$66, $B10=0, $B1A=$40; Lexichip B $B13=$C0 (meaning unknown; just latch). XMOQ/DLMU/WCS ($200/$400/$600) are not touched by these routines.
