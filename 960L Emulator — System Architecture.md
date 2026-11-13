# 960L Emulator — System Architecture

Oct 18, 2026 · Written by Claude. 

## Overview

The emulator recreates the whole Lexicon 960L as a plugin. The original 960L software runs unmodified; only the hardware under it is emulated. Presets, parameter maths, menus, the mixer and the algorithms all come from Lexicon's own V4.02 code, running on emulated chips.

A real 960L has three computers, and the emulator keeps all three:

- **Host PC.** An embedded NT4 PC running `Framework.exe` and its DLLs. It runs the system, computes every coefficient and drives the remote.
- **Reverb card.** A PCI card ("Chameleon") with a Motorola DSP56301, two Z80s, four Lexichip III reverb processors, three T-MIX routing chips and an I/O bus.
- **LARC2 remote.** A StrongARM SA-1100 running Windows CE. It is the only user interface.

In the emulator, the host software runs as a 32-bit helper process. A hook DLL redirects its hardware to the plugin over local TCP. The plugin process holds the emulated reverb card, the emulated LARC2 and the audio engine.

Five kinds of work run side by side:

- **Audio:** the plugin's audio thread runs the Lexichips.
- **Control:** the card's 56301 and Z80s execute the host software's commands on the bridge and idle threads.
- **User interface:** the LARC2 runs on its own thread; its screen and controls are the plugin's editor.
- **Host:** the original host software runs in its own process.
- **Plumbing:** bridge threads carry device traffic between the host software and the emulated hardware.

## System diagram

&#91;embedded content: System architecture · 2 processes, 1 TCP bridge\]

Everything the host software believes is hardware ends at the hook DLL and crosses one local TCP bridge. On the plugin side, ChamDevice and Larc2Device stand where the PCI card and the remote's serial cable would be.

## Component reference

Each box in the diagram above is one class or process. The core library (`core/`, no JUCE) holds all the emulation; the plugin only wires it to a DAW and a window.

| Component | What it is | Source | Talks to |
| --- | --- | --- | --- |
| Plugin processor | JUCE `AudioProcessor`. Owns the Engine, the LARC2 and HostSystem; starts the helper once the LARC2 has booted | `plugin/PluginProcessor.*` | DAW, Engine, HostSystem, Larc2Device |
| Editor / remote | The window: built-in LARC2 panel or an image skin, plus output-pair, levels, restart | `plugin/PluginEditor.*`, `Larc2Component.*`, `Larc2Skin.*` | Larc2Device (keys, faders, joystick, LCD frames) |
| Engine | Audio per sample: T-MIX routing, the 56301 mixer kernel (high-level), the four Lexichips | `core/engine.*` | ReverbCard (under the card lock) |
| HostSystem | Glue: starts the bridge, the CHAM device and the idle thread; prepares and launches the helper | `core/hostsystem.*` | HostBridge, ChamDevice, Larc2Device, helper process |
| HostBridge | TCP server on 127.0.0.1, ports base+0..2 (default 7960-7962) | `core/hostbridge.*` | Hook DLL in the helper |
| ChamDevice | Re-implementation of the NT driver `CHAM.sys`: 10 IOCTLs, command queues in a 4 KB PCI common buffer, interrupt and DPC handling | `core/chamdevice.*` | HostBridge, ReverbCard's 56301 (HI32) |
| ReverbCard | The Chameleon card: external bus decode, reset lines, Z80 mailboxes | `core/card.*` | DSP56301, Z80s, Lexichips, T-MIX, IoBus |
| DSP56301 | dsp56300 JIT core plus our HI32 PCI interface, PCI master DMA, host commands, timers | `core/dsp56301.*`, `patches/` | ChamDevice (host side), card bus |
| Z80 x2 | Real `Z80OS.PCI` kernel on the floooh/chips core; program the Lexichips | `core/z80unit.*` | Dual-port RAM mailboxes, two Lexichips each, banked SRAM |
| Lexichip III x4 | The reverb processors: microcode, delay memory, modulation units, serial slots | `core/lexichip.*` | Z80 bus, Engine's serial audio |
| T-MIX x3 / IoBus | Routing maps and ping-pong RAM; the I/O backplane with clock, AES, analog in and out cards | `core/card.*` | 56301, Engine |
| Larc2Device / Larc2System | The remote: ARMv4 core, SA-1100 peripherals, FPGA (keys, faders, joystick), LCD, host-link UART | `core/larc2/*` | HostBridge (COM1 bytes), editor |
| Helper process | Unmodified `Framework.exe` + host DLLs + algorithm DLLs, 32-bit | 960L V4.02 CD | Its devices, through the hook |
| Hook DLL | Proxy `Enums.dll`: virtual `CHAM` device and COM ports, virtual registry, NT 4 version, `%SystemDrive%` and log redirection | `hosthelper/hook.cpp` | HostBridge over TCP |

## How the parts interact

### Start-up (about 30 s on first run)

1. **Firmware folder chosen.** The plugin loads the 960L folder and starts the LARC2 thread. The LARC2 boot ROM runs its self-test and finds no host yet, so `Larc2Device` answers the boot handshake itself ("looking for 960L" → boot application). Windows CE starts and the LARC2 application asks for a host connection.
2. **Bridge up.** HostSystem opens TCP ports 7960-7962 (the next free set of ten for a second instance) and the idle thread.
3. **Helper launched** once the LARC2 is linked. HostSystem copies the 960L folder into the data folder and installs the hook. It maps two drive letters (writable drive, system drive) and starts `Framework.exe` hidden in a job object, so it dies with the plugin.
4. **Driver load.** `CreateFile(\\.\CHAM)` reaches ChamDevice. ChamDevice boots `ChamID.pci` on the 56301, which DMAs the card identity into the common buffer.
5. **Card bring-up** through IOCTLs:
   1. Download `ChamDSP.pci`.
   2. Download `Z80OS.PCI`. The 56301 copies it into both dual-port RAMs and releases the Z80s, which answer through their mailboxes.
   3. InitializeQueues.
6. **Unit start-up.** The host runs its SRAM diagnostics on both Z80s (16 fill/verify blocks).
7. **I/O manager.** The host has the 56301 scan the I/O bus slots. The IDs come back in the periodic status block: clock card, AES, analog in, analog out. The host then programs the T-MIX maps and the mixer coefficients.
8. **LICS and session restore.** The host loads the last program: microcode, delay addresses and coefficients go to the Lexichips via Z80 commands.
9. **LARC2 session.** The host checks the LARC2 version against `Larc2/larc2.rom`, then draws the screens.

### A fader move, end to end

1. The user drags fader 3 in the editor. `Larc2Device::setFaderTouched` and `setFader` set the emulated FPGA's touch bit and A/D value.
2. The LARC2 application's A/D scan reads it and sends a fader message over its UART (COM1 to the host software, at 57600 baud).
3. `Larc2Device` passes the bytes to HostBridge, which sends them to the hook in the helper, where `ReadFile(COM1)` returns them to `LightningInterface.dll`.
4. LICS computes the new parameter value and coefficients. It sends the new text to the LARC2 the same way back.
5. It also calls ChamAPI. ChamAPI issues Z80 commands as `DeviceIoControl` IOCTLs, which the hook forwards to ChamDevice.
6. ChamDevice posts each command into the queue in the common buffer and rings the 56301's doorbell (host command vector).
7. The 56301 firmware DMAs the command in and writes it into a Z80's dual-port RAM, then raises the Z80 mailbox.
8. The Z80 kernel writes the new coefficients into its Lexichips and replies. The 56301 sends a completion message, and ChamDevice's interrupt handler returns the event slot to the hook, which signals ChamAPI's wait event.
9. The next audio block uses the new coefficients.

### Audio, every sample

1. Plugin inputs 1/2 enter as analog input card channels. They are written into T-MIX 1's ping-pong RAM where its map says.
2. The mixer kernel (the 56301's audio code, re-implemented) mixes inputs into machine sends with the host's slewed coefficients. The sends are written to T-MIX 3.
3. T-MIX 3 feeds each Lexichip's serial inputs. Chips 1+2 (machine A) and 3+4 (machine B) run their programs and exchange signal through their chip-to-chip slots.
4. Lexichip outputs return through T-MIX 3. The mixer applies return gains and the output mix, including the stereo fold-down, and writes to T-MIX 2.
5. T-MIX 2's map sends them to the analog output card. The plugin takes the pair chosen in the editor (default outputs 7/8).

### The LARC2 screen

The host software sends screen objects (text, soft-key labels, lists, meters) as LARC2 link frames. `Larc2Device` paces them into the emulated UART at the link's real 57600 baud, otherwise the CE serial driver drops data. The CE application draws them into the SA-1100's frame buffer. The editor copies the 640 x 240 LCD 30 times a second. Keys go the other way as key-down/up frames.

## Threads, timing and locking

Two locks keep the shared state safe. The card lock is held while the audio thread or a control-processor slice touches the Lexichips. The CHAM lock serialises IOCTLs and idle slices on the one 56301.

| Thread | Runs | Paced by | Locks |
| --- | --- | --- | --- |
| DAW audio thread | `Engine::process`: routing, mixer, 4 Lexichips per sample | The DAW's buffer size | Card lock, held for the whole block |
| Bridge CHAM thread | Each IOCTL: post the command, then run 56301 + Z80s in slices until it completes (up to 4 s) | Host requests | CHAM lock; card lock per slice |
| Idle thread | 20 000 56301 instructions every 20 ms while the card runs (status, coefficient slewing) | Timer | CHAM lock; card lock per slice |
| Bridge COM1 / COM2 threads | Bytes host → LARC2; terminal log | Socket reads | LARC2 lock (inside `hostInput`) |
| LARC2 thread | ARM emulation at about 206 M instructions/s of wall time; LCD frame every 33 ms | Wall clock | LARC2 lock |
| Message thread | Editor, timers, helper launch | JUCE | Engine spin lock when firmware reloads |
| Helper process | The 960L host software, about 20 threads of its own | Itself | (separate process) |

- **Slices.** The card runs in slices of 2000 56301 instructions plus 2000 Z80 T-states per Z80. The Z80/56301 clock ratio is an assumption, chosen so the host's start-up SRAM test finishes well inside its 45 s limit.
- **Word clock.** The 56301's timers 0/1 are fed a period of 40 MHz / sample rate each slice, which is what the host reads as the word clock. 48 kHz is the default.
- **Audio clock.** The Lexichips run once per plugin sample, so the card follows the DAW's rate. There is no resampling to the card's own 48 kHz yet.
- **LARC2 link pacing.** Host-to-LARC2 bytes are delivered at one byte per 57600-baud character time of emulated CPU time.

## Data, files and configuration

The only input the user supplies is the `960L` folder from the V4.x CD. Everything else is generated or embedded.

| Item | Where | Used by |
| --- | --- | --- |
| 960L software | The folder the user picks | Firmware loader (`Cham/*.pci`, `Z80OS.PCI`, `Larc2/boot.rom`, `larc2.rom`); copied for the helper |
| Software copy | `%APPDATA%/L960Emu/software/960L`, refreshed when `Framework.exe` changes | The helper process; the hook is written into `App/Enums.dll` on every launch |
| Writable drive | `%APPDATA%/L960Emu/drive`, mapped to a free letter (the 960L's "F:") | Presets, sessions, automation |
| System drive | `%APPDATA%/L960Emu/software`, mapped to a second free letter | `%SystemDrive%/960L/...` paths: algorithm DLLs, `larc2.rom` version check |
| Logs | `%APPDATA%/L960Emu/Thunder.log` (host), `l960hook.log` (hook) | Diagnosis |
| Hook DLL | `hosthelper/Enums.dll`, embedded in the plugin at build time | Installed into the software copy |
| Plugin state | DAW session | Firmware folder, skin folder, wet/dry, levels, output pair |
| Skins | Any folder with `skin.json` + images | Editor (`docs/SKINS.md`) |

**Environment for the helper**, set only in its own environment block:

| Variable | Meaning | Default |
| --- | --- | --- |
| `L960_CHAM_PORT` / `L960_COM1_PORT` / `L960_COM2_PORT` | Bridge ports | 7960 / 7961 / 7962 |
| `L960_DATA_DIR` | Data folder (logs) | `<software root>/Data` |
| `L960_DATA_DRIVE` | Value of the host's `WRITEABLE_DRIVE` | the data folder |
| `L960_SYSTEM_DRIVE` | What `%SystemDrive%` expands to | unchanged |
| `L960_REAL_REGISTRY` | Use the real registry instead of the virtual one | off |
| `L960_HOOK_VERBOSE` | Log every IOCTL and serial read | off |

The virtual registry serves `HKLM/Software/Lexicon/960L` (writable drive, LARC2 on COM1, terminal on COM2) and `HKLM/Software/Lexicon/Cham` (firmware paths, command delays). Off Windows, `L960_HELPER_CMD` names a command that starts the helper, for example under Wine.

## Status and fidelity

The whole chain works on the original software. The weakest parts are the Lexichip's undocumented semantics and the high-level mixer.

| Part | How faithful | Open points |
| --- | --- | --- |
| Host software | Unmodified original | Needs a 32-bit Windows runtime |
| CHAM driver | Re-implemented from the `CHAM.sys` disassembly | One card only |
| 56301 | Real firmware on the dsp56300 JIT; HI32, DMA and timers modelled where the firmware uses them | Audio kernel (IRQA + DMA channel 1) is replaced by a C++ equivalent |
| Z80s | Real kernel on a cycle-level core | Clock ratio to the 56301 assumed |
| Lexichips | Rules derived from LexDis.dll, the Z80 kernel and captured programs | Rounding, saturation and slewing rules not verified against hardware; output level about -40 dBFS on impulses |
| T-MIX / I/O bus | Map semantics from 960LDiags; standard 960L card fit | AES card is silent; no sample-rate conversion |
| LARC2 | Real boot ROM and CE application | Motor faders and LEDs not shown |

**Detailed notes** (in the source tree): `docs/CHAM_SPEC.md` (driver), `docs/DSP_SPEC.md` (56301 firmware), `docs/Z80_UNITS.md` (start-up diagnostics), `docs/IO_CARDS.md` (I/O bus), `docs/AUDIO_PATH.md` (T-MIX + mixer), `docs/LEXICHIP.md` (reverb processor), `docs/LARC2_LINK.md` (remote protocol), `docs/SKINS.md` (editor skins), `hosthelper/README.md` (helper).
