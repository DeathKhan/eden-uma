#!/bin/bash
# build.sh <mesa checkout> <driver name>: Turnip (KGSL, Android arm64) packaged for adrenotools.
# Needs NDK=<android ndk root> (toolchains/llvm/prebuilt/linux-x86_64), meson, ninja, python mako.
set -e
WT=$1; NAME=$2
BIN=${NDK:?set NDK to the Android NDK root}/toolchains/llvm/prebuilt/linux-x86_64/bin
cd "$WT"
cat > android-aarch64.cross <<CROSS
[binaries]
ar = '$BIN/llvm-ar'
c = ['$BIN/aarch64-linux-android35-clang']
cpp = ['$BIN/aarch64-linux-android35-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-static-libstdc++', '-Wno-c++11-narrowing']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$BIN/llvm-strip'
pkg-config = ['/bin/false']
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
CROSS
[ -d build-android ] || meson setup build-android --cross-file android-aarch64.cross \
  -Dbuildtype=release -Dplatforms=android -Dplatform-sdk-version=35 -Dandroid-stub=true \
  -Dandroid-libbacktrace=disabled -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
  -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=disabled -Dzstd=disabled -Dvalgrind=disabled
ninja -C build-android src/freedreno/vulkan/libvulkan_freedreno.so
OUT=build-android/pkg; rm -rf $OUT; mkdir -p $OUT
cp build-android/src/freedreno/vulkan/libvulkan_freedreno.so $OUT/
$BIN/llvm-strip --strip-unneeded $OUT/libvulkan_freedreno.so
REV=$(git rev-parse --short HEAD)
cat > $OUT/meta.json <<META
{"schemaVersion":1,"name":"$NAME","description":"Mesa $(cat VERSION) $REV (Eden UMA)","author":"Eden UMA","packageVersion":"1","vendor":"Mesa","driverVersion":"$(cat VERSION)-$REV","minApi":28,"libraryName":"libvulkan_freedreno.so"}
META
(cd $OUT && zip -q -9 ../$NAME.zip libvulkan_freedreno.so meta.json)
echo "built build-android/$NAME.zip"
