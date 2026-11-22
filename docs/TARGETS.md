# Targets

This file lists every input the project accepts, pinned by SHA-256. It must stay in sync with
`games/<id>/game.toml` (from M0), which becomes the machine-readable source.

The recompiler refuses any binary whose hash is not listed. A new build (another language, another release)
is added here and to the manifest first. Only then is it lifted.

## Media

| Game          | Variant | File | Size (bytes) | SHA-256 |
|---------------|---------|------|-------------:|---------|
| `gefeuert`    | `cd-de` | `sd-gefeg.bin`    | 143641344 | `84d483e65bb506f5d832325815a95e04dc31d985d70dcf8ebaa18aee83c9e93e` |
| `gefeuert`    | `cd-de` | `sd-gefeg.cue` (optional) | 71 | `31f79237e37c81279dcaa54a72c4bf8e370a49eb84607918fbfa493a5ed50b01` |
| `crazyschool` | `cd-de` | `CrazySchool.iso` | 122435584 | `702652d03bbd3feaad521bc8ccfbce6f5a68741bc9d4840a73462db80a830a16` |

Source (abandonware, downloaded by the converter):

| Game | archive.org item | URL |
|------|------------------|-----|
| *Gefeuert! – Dein letzter Tag* (Limbic Entertainment, 2003, German) | `cdgefrtdlt` | `https://archive.org/download/cdgefrtdlt/sd-gefeg.bin` |
| *Crazy School – Die große Pause* (Limbic Entertainment, 2004, German) | `crazyschoolgame` | `https://archive.org/download/crazyschoolgame/CrazySchool.iso` |

The archive.org SHA-1s (`d683dfe6…` for the `.bin`, `9fa967bd…` for the `.iso`) match the files measured here.

### Installers inside the images

| Game | File in the image | Size (bytes) | SHA-256 | Format |
|------|-------------------|-------------:|---------|--------|
| `gefeuert`    | `Gefeuert_RC1.EXE`       | 75376683 | `b479ffde2b1c1a28403c52a150021c5a5c4b0dcc782857be40fc32032e3b2627` | NSIS, "Limbic Installer (NSIS)" (old build) |
| `crazyschool` | `CrazySchool_Setup.exe`  | 81644809 | `0d69a7c88f9639d9e5b02613428144afe7ce8aabede663ff2405689d58b09111` | Inno Setup 3.0.6.1 (isx), zlib |

## Binaries

| Game | File | Size (bytes) | SHA-256 | Treatment |
|------|------|-------------:|---------|-----------|
| `gefeuert`    | `Gefeuert.exe`      | 487424 | `f1ddc93d9fa17ed9db4199139c58a19aea71e8e77317ed60085a518f1715d05d` | lift (main) |
| `gefeuert`    | `stlport_vc645.dll` | 819200 | `4db465e6e42e9682822d014466b92715c803b1d0b79945b18c397fbd6ba770ef` | lift |
| `gefeuert`    | `ogg.dll`           |  49152 | `a391e1e2dbbb11f1c5041482223e8b014530649bccbf6c2cb95d9f173981445f` | lift |
| `gefeuert`    | `vorbis.dll`        | 102400 | `b594b271958676d3526e17db686e02d0f7e8ecb380582ffc8b9e09001d4a6801` | lift |
| `gefeuert`    | `vorbisfile.dll`    |  28672 | `e5fd294664e08b7220cc4a620295594082861220ad6a805b92096e98c3a3905e` | lift |
| `crazyschool` | `crazyschool.exe`   | 417792 | `291ba2aa7563e3cecc4805deafc1998e7733bc0d171de349e794b06b554e5d1b` | lift (main) |
| `crazyschool` | `ogg.dll`           |  49152 | `308540dbd488f3bceca2dbadefe02cf29d10a27c4ac096bb3da053e3e0b923ea` | lift |
| `crazyschool` | `vorbis.dll`        | 974848 | `b4fa55cfe7547ade0a2d5b800ef085ce20cdd71f61898d2461ea61eb0241812b` | lift |
| `crazyschool` | `vorbisfile.dll`    |  28672 | `f44472c6d9a64045c14583d12c0cfab5b4aa268aceb8bc9e3e1236b3008306f2` | lift |

`MSVCRT.dll` and `MSVCP60.dll` (imported by Gefeuert's binaries and all Vorbis DLLs) are Windows system DLLs, not
part of either game: HLE.

Skipped by name (not pinned): `DirectX/DX81*.exe` (DirectX 8.1 redistributable), `Autostart.exe` (Crazy School's
CD launcher), the uninstaller NSIS writes, `Desktop Hintergründe/*.bmp`, `autorun.inf`, `*.ico`, `*.url`.

## Data files (not pinned, read by the game)

- Gefeuert: `CO_BACKGROUND.OGD`, `CO_CHARS.OGD`, `CO_EFFECT.OGD`, `CO_GUI.OGD`, `CO_OBJECTS.OGD`,
  `co_objects.dat`, `International.ttf`, `Levels/LEVEL_nn.col`, `LEVEL_nns.col`, `Level_nn.txt` (00–19),
  `Uberschriften.txt`, `Sound/sounds.txt`, `Sound/FX/S*.wav`, `Sound/Musik/*.ogg`.
- Crazy School: `CS_*.OGD`, `CS_*.PAL`, `*.O2F` (fonts), `CS_OBJECTS.DAT`, `HIGHSCORE.DAT`, `NAMES.DAT`,
  `Levels/Level_nn.col`, `Level_nns.col`, `Level_nn.txt` (00–20), `Sound/sounds.txt`, `Sound/FX/*.wav`,
  `Sound/Musik/*.ogg`.
