# DIFastBoot

Faster boot and loading for **Dead Island** (2011, Steam, 32-bit). One ASI, no game files replaced.

Measured on one PC: the main menu came up in about 10.8 s instead of 47 s, and a save loaded in 7.7 s instead of 13 s.

## What it does

- **Boot lock fix:** the loading-screen thread no longer sleeps 200 ms while holding the renderer lock, which kept the
  loader waiting for most of the boot.
- **Shader cache:** compiled shaders are kept on disk (`DIFastBoot.shadercache`) instead of recompiled every boot.
- **Texture cache:** generated texture mip levels are kept on disk (`DIFastBoot.texcache`).
- **Skip intro:** straight to the main menu (no logos, disclaimer or "press any key").
- **Skip Facebook:** the dead Facebook login setup is skipped (about 5 s every boot).
- **Quieter, faster crash.log:** WARN lines are left out and lines go through the OS cache. A crash still loses
  nothing.
- **Loading screen:** frame cap setting (default 60) and no VSync wait, so its frames stop blocking the loader.

Every feature has its own switch in `DIFastBoot.ini`.

## Install

1. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) in the game folder as
   `winmm.dll` (as `dinput8.dll` it can fail to launch under the Steam overlay).
2. Put `DIFastBoot.asi` and `DIFastBoot.ini` in `Dead Island\scripts\`.

The first boot builds the caches, so it's only a little faster. Later boots use them. Delete the two cache files to
rebuild them.

## Build

Visual Studio (x86 C compiler). Edit the `vcvars32.bat` path in `build.bat`, then run it. Output:
`DIFastBoot.asi`. It must be 32-bit.

`NOTES.md` has the patched sites and the measurements.

## License

MIT
