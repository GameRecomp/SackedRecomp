# Research

Findings about the games and their binaries, in our own words. Unknowns are marked **(open)**. Addresses may
appear here; disassembly, lifted or decompiled code never does.

## The media

### Sacked, German CD (`sd-gefeg.bin` + `.cue`)

- One data track, `MODE2/2352`: raw 2352-byte sectors, Mode 2 Form 1, 2048 bytes of user data at offset 24 of
  each sector. The user data is an ISO 9660 volume `GEFEUERT` (125,075,456 bytes as a cooked image).
- Contents: `Gefeuert_RC1.EXE` (the installer, also the `autorun.inf` target), `Gefeuert_RC1.ico`,
  `Gefeuert Website.url`, `DirectX/DX81{eng,ger,NTeng,NTger}.exe`, four wallpapers in `Desktop Hintergründe/`.
- The installer is NSIS, branded "Limbic Installer (NSIS)". 7-Zip 23.01 does not open it (an older layout than
  it knows). The layout (answered in M1; `rc-media/src/nsis.rs` describes it):
  - The first header is at file offset `0x8E00` (flags `0`, the `0xDEADBEEF` signature, `NullsoftInst`), header
    size 7944, data size 75,340,331, which ends exactly at the end of the file. No CRC flag.
  - The data starts with the header block: a 32-bit length with the top bit set for "compressed", then raw
    deflate (zlib without its wrapper), 7944 bytes once inflated. 175 blocks follow in the same form, the last
    one ending at the end of the file: one per installed file, in the order of the script.
  - The inflated header holds the string table (installer texts, directory and file names, registry keys) and
    the entry table. Directories are written as `$INSTDIR\…` with a one-byte variable code in front (`0xF3`
    `$INSTDIR`, `0xF6` `$PROGRAMFILES`, `0xF7` `$SMPROGRAMS`). The header is 60 words (entry count in word 13,
    section count in word 56), sections are five words (name, install types, first entry, entry count, KiB),
    entries six (opcode and five parameters). Opcodes used: 1 return, 14 create directory (second parameter set:
    also the output directory), 21 extract file (name, block offset from the end of the header block, file time),
    42 shortcut, 50 write registry (root, key, name, value, type 1 = string), 61 write uninstaller. Two entries may
    share a block. UI strings with umlauts collide with the variable codes (an old-NSIS limitation; only paths
    and registry values are interpreted here).
  - Registry: `HKLM\SOFTWARE\Limbic Entertainment\Gefeuert` `Install_Dir` = the install folder; an uninstall
    entry; start menu shortcuts.
  - Files: `CO_*.OGD`, `co_objects.dat`, `Gefeuert.exe`, `International.ttf`, `ogg.dll`, `stlport_vc645.dll`,
    `vorbis.dll`, `vorbisfile.dll`, `Levels/…`, `Sound/sounds.txt`, `Sound/FX/S*.wav` (91 RIFF files in all),
    `Sound/Musik/{Menu1,Menu2,Theme1,Theme2,Theme3}.ogg`.

### Crazy School (`CrazySchool.iso`)

- ISO 9660 volume `CRAZYSCHOOL`: `CrazySchool_Setup.exe`, `Autostart.exe` (the CD launcher), `liesmich.txt`,
  `DirectX/DX81*.exe` (the same four files as on the Gefeuert CD).
- The installer is Inno Setup 3.0.6.1 ("My Inno Setup Extensions", isx), one language. 204 files, 236 MB
  installed. File data in zlib blocks (`zlb\x1a`), one per file, each checked by Adler-32. Read by
  `rc-media/src/inno3.rs` (M1), byte-identical to innoextract 1.9.
- The installer writes `HKLM\SOFTWARE\LIMBIC\CRAZYSCHOOL` `INSTALL_DIR` = `{app}`; the game reads and writes
  `HKLM\SOFTWARE\Limbic Entertainment\Crazy School` itself.

### The other languages

| Game | Variant | Medium | Installer | Notes |
|------|---------|--------|-----------|-------|
| Sacked | `en` | NSIS exe | NSIS 1.9x, as German | `sacked.exe`: link time 2002-10-21, the German exe's layout (same entry, same sections), rebuilt with English texts |
| Sacked | `hu` | ISO 9660 | NSIS 1.9x | the English `sacked.exe` (same hash), Hungarian data |
| Sacked | `pl` | NSIS exe | NSIS 1.9x | own `sacked.exe` (2003-02-24, 479,232 bytes) |
| Sacked | `nl` | zip of the CD | Inno Setup 4.2.6, LZMA1, one solid chunk | `Ontslagen.exe` (2004-08-09) built with **MSVC 7.1**, linking `MSVCR71.dll` and `MSVCP71.dll` (shipped beside it) instead of MSVCRT and STLport. The CD carries SafeDisc driver files (`SECDRV.SYS`, `DrvMgt.dll`) for its autorun, but the game exe is plain (normal sections, no wrapper) |
| Sacked | `ru` | raw MODE1/2352 CloneCD image | InstallShield 6 (`DATA1.HDR`, `DATA1.CAB`, `DATA2.CAB`) | own `sacked.exe` (493,056 bytes) by 1C; the setup also installs InstallShield engine files beside the game |
| Crazy School | `ru` | raw MODE1/2352 CloneCD image | Windows Installer (InstallShield Developer 7): `Оторва в школе.msi` (code page 1251) + `data.cab` (40 MSZIP folders, 203 files) | own `crazyschool.exe` (2003-08-06, 348,160 bytes), the German Vorbis DLLs |
| Crazy School | `pl` | Inno exe | Inno Setup 4.2.2, three UI languages, LZMA1 solid chunk, executables with the 4.1.8 call filter | `CRAZYSCHOOL.exe` (2004-06-03, 798,720 bytes) built with **MSVC 7.1**, static CRT; own Vorbis DLLs (2003-11, static CRT) and an unused `vorbisenc.dll` |

The Vorbis DLLs of Sacked are the same in every language (one build); Crazy School has two builds (de/ru and pl).
Whether the localised game builds differ in code beyond their texts is **(open)** (M2: `recomp codediff`).

## Game time in scripted runs

- Sacked reads `GetTickCount` about ten times a frame in a level. The virtual clock (`RC_CLOCK=virtual`) moves
  0.1 ms a reading, so a frame lasted about 1 ms of game time and the level clock stayed at 00:00.
  `RC_CLOCK_FPS=N` (scenario `FPS=N`) makes every presented frame take at least 1/N s: at 60, 1200 frames in
  the first level move its clock by exactly 20 seconds (00:05 to 00:25). Scenarios that play a level through
  use it; the others keep the old model and their references.
- Sacked's first level in time mode, nothing pranked: the colleagues and the boss walk their rounds, and the
  level ends between frame 21000 (clock 05:17) and 24000 with the "angel" screen (buttons "Hauptmenü" and
  "Nochmal"). The game puts its pointer in the middle of that screen.
- Crazy School's first level in time mode, nothing pranked: still running at frame 30000 (clock 07:47). Whether
  the level has a time limit, or ends only through what the player does, is **(open)**.

## The browser download

- Both files are served by archive.org through a redirect (`archive.org/download/<item>/<file>` → a mirror
  `dnNNNNNN.ca.archive.org/0/items/<item>/<file>`) and honour `Range` requests (206).
- CORS (checked with Firefox, 2026-11-22): `/download/<item>/<file>` answers other sites only for text files
  (the mirror sends `Access-Control-Allow-Origin: *` for `.txt`, nothing for `.exe`, `.bin`, `.iso`). The path
  `/cors/<item>/<file>` answers every file with `Access-Control-Allow-Origin` set to the asking origin, without
  a redirect: the page downloads from there. A `Range` header makes the browser ask first (preflight), which
  fails; whole files are fetched. Measured: the Polish Crazy School installer (62 MB) took 4 minutes through
  `/cors/` from a headless Firefox, against 11 seconds through `/download/` with curl.
- Testing the player headless: the canvas is WebGL without `preserveDrawingBuffer`, so `toDataURL` reads an
  empty (transparent) buffer unless a frame was drawn in the same task. Sacked's menu animates and is caught by
  chance; Crazy School's menu is static (unchanged frames are not presented again) and reads as empty. A test
  page forces `preserveDrawingBuffer` through `HTMLCanvasElement.prototype.getContext` before the start.

## Gefeuert.exe (Sacked, German)

- PE32 GUI, linker 6.0 (MSVC 6), link time 2002-10-14, image base `0x400000`, entry `0x46230C`, **no
  relocations**. `.text` `0x401000` (0x649BB bytes), `.rdata` `0x466000`, `.data` `0x46D000` (0x257D4 virtual),
  `.rsrc` `0x493000`.
- The C runtime is the system's `MSVCRT.dll` (dynamic CRT, `/MD`) and the C++ library is STLport 4.5
  (`stlport_vc645.dll`, `_STL::basic_string`, the node allocator).
- DirectX through `DirectDrawCreateEx` (`IID_IDirectDraw7` present), `DirectInputCreateEx`
  (`IID_IDirectInput7`, `IID_IDirectInputDevice7`), DirectSound ordinal 1. No Direct3D interface IDs.

### Imports (151)

| DLL | # | Notes |
|-----|--:|-------|
| `MSVCRT` | 56 | CRT start-up (`__getmainargs`, `_initterm`, `_acmdln`, …), `malloc`/`new`, stdio, `sprintf`/`sscanf`, `_beginthreadex`, C++ EH (`__CxxFrameHandler`, `_CxxThrowException`, `__RTDynamicCast`, `type_info`), `_except_handler3` |
| `USER32` | 32 | one window, message loop, keyboard state and text entry (`ToAsciiEx`, `MapVirtualKeyExA`, `GetKeyboardLayout`) |
| `stlport_vc645` | 20 | `basic_string<char>`, `__node_alloc` |
| `GDI32` | 14 | `AddFontResourceA`, `CreateFontA`, `ExtTextOutA`, `GetTextExtentPoint32A`, `CreateDIBSection`, memory DCs |
| `WINMM` | 9 | the `mmio*` calls (WAV files) |
| `vorbisfile` | 7 | `ov_open_callbacks`, `ov_read`, `ov_info`, `ov_pcm_total`, `ov_pcm_seek`, `ov_raw_seek`, `ov_clear` |
| `KERNEL32` | 6 | everything else is in MSVCRT |
| `ADVAPI32` | 4 | `RegCreateKeyExA`, `RegQueryValueExA`, `RegSetValueExA`, `RegCloseKey` |
| `DDRAW`, `DINPUT`, `DSOUND` | 1 each | `DirectDrawCreateEx`, `DirectInputCreateEx`, `DirectSoundCreate` (#1) |

Missing from HoMM4Recomp's HLE layer (compared by name, 2026-11-22): `GetKeyboardLayout`, `MapVirtualKeyExA`,
`ToAsciiEx`, `mmioOpenA`, `mmioRead`, `mmioAscend`, `AddFontResourceA`, `DirectInputCreateEx`, and all of
`MSVCRT`.

## stlport_vc645.dll

- PE32 DLL, MSVC 6, 2002-09-05, base `0x10000000`, relocations present, `.text` 0x84552 bytes, 2062 exports
  (mangled C++), a section `STLPORT_` of 0x20 bytes.
- Imports `MSVCRT` (≈ 55: stdio including the data exports `_iob` and `__pioinfo`, `_get_osfhandle`, `_fstat`,
  `setlocale`, `_ecvt`/`_fcvt`, `_CIsinh`/`_CIcosh`, the `exception` class members, C++ EH), `MSVCP60` (3:
  `std::bad_alloc`), `KERNEL32` (file mapping, locale and string-type calls, `CompareStringA/W`,
  `EnumSystemLocalesA`).

## The Vorbis DLLs

Two builds, one per game, all MSVC 6, base `0x10000000`, with relocations:

| File | Gefeuert (2001-12-31) | Crazy School (2002-07-19) |
|------|-----------------------|---------------------------|
| `ogg.dll` | 48 KiB, 44 exports | 48 KiB, 44 exports |
| `vorbis.dll` | 100 KiB | 952 KiB (0xDA0D4 bytes of `.data`: static codebooks) |
| `vorbisfile.dll` | 28 KiB, 25 exports | 28 KiB, 25 exports (`vorbis_synthesis_trackonly` used) |

`ogg.dll` uses the statically linked CRT (50 kernel32 imports); `vorbis.dll` and `vorbisfile.dll` import
`MSVCRT` (`malloc`, `qsort`, `_CIpow`, `_CIacos`, `ldexp`, `frexp`, `fread`/`fseek`/`ftell`, `_errno`, `_ftol`).

## crazyschool.exe

- PE32 GUI, linker 6.0, 2003-08-13, image base `0x400000`, entry `0x43E778`, **no relocations**. `.text`
  `0x401000` (0x4BDCE bytes), `.rdata` `0x44D000`, `.data` `0x454000` (0x2A908 virtual), `.rsrc` `0x47F000`.
- C runtime statically linked (heap, environment, locale, `RaiseException`, `SetUnhandledExceptionFilter`,
  `IsBadReadPtr`/`IsBadWritePtr`/`IsBadCodePtr`, `CreateThread`/`ExitThread`).
- The same DirectX set as Gefeuert (DirectDraw 7, DirectInput 7, DirectSound #1), `vorbisfile` (the same 7
  functions), `mmio*`, GDI text (`CreateFontA`, `ExtTextOutA`) and `StretchDIBits`. Fonts of its own (`*.O2F`)
  and palettes (`CS_INGAME.PAL`, `CS_MENUE.PAL`, `CS_MINIGAME.PAL`, 768 bytes each).
- Missing from HoMM4Recomp's HLE layer: `GetKeyboardLayout`, `MapVirtualKeyExA`, `ToAsciiEx`, `mmioOpenA`,
  `mmioRead`, `mmioAscend`, `DirectInputCreateEx`.
- The readme asks for a "DirectX 8.1 compatible 3D card", but the executable uses no Direct3D interface: what the
  engine needs from the card (hardware blits, alpha) is **(open)**.
