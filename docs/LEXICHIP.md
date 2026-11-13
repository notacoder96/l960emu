# Lexichip model: settled semantics

What `core/src/lexichip.cpp` implements and why. "Captured" means the Lexichip bus images the host software
(Framework.exe through the helper process) and the Z80 extension kernel actually program, dumped with
`tests/lexi_capture` and disassembled with `tests/lexbus_dis.py`. The factory algorithm DLL images are
templates: they contain no DLMU lines and placeholder delays/gains, so they are silent on their own. Only
captured images are meaningful as reverbs.

## Bus map (per chip, 3 KB window)

| Range | Contents | Evidence |
|---|---|---|
| $200-$3FF | XMOQ: 128 x 32-bit memory-op entries, little endian | LexDis.dll 0x10002e30 |
| $400-$51F | DLMU: 9 byte planes x 32 units (`$400 + $20*plane + unit`) | LexDis.dll 0x10002bd0 |
| $600-$AFF | WCS: 5 byte planes x 256 lines (`$600 + $100*plane + line`) | LexDis.dll 0x10002900 |
| $B00-$B04 | Host wide-data latch | captured write stream |
| $B18 | status (bit 6 = mailbox int, active low) | ChamAPI |

## Wide data latch ($B00-$B04)

Writes to $B00-$B04 are held. Two writes commit them ("Host Wide Data Latch", service manual):

* XMOQ byte 3 (`$203 + 4n`) commits the pending $B00-$B02 into bytes 0-2 of that entry (894 occurrences in a full
  load; without this the delays stay at the DLL placeholders).
* WCS plane 0 (`$600 + line`) commits the pending $B01-$B04 into planes 1-4 of that line (514 full commits,
  286 with only $B04 pending).

Direct plane/XMOQ writes (host LoadLexichip) also occur and are taken as is. Other observed registers:
$B7F ($FE start / $FF stop), $B7E bank select, $B7A written every kernel tick, $B50 (4-byte read), $B28 (read).
They are stored but have no effect in the model.

## Registers (LexDis.dll name table at VA 0x1000a378)

`c0-c31` = $00-$1F, `x0-x31` = $20-$3F, $40-$4F serial outputs (even = TMX, odd = LEX), $50-$5F serial
inputs (same split), `peak0-3` = $60-$63 (store keeps max |v|), `dlmu0-1` = $64-$65, >= $65 no store / reads 0.

**c0..c25 are pairs: reading an odd register returns its even partner as it was at the end of the previous
sample.** Memory reads land in even registers and are consumed through the odd one; one-pole filters store
`c14` and read `c15` as the y[n-1] term (captured SurroundHall lines 16-18). With odd registers as independent
storage the captured programs produce no output at all. Odd-reads-current-even gives identical results on
every captured program; the one-sample double buffer is chosen because it matches the filter idiom exactly.
c26-c31 and all x registers are plain.

## WCS line (planes b0..b4)

b0 imm (8-bit coefficient /256, or DLMU unit in bits 0-4, ?STEP? in bit 5); b1: bit0 unity, bit1 negate,
bits2-3 add scale (0 = x4), bit4 add enable, bit5 add the accumulator (else the source register again),
bit6 multiply the accumulator; b2: bits0-6 store destination, bit7 accumulator used by store/acc operands;
b3: bits0-6 source, bit7 target accumulator; b4: bit0 invert coef (~imm / ~coef16), bit1 DLMU coefficient,
bit2 no DLMU offset, bit4 memory op, bits 3/5/6/7 unknown (no effect modelled).

**Store-before:** the store in a line writes the accumulator value from *before* that line's MAC (pipeline).
`acc0 = 1.0*x1 ; x0 = acc0` copy idioms and the LP feedback lines only make sense this way, and store-after
gives silence (energy -98 dB) on the captured programs versus a decaying reverb with store-before.

## Memory (XMOQ)

Each line with b4 bit 4 consumes the next XMOQ entry: bytes 0-2 address, byte 3 bits 0-6 register,
bit 7 set = write `[addr] = reg`, clear = read `reg = [addr]`; applied at its line, before the MAC.
DLMU lines add the unit's 24-bit offset unless b4 bit 2.

**Addressing is linear:** `phys = (ptr + addr) mod 2^20`, with `ptr` decremented once per sample over the whole
1M-word DRAM (1M x 16 per chip, service manual parts list U41/43/47/49). Writing at A and reading A+N gives an
N-sample delay. SimpleDelay's taps continue across $1FFFF -> $20000, which rules out 128K regions. In
captured programs the top bits act as allocation zones (input diffusion at $4xxxx/$5xxxx, ER line at $6xxxx,
tank at $0xxxx-$3xxxx in 32K segments), which is a program convention rather than a hardware feature.
Write ordering within a sample (first-wins, deferred) made no difference on any captured program.

## DLMU units

Fields: step p2:p1:p0 (20-bit), goal `p3<<8 | (p2 & $F0)`, coefficient p5:p4 (16-bit, /65536), offset
p8:p7:p6 (24-bit). p6 bit 0 flips the coefficient sign. Unit select: imm & $1F, and if (imm & $C0) == $C0 the
low bit is cleared (LexDis). Slewing: the coefficient moves by `int8(p0)*2` per sample toward the goal and
stops there (LexDis "DLMU Status" page shows step as p0x2). **ASSUMPTION:** every unit slews once per sample;
slewing only on ?STEP? lines leaves units without such a line (e.g. the block coupling gain d16 on some chips)
at zero. The Z80 kernel's background task rewrites units 17-24 (p5) continuously (LFO allpass modulation) and
unit 12; that requires the Z80 to run alongside audio.

## LEX link between chip pairs

A chip's LEX output channel c (odd register $41 + 2c) arrives at its partner's LEX input channel (c + 2) mod 8
(odd register $51 + 2c'). Captured chip 0 writes LEX out 01/03/05 and reads LEX in 19/21/23/25; chip 1 writes
01/03/05/15 and reads 21/23/25. With this mapping the SurroundHall tank closes as a four-block loop
(A.block1 -> B.block0 -> B.block1 -> A.block0 -> A.block1).

## Measured (tests/lexichip_ir, captured SurroundHall, 0.5 FS impulse into TMX in 18/20/22)

Machine A (chips 0/1): TMX_out00 energy -29.6 dB, outputs decay ~30 dB/s (RT60 1.7-2.1 s on the main
outputs), smooth down to -145 dB. Machine B (chips 2/3): same shape, RT60 ~2.0 s.

## Open points

* Fixed point: truncating `>>` leaves a constant offset of about -102 dB in the tank after the decay (a DC
  limit cycle). Rounding vs truncation and 24-bit saturation are unconfirmed.
* Several 8-bit gains in the captured images are 0 (e.g. the ER-to-tank taps `0.000*c28`, diffusion
  coefficients): presumably preset values written by the host/Z80, but not verified against hardware.
* b4 bits 3/5/6/7 and b1 bit 7 (`dp`) have no modelled effect.
* Whether odd c registers read the previous sample or the current even value (indistinguishable so far).
