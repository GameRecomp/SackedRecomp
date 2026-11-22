# Research

Findings about the games and their binaries, in our own words. Unknowns are marked **(open)**. Addresses may
appear here; disassembly, lifted or decompiled code never does.

## The media

### Gefeuert (`sd-gefeg.bin` + `.cue`)

- One data track, `MODE2/2352`: raw 2352-byte sectors, Mode 2 Form 1, 2048 bytes of user data at offset 24 of
  each sector. The user data is an ISO 9660 volume `GEFEUERT` (125,075,456 bytes as a cooked image).
- Contents: `Gefeuert_RC1.EXE` (the installer, also the `autorun.inf` target), `Gefeuert_RC1.ico`,
  `Gefeuert Website.url`, `DirectX/DX81{eng,ger,NTeng,NTger}.exe`, four wallpapers in `Desktop Hintergründe/`.
- The installer is NSIS, branded "Limbic Installer (NSIS)". 7-Zip 23.01 does not open it (an older layout than
  it knows). What is seen so far:
  - The first header is at file offset `0x8E00` (flags `0`, the `0xDEADBEEF` signature, `NullsoftInst`), header
    size 7944, data size 75,340,331, which ends exactly at the end of the file. No CRC flag.
  - The data starts with the header block: a 32-bit length with the top bit set for "compressed", then raw
    deflate (zlib without its wrapper), 7944 bytes once inflated. 175 blocks follow in the same form, the last
    one ending at the end of the file: one per installed file, in the order of the script.
  - The inflated header holds the string table (installer texts, directory and file names, registry keys) and
    the entry table. Directories are written as `$INSTDIR\…` with a one-byte variable code in front. The opcode
    numbering of this NSIS version is **(open)**; the strings point to the 1.9x–2.0 beta era.
  - Registry: `HKLM\SOFTWARE\Limbic Entertainment\Gefeuert` `Install_Dir` = the install folder; an uninstall
    entry; start menu shortcuts.
  - Files: `CO_*.OGD`, `co_objects.dat`, `Gefeuert.exe`, `International.ttf`, `ogg.dll`, `stlport_vc645.dll`,
    `vorbis.dll`, `vorbisfile.dll`, `Levels/…`, `Sound/sounds.txt`, `Sound/FX/S*.wav` (91 RIFF files in all),
    `Sound/Musik/{Menu1,Menu2,Theme1,Theme2,Theme3}.ogg`.

### Crazy School (`CrazySchool.iso`)

- ISO 9660 volume `CRAZYSCHOOL`: `CrazySchool_Setup.exe`, `Autostart.exe` (the CD launcher), `liesmich.txt`,
  `DirectX/DX81*.exe` (the same four files as on the Gefeuert CD).
- The installer is Inno Setup 3.0.6.1 ("My Inno Setup Extensions", isx), one language. 204 files, 236 MB
  installed. File data in zlib blocks (`zlb\x1a`). innoextract 1.9 reads it; `rc-media` (Inno 5.5/5.6 only) does
  not yet.
- The installer's registry entries **(open)**; the game reads and writes
  `HKLM\SOFTWARE\Limbic Entertainment\Crazy School` itself.

## The browser download

- Both files are served by archive.org through a redirect (`archive.org/download/<item>/<file>` → a mirror
  `dnNNNNNN.ca.archive.org/0/items/<item>/<file>`) and honour `Range` requests (206).
- From `curl` with an `Origin` header, neither the redirect nor the mirror answered with
  `Access-Control-Allow-Origin` (2026-11-22). Whether a browser's request gets CORS headers is **(open)**;
  checked in M1 with a real browser.

## Gefeuert.exe

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
