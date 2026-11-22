# Targets

This file lists every input the project accepts, pinned by SHA-256. It must stay in sync with
`games/<id>/game.toml`, the machine-readable source (which also holds each medium's download URL).

The recompiler refuses any binary whose hash is not listed. A new build (another language, another release)
is added here and to the manifest first. Only then is it lifted.

All releases below are abandonware archived on archive.org. The converter downloads the medium of the
language the user picks from the exact URL given here, checks its size and SHA-256, and builds from it; a local
copy of the same file is accepted too.

## Media

### `sacked` – Sacked: Your Last Day / Gefeuert! – Dein letzter Tag (Limbic Entertainment, 2003)

| Variant | Release | Download (exact URL) | Size (bytes) | SHA-256 | What it is |
|---------|---------|----------------------|-------------:|---------|------------|
| `cd-de` | German CD, *Gefeuert! – Dein letzter Tag* | https://archive.org/download/cdgefrtdlt/sd-gefeg.bin | 143641344 | `84d483e65bb506f5d832325815a95e04dc31d985d70dcf8ebaa18aee83c9e93e` | raw MODE2/2352 CD image (its `.cue`, https://archive.org/download/cdgefrtdlt/sd-gefeg.cue, is not needed); NSIS installer `Gefeuert_RC1.EXE` on it |
| `en` | English, *Sacked: Your Last Day* | https://archive.org/download/sacked_your_last_day_pc/Setup.exe | 75160974 | `e77cb450636de1f6d6031856a2e67de5e8adb4178ec9e2bf3c59d6f506070f89` | NSIS installer (the same file is in item `sacked_202610` as `Sacked.EXE`) |
| `hu` | Hungarian CD, *Ki vagy rúgva!* | https://archive.org/download/ki-vagy-rugva/SACKED.ISO | 75585536 | `685a8f35d77fa67e368a54fa81d1e065b361f00cc38fe2b46c3bb060b948bbaf` | ISO 9660 image; NSIS installer on it |
| `pl` | Polish, *Zemsta Urzędasa* | https://archive.org/download/zemsta-urze-dasa-1/ZEMSTA%20URZ%C4%98DASA%20%281%29.exe | 75325442 | `c44c49a86c8d876cb92ebea447379aefd4aa6d0618eec4f961553d7e14ac0f57` | NSIS installer (file name `ZEMSTA URZĘDASA (1).exe`) |
| `nl` | Dutch CD, *Ontslagen: Neem wraak op je laatste werkdag* | https://archive.org/download/ontslagenneemwraakopjelaatstewerkdag/cd.zip | 59020475 | `5587efe35f966aa4a55a5cc993bd8c7b811f76b751a586e6d594c8ca47ea7d11` | zip of the CD's files; Inno Setup 4.2.6 installer `Ontslagen Setup.exe` in it |
| `ru` | Russian CD by 1C, *Оторва!* | https://archive.org/download/sacked_202601/Sacked.img | 138601008 | `bf9c851d4c1a02acbb62019a680400cb70579fbb5e87795e3df3d1031dd8fa00` | raw MODE1/2352 CloneCD image (`.ccd`, `.sub`, `.cue` beside it not needed); InstallShield 6 setup with two cabinet volumes |

Also seen, not pinned: the Dutch CD as an Alcohol image (https://archive.org/download/ontslagencdmdf/New.mdf,
788,886,672 bytes, SHA-1 `4f97d8caaa91a65978e71c01b51a4d1596f039e9`; `recomp extract` reads it, but it is 13
times larger than the zip) and the Russian 1C CDs as RAR archives (item `otorva-plus-v-shkole-1c-russ`; RAR is
not read, the CloneCD images above hold the same releases). The German demo (item `gefeuertgamedemos`) is not
supported.

### `crazyschool` – Crazy School – Schulverweis! (Limbic Entertainment, 2003/2004)

| Variant | Release | Download (exact URL) | Size (bytes) | SHA-256 | What it is |
|---------|---------|----------------------|-------------:|---------|------------|
| `cd-de` | German CD | https://archive.org/download/crazyschoolgame/CrazySchool.iso | 122435584 | `702652d03bbd3feaad521bc8ccfbce6f5a68741bc9d4840a73462db80a830a16` | ISO 9660 image; Inno Setup 3.0.6.1 (isx) installer `CrazySchool_Setup.exe` on it |
| `ru` | Russian CD by 1C, *Оторва в школе* | https://archive.org/download/2_20240726_20240726_0734/Crazy%20School.img | 98442960 | `1624b02ce7ec7c7244de69ed54a80d529c142d5b95592b3d5c293a1bec2ea79d` | raw MODE1/2352 CloneCD image (file name `Crazy School.img`; `.ccd`, `.sub`, `.cue` not needed); Windows Installer package (`.msi` + `data.cab`) on it |
| `pl` | Polish, *Zemsta kujona* (PC FUN cover CD) | https://archive.org/download/setup_20200415/setup.exe | 61869354 | `e6a54cc469b8ce7eb86cdb357b25e169862d46cd6f502ebe9e352768d93aea8a` | Inno Setup 4.2.2 installer (`CS_POL_SETUP`); the item's `iso.iso` (694 MB) is the whole cover CD and not needed |

The archive.org SHA-1s of every pinned file match the files measured here.

## Binaries

### `sacked`

| File | Variants | Size (bytes) | SHA-256 | Treatment |
|------|----------|-------------:|---------|-----------|
| `Gefeuert.exe`      | cd-de | 487424 | `f1ddc93d9fa17ed9db4199139c58a19aea71e8e77317ed60085a518f1715d05d` | lift (main) |
| `sacked.exe`        | en, hu | 487424 | `3d2b86d99ed5bf915aade9c0cda6a0ea5cafa6c0df6b1d845c0935617f78d437` | lift (main) |
| `sacked.exe`        | pl | 479232 | `6404096cc74e1c954daa2033a7d7d7826429a27deaa679f1ff7103027f614a86` | lift (main) |
| `Ontslagen.exe`     | nl | 479232 | `93724b2ec5edb710cc85e18d84c07a28958ba056287de3d73880a00554e1a440` | lift (main) |
| `sacked.exe`        | ru | 493056 | `e22fd18077c30dd9d5636771f4f5a2097bfbe3669e305bc161883727ad44e6d1` | lift (main) |
| `stlport_vc645.dll` | cd-de, en, hu, pl, ru | 819200 | `4db465e6e42e9682822d014466b92715c803b1d0b79945b18c397fbd6ba770ef` | lift |
| `ogg.dll`           | all | 49152 | `a391e1e2dbbb11f1c5041482223e8b014530649bccbf6c2cb95d9f173981445f` | lift (base `0x10200000`) |
| `vorbis.dll`        | all | 102400 | `b594b271958676d3526e17db686e02d0f7e8ecb380582ffc8b9e09001d4a6801` | lift (base `0x10300000`) |
| `vorbisfile.dll`    | all | 28672 | `e5fd294664e08b7220cc4a620295594082861220ad6a805b92096e98c3a3905e` | lift (base `0x10400000`) |
| `msvcr71.dll`       | nl | 348160 | `8094af5ee310714caebccaeee7769ffb08048503ba478b879edfef5f1a24fefe` | lift |
| `msvcp71.dll`       | nl | 499712 | `df96156f6a548fd6fe5672918de5ae4509d3c810a57bffd2a91de45a3ed5b23b` | lift |

`MSVCRT.dll` and `MSVCP60.dll`, which every build but the Dutch one imports (directly or through STLport and the
Vorbis DLLs), are Windows system DLLs, not part of the games: HLE.

### `crazyschool`

| File | Variants | Size (bytes) | SHA-256 | Treatment |
|------|----------|-------------:|---------|-----------|
| `crazyschool.exe` | cd-de | 417792 | `291ba2aa7563e3cecc4805deafc1998e7733bc0d171de349e794b06b554e5d1b` | lift (main) |
| `crazyschool.exe` | ru | 348160 | `9ac9c1e2ef85b496c08217340b718abb49af32fed5ec3d91cc3b6be02bb8a826` | lift (main) |
| `CRAZYSCHOOL.exe` | pl | 798720 | `d7ec2e7a5dc0b807f40d74f56e0dedb9265c374ff79d4d5ab680eebccedb2822` | lift (main) |
| `ogg.dll`         | cd-de, ru | 49152 | `308540dbd488f3bceca2dbadefe02cf29d10a27c4ac096bb3da053e3e0b923ea` | lift |
| `vorbis.dll`      | cd-de, ru | 974848 | `b4fa55cfe7547ade0a2d5b800ef085ce20cdd71f61898d2461ea61eb0241812b` | lift (base `0x10100000`) |
| `vorbisfile.dll`  | cd-de, ru | 28672 | `f44472c6d9a64045c14583d12c0cfab5b4aa268aceb8bc9e3e1236b3008306f2` | lift (base `0x10200000`) |
| `ogg.dll`         | pl | 36864 | `4a4f65427e016b3c5ae0d2517a69db5f1cdc7a43d2c0a7957e8da5d6f378f063` | lift |
| `vorbis.dll`      | pl | 1060864 | `fefda850b69e007fceba644483c7616bc07e9f177fc634fb74e114f0d15b0db0` | lift (base `0x10100000`) |
| `vorbisfile.dll`  | pl | 65536 | `a08923479000cec366967fb8259e0920b7aa18859722c7dda1415726bed4774f` | lift (base `0x10300000`) |

Skipped by name (not pinned): the installers' own programs and the InstallShield engine files the Russian Sacked
setup installs beside the game (`ctor.dll`, `iscript.dll`, `iuser.dll`, `isrt.dll`, `objectps.dll`,
`_IsRes.dll`, `iKernel.exe`), the Polish Crazy School's `vorbisenc.dll` (not imported), `DirectX/DX81*.exe`,
`Autostart.exe`/`Autorun.exe` and the copy-protected CD launchers, wallpapers, manuals.

## Data files (not pinned, read by the game)

- Sacked: `CO_BACKGROUND.OGD`, `CO_CHARS.OGD`, `CO_EFFECT.OGD`, `CO_GUI.OGD`, `CO_OBJECTS.OGD`,
  `co_objects.dat`, `International.ttf`, `Levels/LEVEL_nn.col`, `LEVEL_nns.col`, `Level_nn.txt`,
  `Uberschriften.txt`, `Sound/sounds.txt`, `Sound/FX/S*.wav`, `Sound/Musik/*.ogg`.
- Crazy School: `CS_*.OGD`, `CS_*.PAL`, `*.O2F` (fonts), `CS_OBJECTS.DAT`, `HIGHSCORE.DAT`, `NAMES.DAT`,
  `Levels/Level_nn.col`, `Level_nns.col`, `Level_nn.txt` (00–20), `Sound/sounds.txt`, `Sound/FX/*.wav`,
  `Sound/Musik/*.ogg`.
