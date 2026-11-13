# 960L reverb card: DSP56301 side of the host protocol (ChamDSP.pci / ChamID.pci)

Tools: /tmp/claude-0/dspdis/dis (dsp56kEmu disassembler + 56301 symbols). Listings: /tmp/claude-0/dspdis/cham.dis, id.dis, c.txt (compact ChamDSP).
NOTE: in those listings the label "DTXM" is printed for X:$FFFFCD; from usage $FFFFCC = DTXM (master transmit, DMA dest for PCI writes) and $FFFFCD = DTXS (slave transmit). Read "DTXM" in the listings as DTXS.

## 0. Boot image / boot flow
- .pci format: u32 count, u32 load address, count x u32 (24-bit words) = the 56301 HI32 bootstrap stream. Both ChamID.pci (824 words) and ChamDSP.pci (3864 words) load at P:$0000, entry P:$0000 = `jmp $100`.
- Both programs, when told to (host command vector $72, see below), restore SR/SP/OMR (OMR bit2 set, bits0/1 clear) and `jmp $FF0000` = re-enter the bootstrap ROM to accept the next image. So the driver boots ChamID, reads the ID, sends HCVR $8073, then streams ChamDSP.pci (IOCTL A410EC44 carries the file: in = 4-byte card index + 15464 bytes of file).

## 1. Register addresses used (X: I/O)
| addr | name | use |
|---|---|---|
| $FFFFC5 | DCTR | `&= $8FFFFF; |= $100000` -> HM[22:20]=%001 (PCI mode). bit0 HCIE=1. bits3-5 HF3..HF5 = status code to host, bit6 HINT = host interrupt request |
| $FFFFC6 | DPCR | init $0402A0. `bset #14` before every master transfer and on retry (ASSUMPTION: clear/flush master FIFO) |
| $FFFFC7 | DPMC | master: bits15-0 PCI addr[31:16], bits21-16 burst length-1, bits23-22 FC=%10 (always $800000 base) |
| $FFFFC8 | DPAR | master: bits15-0 PCI addr[15:0], bits19-16 PCI command: $C (mem read multiple) for host->DSP, $7 (mem write) for DSP->host; BE=0. Writing DPAR starts the transaction |
| $FFFFC9 | DSR | bit2 SRRQ (slave receive data in DRXR), bit1 STRQ (DTXS empty), bit3 = end-of-data flag polled in download loops (ASSUMPTION: host flag) |
| $FFFFCA | DPSR | bit4 polled before writing DPMC/DPAR (master address ready). After transfer bit14 set = success. Error bits 5-11 copied into y:$2 by HI32 IRQ handlers; bits21-16 = remaining data count; bit15 = retry-needed |
| $FFFFCB | DRXR | slave writes from host (HTXR) and master-read data (DMA source) |
| $FFFFCC | DTXM | master write data (DMA dest) |
| $FFFFCD | DTXS | slave transmit to host (host reads HRXS) |
| $FFFFEF..EC | DSR0/DDR0/DCO0/DCR0 | DMA ch0 = HI32 master transfers |
| $FFFFEB..E8 | DMA ch1 | audio ping-pong chain (IRQA) |
| $FFFF8F..82 | timers | TCSR0/1=$254: word-clock period measurement (y:$b,y:$c), TCR2 used by audio DMA code |
| $FFFFBD/BE/BF | PDRC/PRRC/PCRC | GPIO port C: PRRC=7 (PC0-2 outputs); PC2 toggled = task-switch activity (LED/scope pin) |
| $FFFFAD | PDRD | bit3 cleared by host opcode 4 |
| $FFFFF5 | IDR | ChamID returns it |

DMA0 for master read: DSR0=$FFFFCB, DDR0=X dest, DCO0=n-1, DCR0=$4EEAC0 then bset #23. Master write: DSR0=X src, DDR0=$FFFFCC, DCR0=$4EFA50. Completion: DMA0 IRQ (VBA+$18) sets y:$2 bit18 (ID: y:$3). Each DSP word <-> one 32-bit little-endian dword, 4 bytes/word address increment (ASSUMPTION: FC=%10 = 24-bit right-aligned; driver reads IDR=0x2301 etc. as plain dwords).

Emulation shortcut: model a master transaction atomically: when DPAR is written, copy BL+1 dwords between host memory at (DPMC[15:0]<<16|DPAR[15:0]) and the DMA0 target, assert the DMA0 interrupt, keep DPSR bit4=1, set bit14=1, error bits 0.

## 2. Vector table (VBA=0). Host command vector = HCVR>>1 &0x7F, x2
| VBA | HCVR written by CHAM.sys | ChamDSP handler | meaning |
|---|---|---|---|
| $10 IRQA | - | $CD2 | word clock: run audio DMA ch1 chain (tables Y:$180/Y:$1C0, toggles x:$1 bit19), T-MIX DSP ports $83C380.., buffers Y:$600-$72F |
| $14 IRQC | - | $CBA | Z80#1 mailbox: y:$3 = X:$9D07FF (reply code), set event bit6 |
| $16 IRQD | - | $CC6 | Z80#2 mailbox: y:$4 = X:$9D17FF, event bit7 |
| $18 DMA0 | - | $8DA | master DMA done -> y:$2 bit18 |
| $1A DMA1 | - | $CAF | audio DMA chain next descriptor |
| $24/$28 | - | $255/$272 | timer0/1 compare: rate measure |
| $60,$62,$64 | - | $8C3 | HI32 error/termination: y:$2 |= DPSR&$FE0, DPSR error bits cleared, y:$2 bit23 |
| $70 | - | $8D8 | y:$2 bit4 |
| $72 | $8073 (NMI+HV$39) | $243 -> x:$1 bit0 | task $291: T-MIX2 reg$63<-6, IPR=0, jump boot ROM $FF0000 (reload). ChamID: same vector sets x:$2 bit0 -> jump $FF0000 |
| $74 | $75 | $245 -> bit22 | Z80 kernel download follows (see 4) |
| $78 | $79 | $CA3 | read 16 words via DRXR into x:$3..x:$12 (8 PCI addrs, lo16 then hi16) and set bit23. ChamID: reads 2 words (reply buffer lo/hi) into x:$3/x:$4 |
| $7A | $7B | $247 | bclr DCTR HINT (host ISR acknowledges interrupt) |
| $7C | $7D | bit1 | doorbell: host-command queue |
| $7E | $7F | bit2 | doorbell: T-MIX command queue |
| $80 | $81 | bit3 | doorbell: periodic status request |
| $82 | $83 | bit4 | doorbell: Z80#1 command queue |
| $84 | $85 | bit5 | doorbell: Z80#2 command queue |
| $86 | $87 | bit13 | host consumed the reply (sent from the driver DPC) |
(vector values for $60-$70 per DSP56301 HI32 vector order: ASSUMPTION.)

## 3. Firmware structure and command protocol
Cooperative kernel: x:$1 = event bits, task table at X:$417 (7 tasks x 25 words, entry/event-mask): $291 (bit0), $345 (bit1), $370 (bit2), $39B (bit3), $418 (bit4), $447 (bit5), $2D9 (bit9, always runnable: coefficient slewing x:$100..$17F->x:$208.., x:$188..->$290, y:$150..->y:$158 once per word-clock tick y:$5!=y:$6). `jsr $195`/`$193` = yield.

### Start-up handshake (P:$100..$150)
1. init HI32, enable HCIE. `jsr $594`: write $A98765 to DTXS (waits STRQ), then DCTR HF=%001 + HINT.
2. wait bit22 (HCVR $75) -> `$2FF`: T-MIX3 reg$66,$67 <- 4; T-MIX2 reg$61,$62 <- 4 (hold resets); read $9D07FF/$9D17FF; `$488` slave download: word count, DPRAM offset, then count words (one byte each, low 8 bits) written to BOTH DPRAMs ($9D0000|off and $9D1000|off) -- this is Z80OS.PCI (IOCTL A410EC54 in = 4 + 1024 bytes). Enable IRQC/IRQD; T-MIX2 $61,$62 <- 6; delay; T-MIX3 $66,$67 <- 6 (release Z80s); wait events 6 and 7 (both Z80 mailboxes answer).
3. `jsr $594` again (HF=%001 + $A98765), wait bit23 (HCVR $79 + 16 address halves).
4. `jsr $599`: $A98765 to DTXS, HF=%010 + HINT. Then scheduler loop.
Driver ISR (CHAM.sys $114D5): reads HF = (HSTR>>3)&7, writes HCVR $7B; HF 1/2: reads one HRXS word == $A98765 -> "ready"; HF 3: reads 3 words -> DPC(word0, word2<<16|word1).

### Host buffers (x:$3..x:$12, lo/hi pairs in the order the driver sends them)
| # | x: | dir | DSP buffer | queue |
|---|---|---|---|---|
| 1 | $3/$4 | read 16 w | X:$500 | host command |
| 2 | $5/$6 | write 16 w | X:$510 | host command readback |
| 3 | $7/$8 | read 16 w | X:$520 | T-MIX command |
| 4 | $9/$A | write 48 w (3 blocks) | X:$530 | periodic status |
| 5 | $B/$C | read 16 w | X:$560 | Z80#1 command |
| 6 | $D/$E | read 16 w | X:$570 | Z80#2 command |
| 7 | $F/$10 | write 16 w | X:$580 | Z80#1 readback |
| 8 | $11/$12 | write 16 w | X:$590 | Z80#2 readback |
Each command buffer is a single 64-byte block (16 dwords) at the queue base; no ring, one outstanding command per queue. Fetch = `$8DE` (y:$8 blocks, y:$9 DSP addr, y:$A table index, y:$7 bit0 = write).

### Per-command cycle (every queue)
doorbell -> DMA-read 16 words -> word0 = opcode, dispatch -> `$35B`: word2 status |= $200 if x:$0 != 0 -> `$99C`: send words 0,1,2 of the block via DTXS -> clear bit13 -> `$59F` HF=%011 + HINT -> `$4B8(#$D)`: busy-wait (no yield) for HCVR $87; timeout (~2^22 polls) sets x:$0 bit13 and T-MIX2 reg$63 <- 4.
Status word (word2): bits7-0 kept from host, bits15-8: 0 ok, 1 bad opcode, 2 failure (sticky x:$0 timeout bits or Z80 reply code 2); bits23-16: 1 = readback block was written.
Generic waits `$4BA(k)`: yield until x:$1 bit k (test-and-clear via table $706); timeout -> x:$0 bit k set.

### Host-command opcodes (block X:$500; params from $506)
0 delay; 1 $506 bit0 ? start audio kernel ($5AD: clear buffers, build DMA1 tables, enable IRQA) : stop IRQA ($6D4);
2 Poke56301: $506 space (0 P, 1 X, 2 Y), $507 addr, $508 value; 3 PokeIO: X:$301000+$506 <- $507;
4 set x:$1 bit11 (enable I/O-slot scan in status), clear PDRD bit3;
5 Peek56301: space $506, addr $507 -> readback {$510=$508, $511=$509, $512=1, $513=value} to buffer 2, status |= $10000;
6 PeekIO X:$301000+$506 -> readback ($510=$507, $511=$508);
7 T3$66<-4 / 8 T3$66<-6 / 9 T2$61<-4 / $A T2$61<-6 / $B T3$67<-4 / $C T3$67<-6 / $D T2$62<-4 / $E T2$62<-6 (reset hold/release, each + delay);
$F PokeBit: addr $507, bit $509, space $506 (1 X, 2 Y), $508 set/clear;
$10 bulk PokeIO: $505/2 pairs (addr,value) from $506 to X:$301000+addr.
Mixer coefficient targets live at X:$100-$17F, X:$188-$207, Y:$150-$157 (slewed by task $2D9) -> SetCoefficientMixer is a Poke56301 there (ASSUMPTION).

### T-MIX opcodes (X:$520; $526 chip 1..3, $527 reg/addr, $528.. data)
0 delay; 1 register: ctl pointer <- reg, data <- $528 ($4D0); 2 map: pointer <- $527, then data bytes from $528.. (3 bytes per word, bits 23-16,15-8,7-0; stops at a zero byte) ($536); 3 DSP-port (DPRAM) write: chip base $83C000/$83A000/$838000 + ($527&$3FF) <- $528 ($503).
T-MIX ctl ports: chip1 $9D2000 (pointer)/$9D2001 (data), chip2 $9D3000/1, chip3 $9D4000/1 (pointer write = reg byte; read with x1 bit23).

## 4. Z80 path (Z80#1: queue X:$560, DPRAM $9D0000, mailbox $9D07FE, reply $9D07FF/IRQC; Z80#2: X:$570, $9D1000, $9D17FE, $9D17FF/IRQD)
Params written into DPRAM from offset 3, then the mailbox byte = Z80 command; then `$C2F` waits event 6 (7). Reply code y:$3 (y:$4) &$FF == 2 -> status err 2.
| op | mailbox | DPRAM[3..] |
|---|---|---|
| 1 | 1 | [3]=$566&$FF (count), [4]=$566>>16, [5..6]=$567 (16-bit LE), [7..]=count bytes from $568.. (3 bytes/word, LSB first) -- LoadBulk |
| 2 | 2 | same header (count also saved to $582); after reply, count bytes from DPRAM[3..] packed into $583.., $580/$581 = $568/$569, block written to readback buffer 7 (8), status |= $10000 -- FetchBulkDump |
| 3 | 3 | $565 words from $566.. each as 2 bytes LE |
| 4,5 | - | rejected: status err 2 |
| 6 | 6 | [3]=$566, [4..5]=$567 16-bit |
| 7..$A | 7..$A | byte count $566, bytes from $567.. (3/word, LSB first) |
Mailbox 5 is also sent by the periodic-status task; the 32 DPRAM bytes from offset 3 are packed into X:$540 (Z80#2: X:$550).

Periodic status block (X:$530, 48 words -> buffer 4; reply words from X:$1E = {5,0,status}): $530=$123456, $535=x:$1 events, $536/$537 = measured periods (y:$b/y:$c), $538.. I/O slot IDs (if bit11: write $FF to $301FFF, read $301000+y:$10..), $53C=y:$5 word-clock count, $53D.. 8 packed counters from x:$29..$30 (+flags OR'ed into $53F), $540.. Z80#1 status, $550.. Z80#2 status.

## 5. ChamID.pci
- Init like ChamDSP, x:$72=$FFFFFF. Wait for HCVR $79 (vector $78, handler $318): 2 words lo16/hi16 of the reply buffer -> x:$3/x:$4.
- T-MIX3 $66,$67 <- 4; delay; T-MIX2 $61,$62 <- 4 (all held in reset).
- x:$73 = T-MIX2 reg $7A (read via $9D3000/1), x:$74 = T-MIX1 reg $7A ($9D2000/1), x:$75 = IDR.
- Master-write 16 words x:$73..$82 to buffer+4, then 1 word x:$72 ($FFFFFF, "done") to buffer+0.
- So host buffer dwords: [0]=$FFFFFF, [1]=T-MIX2 reg$7A (driver: bits7-6 = card type, 0 = Lexichip III), [2]=T-MIX1 reg$7A (bits5-4 = DB version), [3]=IDR ($2301), [4..16]= x:$76..$82 (zero/garbage).
- Then waits for vector $72 (HCVR $8073) -> jump to bootstrap ROM.
For the emulator: T-MIX reg $7A must read with bits7-6 = 0 on T-MIX2.

## Implications for the emulator (card.h ASSUMPTIONS)
- T-MIX resets are per-register writes (reg $61/$62 on T-MIX2, $66/$67 on T-MIX3; value 4 = hold, 6 = release, i.e. bit1 = reset-not), not a single SPO register $FF. ASSUMPTION: T3 $66 -> Z80#1 INIT, $67 -> Z80#2; T2 $61/$62 -> Lexichip/board resets for the two halves.
- The Z80 kernel is written to both DPRAMs by the 56301 itself.
