# Eden UMA

A performance fork of [Eden](https://git.eden-emu.dev/eden-emu/eden) for Android handhelds with
Adreno 8xx GPUs (developed and tested on an AYN Odin 3, Snapdragon 8 Elite / Adreno 830).

Phones and handhelds share one pool of RAM between the CPU and GPU, but emulators are written for
PCs with separate video memory: they copy guest memory into GPU buffers, write-protect it to notice
changes, and copy results back. On a unified-memory (UMA) device that work is pure overhead. Eden UMA
removes it: with the bundled Turnip driver, the GPU reads and writes the emulated Switch's RAM in place.

It installs next to the official Eden (app ID `dev.eden.eden_emulator.uma`, name "Eden UMA") and keeps
its own settings and saves.

## Results

Luigi's Mansion 3, hotel lobby walk, AYN Odin 3, handheld, 1x resolution, 30 fps cap. CPU cost per
frame is measured with `simpleperf` cycle counters over the whole emulator process.

| | Official Eden v0.2.1 + Turnip Gen8 V38 | Eden UMA + bundled Turnip |
|---|---|---|
| Average fps | 24.1 | 29.9 (holds the game's cap) |
| Frames slower than 30 fps | 1,227 of 2,503 | 67 of 3,113 |
| CPU cycles per frame | 261 M | 62 M |
| CPU cores busy | 2.0 | 0.86 |
| Peak SoC temperature | 102 °C | 71 °C |

One run each with each app's own settings (official Eden had vsync off); other games and devices will
differ. Step-by-step A/B measurements are in the commit messages.

## What changed

- **Host aliasing of guest RAM** (needs the bundled Turnip): buffer-cache buffers are imported straight
  from the emulated RAM with `VK_EXT_external_memory_host` (implemented for KGSL in the bundled Turnip).
  No uploads, downloads, clears or join copies for them, and no write protection, so guest CPU writes
  stop faulting. Buffers over physically scattered or partly unmapped memory go through a private
  view of the same memory. Guest fences wait for the real GPU while aliases exist; GPU-engine writes
  that hit memory the GPU is still reading wait once and fall back to an ordinary buffer.
- **UMA paths without host aliasing** (any Turnip): direct writes into mapped cache buffers, in-place
  readback, `VK_EXT_host_image_copy` for fresh compressed textures, in-place async flushes,
  CPU-side quad index conversion, memset/memcpy instead of GPU clears and copies on fresh buffers.
- **CPU-side fixes**: the NCE write-fault window, an interrupt-aware idle for guests that poll with
  `svcSleepThread` yields, fewer scheduler-worker wake-ups, a cached memory-budget query, cheaper
  texture-binding bookkeeping, and OpenSSL built from source so NCA decryption uses the ARMv8 AES
  instructions.

## Installing

1. Install `eden-uma-<version>.apk` from the release.
2. Copy your keys and firmware as for official Eden.
3. In Eden UMA open Settings → GPU Driver Manager and install `turnip-uma-<version>.zip` from the release.
   Host aliasing only turns on with this driver; with other drivers the other UMA paths still apply.

## Settings

All on by default, in `config.ini` (`[Renderer]`) only for now:
`uma_zero_copy`, `uma_direct_write`, `uma_direct_readback`, `uma_host_image_copy`, `uma_lean`,
`uma_host_alias`. Host aliasing also needs asynchronous GPU emulation (the default).

## Building

Same as Eden's Android build (`src/android`, `./gradlew assembleMainlineRelease`). The driver is built
from [whitebelyash/mesa-unified](https://github.com/whitebelyash/mesa-unified) `turnip/gen8` plus the
patches in [`turnip-uma/`](turnip-uma/); see its README.

## Credits and license

Eden and yuzu developers; whitebelyash for Turnip gen8 (Adreno 8xx) support; the Mesa/Turnip
developers. Eden UMA is GPL-3.0-or-later like Eden; the Turnip patches follow Mesa's MIT license.
This is an unofficial fork: please report its bugs here, not to the Eden team.

If this helps you, you can support the work on [Ko-fi](https://ko-fi.com/jamesfo).
