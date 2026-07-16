# Burger Shop — Nintendo Switch port

A native Switch wrapper for **Burger Shop** (GoBit Games), the Android build of
which runs on PopCap's **SexyAppFramework** (C++) with **BASS** for audio. This
project loads the game's two original ARM64 shared libraries directly on Switch
and recreates the thin Android/JNI lifecycle around them, the same technique
used by the FF4: The After Years and TheOfficialFloW Vita ports it derives from.

It contains **no game code or assets**. You supply those from a copy of the game
you legally own (see below). Nothing copyrighted is distributed here.

## What you need to own and extract

You must own Burger Shop on Android. From your copy (the APK and/or the installed
app), extract these three files:

| Files needed           | Where it lives in the APK         | Put it on the SD card as |
|------------------------------|-----------------------------------|--------------------------|
| `libSexyAndroid.so` (slit_config.arm64_v8a)  | `lib/arm64-v8a/libSexyAndroid.so` | `libSexyAndroid.so`      |
| `libbass.so` (split_config.arm64_v8a)         | `lib/arm64-v8a/libbass.so`        | `libbass.so`             |
| `pakfiletable.wmv` (base.apk) | `assets/pakfiletable.wmv`         | `pakfiletable.wmv`  

## SD card layout

```
sdmc:/switch/burgershop/
├── burgershop.nro        (this port, after building)
├── libSexyAndroid.so     (you provide)
├── libbass.so            (you provide)
└── pakfiletable.wmv      (you provide)
```

Launch `burgershop.nro` through hbmenu using title override. The working directory is the folder the
`.nro` lives in, which is why the data files sit beside it.

Optionally, drop a `cursor.png` (64x64, transparency supported) in the same folder to replace the on-screen cursor with your own.

## Controls
 
| Input | Action |
| --- | --- |
| `+` | Toggle the on-screen cursor |
| `-` | Toggle gyro pointing (tilt/turn the controller to aim) |
| Left stick | Move the cursor |
| `L` / `R` | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| `A` / `ZR` / `ZL` | Tap / confirm (ZL and ZR let you play one-handed) |
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |

A USB mouse works in both handheld and docked: move to control the cursor, left-click to tap, and use the scroll wheel to change 
sensitivity.
Your stick, mouse and gyro sensitivities are remembered in `pointer.cfg` automatically after in-game adjustment.

## Languages
On first launch the wrapper writes `sdmc:/switch/angrybirds/config.txt`
(one `name value` per line, `#` for comments):
 
```
# language: 'auto' follows the Switch system language, or one of the codes in
# the table below (e.g. fr, de, es, es_419, pt, ru, ja, zh, zh_TW). 
```

| `language=` | Game locale | UI text |
|---|---|---|
| `auto` | follows the Switch system language | — |
| `en` | en_EN | English (no patch — stock) |
| `fr` | fr_FR | French |
| `it` | it_IT | Italian |
| `de` | de_DE | German |
| `es` | es_ES | Spanish (Spain) |
| `es_419` | es_419 | Latin-American Spanish |
| `pt` | pt_BR | Brazilian Portuguese |
| `ru` | ru_RU | Russian |
| `ja` | ja_JP | Japanese |
| `zh` | zh_CN | Simplified Chinese |
| `zh_TW` | zh_TW | Traditional Chinese |

## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
**switch-dev** group plus these portlibs:

```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-freetype switch-libpng switch-zlib switch-bzip2
```

## Legal

You must own Burger Shop. This repository distributes only original wrapper code
(MIT-licensed, see `LICENSE`); it bundles no GoBit/PopCap code or assets. The
game's libraries and `pakfiletable.wmv` are loaded from files you extract from
your own copy. Burger Shop, SexyAppFramework and BASS are the property of their
respective owners. BASS (un4seen) is free for non-commercial use; respect its
license for any redistribution.


## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `util`, `error`) derives
from the open-source Switch port of *Burger Shop* by Andy Nguyen, fgsfds and
ChanseyIsTheBest, which in turn builds on TheOfficialFloW's Vita/Switch loader
lineage — all MIT-licensed. The Fusion-specific JNI, platform callbacks, audio,
imports and main loop in this project are new. Thanks to everyone in that
lineage for making this approach possible.
 
The on-boot language patcher vendors two public-domain libraries, unmodified
except for build configuration: the **LZMA SDK** by Igor Pavlov
(`LzmaDec.c`, `LzmaEnc.c`, `LzFind.c`, `Alloc.c` and headers) for the game's
script (de)compression, and **tiny-AES-c** by kokke (`aes.c`/`aes.h`, set to
AES-256) for the AES-CBC layer. Both are public domain; the sources live in
`source/` alongside the wrapper. `lzma_alone.c`, `patch_bytecode.c` and
`locale_patch.c` (the LZMA-alone framing, Lua-bytecode edit, and boot logic) are
new to this project.
 
