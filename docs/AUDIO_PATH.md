# 960L reverb card audio path (T-MIX, 56301 mixer kernel, Lexichip serial) — implementable model

Sources: service manual ch. 6 (SerialAudioTest port lists) and ch. 7 (Reverb card); 960LDiags.exe SerialAudioTest
(0x410b40.., map helpers 0x415e60..0x416200); ChamDSP.pci IRQA kernel P:$CD2-$EF4, DMA1 chain set-up P:$5AD,
slew task P:$2D9; host logs /tmp/claude-0/tmix.log, /tmp/claude-0/probe/{dspmem,io}.txt, lex*.bin;
measurement harness /tmp/claude-0/probe/kern.cpp (runs the real kernel on the emulated 56301, impulse per input).

## 1. T-MIX
Three T-MIX ASICs, each = serial ports <-> "ping-pong RAM" (PPRAM) visible to the 56301 as DSP-port words.
* DSP port: T-MIX1 $83C000, T-MIX2 $83A000, T-MIX3 $838000 (1K words). PPRAM location v <-> DSP word $300+v.
  The T-MIX double-buffers PPRAM internally per word clock; the DSP always sees $300-$3FF.
* Serial ports (manual 6-13/14): T-MIX1 ports 0-11 = I/O backplane octal lines (mostly inputs), T-MIX2 ports 0-11 =
  backplane (mostly outputs), T-MIX3 ports 0-3 -> LEXI_1..4_SER_IN, ports 4-7 <- LEXI_1..4_SER_OUT. 8 channels/port.
* Map (registers $80-$FF, written by op 2 rows of 16 or op 1 single bytes): entry index = $A0 + 8*port + channel
  ($80-$9F = "ports -4..-1", unused, always FF). Value = PPRAM location: input port: sample is stored to $300+v;
  output port: sample is sent from $300+v. FF = null location ($3FF, never touched), FE = zero location (silence).
  (Diag uses $7F=null, 3=zero, 1=send, 2=receive the same way.)
* Registers (op 1): $48+p port direction (bit7 = output/send) ; $54+p unknown per-port flag (diag: $80 all, host: 0);
  $7D DSP engine: $CD run, $8D stopped (host stops it around map changes), $00 off during config; $7B test control
  ($1C during init, then 0); $7C $FB; $7E clock control ($C0 T-MIX1/2, 0 T-MIX3); $7F control (T-MIX2 = 9);
  $74-$77 AWCK0/1 rising/falling edge (T-MIX3: 74=0 75=FF 76=0 77=FF, Lexichip word clock); $63-$66 SPO reset lines.
  None of these change the data path once running except direction + map; model: run when $7D bit6 set.
* Op 3 (1024 commands): all value 0 to $300-$3FF of each T-MIX (T-MIX2 twice) = clearing PPRAM.

Final maps of the running SurroundHall system (port p, channel c):
* T-MIX1: dir out = ports 4,5. Input ports 10,11: p10 c -> $380+2c, p11 c -> $381+2c (16 words $380-$38F).
  Output ports 4,5: p4 c <- $390+c, p5 c <- $398+c ("wet output", host mEnableTMIX1WetOutput).
* T-MIX2: dir out = ports 0-3, 8-11. Ports 8,9: p8 c <- $390+2(c&3), p9 c <- $391+2(c&3) (slots 4-7 repeat 0-3);
  ports 0-3, 10, 11 <- FE (silence). No inputs mapped.
* T-MIX3: dir out = ports 0-3. p0 (Lex1 in) c0..2 <- $390,$391,$392 ; p1 (Lex2 in) c0,1 <- $393,$394 ;
  p2 (Lex3 in) c0..2 <- $398..$39A ; p3 (Lex4 in) c0,1 <- $39B,$39C ; other channels FE.
  p4 (Lex1 out) c0..2 -> $380..$382 ; p5 (Lex2 out) c0,1 -> $383,$384 ; p6 (Lex3 out) -> $388..$38A ;
  p7 (Lex4 out) -> $38B,$38C ; others FF.
* I/O backplane: backplane "interface" n = T-MIX port pair (2n, 2n+1). AIN (slot 3, CTL $601=$80) takes input
  interface 5 = T-MIX1 ports 10/11; AOUT (slot 4, CTL $801=$2C: unmuted, octal select 1) takes output interface 4 =
  T-MIX2 ports 8/9; AES (slot 2) in/out interfaces are mapped but silent (FE) in this config.
  At single speed a card's 8 channels use slots 0-3 of both lines; channel index i = 2*slot + line (ASSUMPTION on
  which physical jack is i; consistent for in and out).

## 2. Lexichip serial slots (LexDis.dll names)
In/out registers $40-$4F (out) and $50-$5F (in): even = "TMX" (T-MIX3 line, slot k = (reg&$F)/2), odd = "LEX"
(partner chip, chained Lex1<->Lex2, Lex3<->Lex4; out odd reg r of one chip -> in odd reg r of the partner).
The engine's even/odd convention is therefore right. Frame alignment (deduced from the programs vs the maps):
  T-MIX3 channel c arrives in TMX in slot (c+1) mod 8 ; TMX out slot k is sent as T-MIX3 channel (k+1) mod 8.
Check: Lex1 XMOQ writes inputs $52/$54/$56 (TMX 1,2,3) = channels 0,1,2; Lex1 writes outs $40/$42/$4E (TMX 0,1,7)
= channels 1,2,0; Lex2 reads TMX 1,2 and writes TMX 0,7 = channels 0,1 -> exactly the 3+2 channels in the maps.

## 3. 56301 audio kernel (IRQA = word clock, one call per sample)
DMA1 chain (P:$5AD), 5 words per descriptor {src,dst,count-1,DCR,link}, two sets (Y:$180 uses buffers $6xx,
Y:$1C0 uses $7xx) alternating each word clock (x:$1 bit19); the kernel processes the set the DMA filled last tick:
  1 T-MIX1 $380-$38F -> Y:$600 (16)      inputs
  2 Y:$610 -> T-MIX1 $390-$39F (16)      wet out
  3 Y:$620 -> T-MIX2 $390-$39F (16)      main out
  4 X:$600 -> T-MIX3 $390-$39F (16)      Lexichip sends
  5 T-MIX3 $380-$38F -> X:$610 (16)      Lexichip returns
  6 T-MIX2 $380-$38F -> X:$620 (16)      T-MIX2 PPRAM inputs (unmapped here: 0)
  7 T-MIX2 $3A0-$3BF -> T-MIX2 $3C0-$3DF (32) (pass-through, unmapped here)
  DCR $DD02D4/D1/D0 = DE+DIE, 1-D, X/Y as listed. DMA1 IRQ ($1A) loads the next descriptor (P:$CAF).
Mode: x:$2 != 0 -> x:$1 bit20 = 16-input mode; the running system has x:$2 = 0 (8-input mode, below).
Coefficients (host Poke56301, 24-bit fractions stored NEGATED: kernel uses mpy -y0,x0), targets -> slewed copies
used by the kernel (task $2D9 once per word clock, step = |cur|/256 + $400 LSB toward target):
  X:$100-$17F -> X:$208 Min ; X:$180-$187 -> X:$288 Gsend ; X:$188-$207 -> X:$290 Mout ; Y:$150-$157 -> Y:$158 Gret
Per sample (8-input mode), all values 24-bit fixed, products rounded (macr/mpyr), saturating:
  in[i]  = Y:$600+i, i=0..7                    (T-MIX1 $380+i)
  s[j]   = sum_i Min[j][i] * in[i], j=0..15    Min[j][i] = -X:$208 + 16*(j>>1) + 2i + (j&1)
  send[j]= Gsend[j>>1] * s[j]       -> X:$600+j (T-MIX3 $390+j)
  ret[k] = X:$610+k                              (T-MIX3 $380+k)
  r'[k]  = Gret[k>>1] * ret[k]                   (k=0..15)
  wet[k] = r'[k]                    -> Y:$610+k (T-MIX1 $390+k)
  o[m]   = X:$620+m + sum_{k=0..7} Mout[m][k] * r'[(m>>3)*8 + k], m=0..15,
           Mout[m][k] = -X:$290 + 16*(m>>1) + 2k + (m&1)   (same pair layout as Min)
  8-input mode only: o[m] += o[m+8] for m=0..7 (rows 8-15 = second machine block folded onto 0-7)
  out[m] = o[m]                     -> Y:$620+m (T-MIX2 $390+m)
  meters: x:$29..$30 = max|in| (P:$D0A).  y:$5 += 1 (word-clock counter for the slew task).
Measured with the live coefficients (kern.cpp; gains 1.0 = identity):
  in0..4 -> send0..4 (machine A), in6,7 -> send8,9 (machine B); Gsend = Gret = 1.0 (stored -1.0).
  ret0..4 -> out0..4 ; out6/out7 = stereo fold of ret0..4 with (1,0) (.010,.990) (.505,.495) (.924,-.383) (-.383,.924);
  ret8..12 likewise to out0..4/out6,7 (folded); wet[k] = ret[k] for all 16.
Cost if run as code on the emulated 56301: ~540 kernel instructions + 7 DMA IRQs (~10 each) ~= 600 instr/sample
= 29 M instr/s at 48 kHz (58 M at 96 kHz) plus slew/scheduler; HLE above is a few hundred flops/sample.

## 4. Machine A, end to end (current SurroundHall, single speed)
AIN ch i (i=0..4) -> backplane interface 5 -> T-MIX1 port 10/11 slot i>>1, line i&1 -> PPRAM $380+i -> DMA1 ->
Y:$600+i -> Min (identity) x Gsend -> X:$600+i -> DMA4 -> T-MIX3 PPRAM $390+i ->
  i=0,1,2: T-MIX3 port 0 ch i -> Lexichip 1 TMX in slot i+1 (regs $52,$54,$56)
  i=3,4:   T-MIX3 port 1 ch i-3 -> Lexichip 2 TMX in slot i-2 (regs $52,$54)
-> SurroundHall on Lex1+Lex2 (chained through the odd LEX regs) ->
  Lex1 TMX out slots 7,0,1 -> T-MIX3 port 4 ch 0,1,2 -> PPRAM $380..$382 ; Lex2 TMX out 7,0 -> port 5 ch 0,1 -> $383,$384
-> DMA5 -> X:$610..$614 = ret0..4 -> Gret -> out0..4 (+ fold into out6/7) -> Y:$620+m -> DMA3 -> T-MIX2 PPRAM
$390+m -> T-MIX2 port 8 (m even, slot m/2) / port 9 (m odd) -> AOUT ch m (0..7) ; also wet[0..4] -> T-MIX1 ports 4/5.
Machine B: in6,7 -> send8,9 -> T-MIX3 $398,$399 -> Lex3 TMX slots 1,2 ; Lex3/Lex4 returns $388-$38C -> ret8..12.
Latency: DMA ping-pong + kernel = 2 word clocks DSP side, plus T-MIX/Lexichip serial pipeline (not modelled).

## 5. Implementation notes for the plugin
* Keep per-T-MIX: dir[12], map[128] (from op1/op2 writes; card.cpp Tmix already stores m_ctl), PPRAM[256] read
  side and write side per sample (dspRead/dspWrite at offsets $300-$3FF).
* Per word clock: (1) T-MIX input ports -> PPRAM (T-MIX1 from plugin inputs via the AIN interface; T-MIX3 from
  Lexichip TMX out slots with the +1 rotation); (2) mixer HLE above (or the real kernel via IRQA + DMA1 emulation);
  (3) PPRAM -> output ports (T-MIX3 -> Lexichip TMX in slots with the +1 rotation; T-MIX2 -> plugin outputs via
  the AOUT interface); (4) Lexichips processSample + odd-reg chaining.
* Plugin I/O: inputs = the interface the AIN card got (T-MIX1 ports 10/11), outputs = AOUT's (T-MIX2 ports 8/9);
  derive from the map/dir state rather than hard-coding (entries whose port is an I/O interface).
* Coefficients come only from host Poke56301 into X:$100-$207 / Y:$150; IRQA is not injected by the emulator today,
  so the slewed copies X:$208.. stay 0 — an HLE must read the targets (and slew them) itself.

## 6. Observation (outside the routing model)
With the dumped host-programmed Lexichips (lex0/1.bin), signal fed to the TMX inputs ($52/$54/$56, stored to DRAM
by XMOQ writes at $5FF80/$5FFC0/$5FF40) never reaches any output in the current Lexichip model (DRAM fills, outputs
stay 0), while signal on the LEX chain regs does. So the plugin will be silent even with correct routing until the
Lexichip memory read side (read offsets / DLMU base as programmed by the Z80) is checked. Repro:
/tmp/claude-0/probe/lexprobe.cpp.
