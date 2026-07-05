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

## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
**switch-dev** group plus these portlibs:

```
sudo dkp-pacman -S switch-dev \
    switch-mesa switch-libdrm_nouveau switch-sdl2

## Legal

You must own Burger Shop. This repository distributes only original wrapper code
(MIT-licensed, see `LICENSE`); it bundles no GoBit/PopCap code or assets. The
game's libraries and `pakfiletable.wmv` are loaded from files you extract from
your own copy. Burger Shop, SexyAppFramework and BASS are the property of their
respective owners. BASS (un4seen) is free for non-commercial use; respect its
license for any redistribution.

## Credits

- ELF loader, bionic shim layer, OpenSL ES shim and JNI scaffolding adapted from
  the FF4: The After Years Switch port (fgsfds, Andy Nguyen) and the broader
  max_nx / TheOfficialFloW lineage.
```
