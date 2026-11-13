# 960L host helper

This runs the original 960L host software (`Framework.exe` and its DLLs from the V4.02 ISO) as a 32-bit helper
process. Its hardware is redirected to the emulator over TCP on 127.0.0.1:

| Port (default) | Original device | Emulated by |
|---|---|---|
| 7960 | `\\.\CHAM` (the CHAM.sys reverb-card driver) | `l960::ChamDevice` with the emulated card |
| 7961 | COM1, the LARC2 link | `l960::larc2::Larc2Device` |
| 7962 | COM2, the service terminal | logged |

`Enums.dll` is a proxy for the original `Enums.dll`, which is renamed `EnumsOrig.dll`. It patches the import
tables of every module in the process. It provides:

- **The virtual devices** listed above.
- **A virtual registry.** It serves `HKLM\Software\Lexicon\960L` and `\Cham`, so no import and no admin rights are needed.
  Set `L960_REAL_REGISTRY=1` to use the real registry instead.
- **Windows version.** It reports NT 4.0, because ChamAPI only drives the card on NT 4.
- **Faked service-manager calls** for loading the driver.
- **Serial-port semantics.** These match a real overlapped COM port: an empty overlapped read returns `ERROR_IO_PENDING`.
- **Log redirection.** `C:\Thunder.log` goes to the data folder.

## Normally: the plugin does it

The plugin embeds this DLL (`hosthelper/Enums.dll`, built with MinGW and checked in). On Windows it:

1. copies the 960L folder to `%APPDATA%\L960Emu\software\960L` and installs the hook there;
2. maps two free drive letters: one to `%APPDATA%\L960Emu\drive`, the 960L's writable "F:" drive, and one to
   `%APPDATA%\L960Emu\software`, the 960L's system drive. The host software opens
   `%SystemDrive%\960L\Larc2\larc2.rom` for the LARC2 version check and loads its algorithms from
   `%SystemDrive%\960L\App\Algorithms`, and it only has room for a drive letter there. The hook expands
   `%SystemDrive%` to `L960_SYSTEM_DRIVE`;
3. starts `Framework.exe` hidden, with the ports and folders in environment variables:
   - `L960_CHAM_PORT`, `L960_COM1_PORT`, `L960_COM2_PORT`
   - `L960_DATA_DIR`
   - `L960_DATA_DRIVE`, `L960_SYSTEM_DRIVE`

The logs are `%APPDATA%\L960Emu\Thunder.log` (the host's own) and `l960hook.log`.

## Build the hook

    i686-w64-mingw32-g++ -O2 -shared -static -static-libgcc -static-libstdc++ -o Enums.dll hook.cpp enums.def -lws2_32

With MSVC, build `hook.cpp` as a **32-bit** DLL with `enums.def` and link `ws2_32.lib`. To embed a different build in the
plugin, set CMake's `L960_HOOK_DLL` to it.

## Manual use

1. Copy the 960L folder somewhere. Rename `App\Enums.dll` to `EnumsOrig.dll` and put this `Enums.dll` there.
2. Map the writable drive, e.g. `subst F: C:\l960drive`, and set `L960_DATA_DRIVE=F:`. If the copy is not at
   `C:\960L`, also map its parent folder (`subst Y: D:\stuff`, where `D:\stuff\960L` is the copy) and set
   `L960_SYSTEM_DRIVE=Y:`. Otherwise the LARC2 reports "LARC2 firmware needs updating" and no programs load.
3. Start the emulator side: `l960_bridge <960L folder>` or the plugin.
4. Run `Framework.exe`.

Note: the host software needs the writable drive to be a drive letter. With a plain directory path it stalls
while loading its automation library.
