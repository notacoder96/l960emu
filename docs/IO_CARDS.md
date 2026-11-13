# 960L I/O card identification (IO manager, LICSDLL.dll) — what the emulator must return

## Slot scan path
1. LICSDLL 0x10019230 (IO manager init, called from 0x100183d0):
   Poke56301(Y:$F = 0) -> wait until status slot count == 0;
   Poke56301(Y:$10+i = slotAddr[i]) for i=0..4 with slotAddr = {0x000,0x200,0x400,0x600,0x800};
   Poke56301(Y:$F = 5) -> wait (Sleep 10 loop) until status reports count 5.
   (vtable+8 of the card interface = Poke56301(unit 0, space 2=Y, addr, value)).
   The scan only runs while x:$1 bit11 is set = host opcode 4 (SetIOmaster), already sent at start-up.
2. ChamDSP status task (P:$3D6..$3F1): a = min(y:$F, 11); x:$536 = a; for each slot:
   write $FF to X:$301FFF (bus precharge), then read X:$301000 + y:$10[i]  -> x:$537+i.
   Then $6D7 packs x:$536.. 3 per word into $538..$53B (first byte <<16 NOT masked, 2nd &$FF00, 3rd &$FF)
   => every I/O bus read must return an 8-bit value (0..$FF), never wider.
3. Status block as the host receives it (48 dwords, block base X:$530):
   dword 8  = count<<16 | id0<<8 | id1
   dword 9  = id2<<16 | id3<<8 | id4
   dword 10/11 = ids 5..10
   ChamAPI::GetPeriodicStatus (0x10002160) unpacks: STATUS+8 = count (byte 2 of dword 8),
   STATUS+0xC.. = ids; STATUS+0x18 = dword 6, STATUS+0x1C = dword 7 (measured word-clock periods).
   Only card 0's status carries the slot data.
4. LICSDLL 0x100192f0 copies the ids to IOmgr+0x3C.. and sets IOmgr+0x38 = count (releases the wait).

## ID byte decode (0x100183d0, byte table 0x10018828 / jump table 0x10018810)
ID = type<<4 | revision. 5 slots.
| type nibble | card | limit | error if exceeded | supported revisions |
|---|---|---|---|---|
| 0 | Clock/MIDI (IOCLK) | exactly 1 | 0x258 "Missing or extra clock card" (2nd one; also 0x100188e0/0x10018ff1 when none) | any (manual default rev 1) |
| 1 | Analog In (AIN)  | 2 | 0x25B | 0 or 1 (else 0x25C "Unsupported Analog Input card version") |
| 2 | Analog Out (AOUT)| 2 | 0x25D | 0 or 1 (else 0x25E) |
| 3 | AES/EBU          | 3 | 0x259 | 1, 2, 3 (rev 0 has no I/O masks -> assignment fails; 0x25A for bad routing) |
| 4..14 | — | | "Nonsense value %02x in Slot %d", error -1 | |
| 15 | empty slot ($FF, bus pull-ups — manual p.7-6) | | | |
Our log "Clock/MIDI :: Interface 0 :: Slot 0" + 0x258: all slots read 0 => slot 0 = clock, slot 1 = 2nd clock => "extra".

After the loop: number of input cards (AIN + AES) <= 3 and output cards (AOUT + AES) <= 4, then each card
gets an interface from free masks (inputs: bits 2,1,0 -> interface 5,1,0 table 0x10035820; outputs: bits 3..0 ->
5,4,1,0 table 0x10035830). Per-card masks: AIN rev1 = 7 (rev0 = 1), AOUT rev1 = 0xD (rev0 = 1),
AES rev1 = in 6/out 6, rev2/3 = in 7/out 0xE. Failure to find a free bit (result 6) => error.

## Standard 960L configuration (manual: "One Analog Input, One Analog Output, One AES/EBU, One IO/Clock card,
one spare slot"; recommended slot order Table 1-1) — values for ReverbCard::busRead at X:$301000+addr:
| slot | address | card | ID ($301000+base) |
|---|---|---|---|
| 1 (top) | $000 | IO/Clock | $01 |
| 2 | $200 | empty | $FF |
| 3 | $400 | AES/EBU | $33 (manual: "Board I.D. register, hardwired to $33"; text elsewhere says $32 — both rev ok, use $33) |
| 4 | $600 | Analog In | $11 |
| 5 | $800 | Analog Out | $21 |
Empty slot / any unmapped address inside $301000-$3019FF: $FF.
=> inputs: AIN->iface 5, AES->iface 1; outputs: AES->iface 5, AOUT->iface 4 (slot order AES before AOUT).

## Card registers (PokeIO host op 3/$10 = write X:$301000+addr, PeekIO op 6 = read). Model as 8-bit latches
that read back what was written (diagnostics write walking-1 patterns and read them back), except ID at base+0.
- IO/Clock (base $000): +0 ID $01 (RO); +1 CTLREG: b7 OSC_DRIVES_BNC, b6 SEL_1X_CLKMODE (0=44.1/48k, 1=88.2/96k),
  b5:4 WCKSEL (0=48/96 int, 1=44.1/88.2 int, 2=BNC, 3=slot WCK), b3 PVW_WCK_SEL, b2 PWCLK_EN, b1 XTAL_EN (default 1),
  b0 TESTMODE. The emulator can derive the word-clock rate from WCKSEL/SEL_1X (0 -> 48k, 1 -> 44.1k, x2 when b6=1).
- AIN (base $600): +0 ID $11; +1 CTLREG b7:6 octal drive select (def 3), b5 AKM5394EN, b3 DFS1, b2 DFS0 (double speed),
  b1 CONV_RESET.
- AOUT (base $800): +0 ID $21; +1 CTLREG b6:5 octal select (def 3), b4 de-emphasis, b3 RELAY_MUTE/ (active low),
  b2 DAC_RESET/ (active low), b1 DAMUTE, b0 96K_EN.
- AES (base $400): +$00 ID $33; +$20 FPGA control (b7:6 slot WCK source AES in 1-4, b5:4 preview source,
  b3 slot WCK out en, b2 preview out en, b1 double speed, b0 loopback); +$40 lock register (b1 seek, b0 drive
  clocks); +$60 TX global chip select; +$80 octal select; +$100/$120/$140/$160 CS8412-type AES RX #1-4,
  +$180..$1E0 CS8402-type TX #1-4 (register files; latch them; receiver error/status registers should read 0 =
  locked / no errors when the AES input is "connected", or report loss of lock if no source).

## Next blocker after the IO manager: "Lost wordclock" (0x260, LICSDLL 0x100174c0/0x10017590)
LICSDLL 0x100198a0/0x100199c0 use STATUS+0x1C = status dword 7 (= DSP y:$C, timer-1 word-clock period):
fs = 40,000,000 / period; 0 => lost word clock (reported after 60 s). So the emulated 56301 must measure the
word clock: either emulate TCSR0/1 period capture (vectors $24/$28 fill y:$B/y:$C) or have the card model put
period = 40e6/fs (48 kHz -> 833, 44.1 kHz -> 907, 96 kHz -> 417) there. Dword 6 (y:$B) = second (external/
preview) clock period, same unit. The word clock itself (IRQA, $10) also drives the DSP audio kernel/status counter y:$5.
