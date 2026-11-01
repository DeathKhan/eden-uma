# Turnip for Eden UMA

Patches on top of [whitebelyash/mesa-unified](https://github.com/whitebelyash/mesa-unified) branch
`turnip/gen8`, commit `9c7e022677d` (Adreno 8xx support), applied in order:

1. `tu: skip rebuilding the VFD draw state when vertex buffers are unchanged`
2. `tu: skip identical push constant updates while shader constants are clean`
3. `tu/kgsl: implement VK_EXT_external_memory_host`: imports host pointers with
   `IOCTL_KGSL_GPUOBJ_IMPORT` (`KGSL_USER_MEM_TYPE_ADDR`), IO-coherent and write-back cached.
   Eden UMA uses it to let the GPU use the emulated RAM directly.
4. `tu: do not count host-pointer imports as heap usage`

Build for Android (adrenotools zip):

```sh
git clone https://github.com/whitebelyash/mesa-unified && cd mesa-unified
git checkout 9c7e022677d
git am /path/to/eden-uma/turnip-uma/patches/*.patch
NDK=/path/to/android-ndk ./../eden-uma/turnip-uma/build.sh "$PWD" turnip-uma
```

`build.sh` needs the Android NDK, meson, ninja and Python with mako. The result is
`build-android/turnip-uma.zip`, installable from Eden's GPU Driver Manager.
