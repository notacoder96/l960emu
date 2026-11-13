# LARC2 link: why the session never started (and the fix)

## Root cause: hook ReadFile semantics (overlapped, no data)
LightningInterface.dll receive thread (0x10016460/0x10016490) reads COM1 one byte at a time with an OVERLAPPED:
    ok = ReadFile(h, &b, 1, &n, &ov)
    if (!ok || n != 1) {
        if (GetLastError() != ERROR_IO_PENDING) -> 0x1000179e(-1) = 0x10016430: CloseHandle + ExitThread  (FATAL)
        GetOverlappedResult(h, &ov, &n, TRUE)
    }
    if (n == 1) process byte (F0 start, F7 end -> checksum 0x10016670 -> queue 0x10016f50, FF escape)
Timeouts are I=MAXDWORD, M=MAXDWORD, C=250, so whenever no byte arrives within 250 ms our hook returned
TRUE with 0 bytes; GetLastError() was stale (not 997) and the receive thread exited. This happened right after
the first frame (cmd 10): the hook log shows exactly one frame read, then no further ReadFile calls.
Everything else (TX thread sending type 13, watchdog) kept running, so it looked like an idle exchange.

## Fix (hosthelper/hook.cpp, hkReadFile), after completeOverlapped(_ov, got):
    if (_ov && !got) { SetLastError(ERROR_IO_PENDING); return FALSE; }
(GetOverlappedResult hook then returns TRUE with 0 bytes and the loop continues.)
Verified under Wine: ReadFile keeps running, host sends type 3 (version), LARC2 answers cmd 2 `04 00 52`,
then screen/menu traffic follows; the emulated LARC2 LCD shows "B01 P1: Large Hall" with the parameter
row (ErlLvl, RvbLvl, MidRT, RtHicut, FrRoll, RearRoll, Size, Mix) and the 960L status panel
(lcd_session.png).

## Link-layer protocol notes (LARC2 -> host, byte0 = 5: handler 0x10015820, switch on cmd)
| cmd | meaning / host action |
|---|---|
| 2 | version reply, payload 3 bytes -> stored, SetEvent (completes type-3 request, 0x100158e0) |
| 6 | "looking for 960L" (boot ROM): link obj+0x30=1, host sends type 9 (0x100287f0) |
| 7 | ping reply: if obj+0x30 set -> obj+4=1 (alive) else host sends type 2 |
| 8 | boot ROM ready: host sends type 7 (boot application, 0x10028810) |
| 10 | CE app connection request: obj+0x30=1 (enables type-13 pings) |
Host -> LARC2 link types: 2, 3 version request (0x100287b0), 7 boot app, 9, 13 ping (0x100287d0, sent while obj+0x30).
Session start: watchdog thread 0x10013f30 (every 3 s) sees obj+4 set -> 0x10013fd0(1) -> posts event 16 ->
0x1000f190 -> 0x10014a40 version check (type 3, 5 s wait, compares with larc2.rom header) -> obj+0x6c44=1,
UI traffic starts. Frames with byte0 1..6 (cmd >= 0x93 for keys etc.) go through 0x10014d20 / 0x10014e30.
No change was needed on the LARC2 side; cmd 10 carries no payload and is correct.
