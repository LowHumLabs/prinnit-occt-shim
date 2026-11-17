#!/bin/zsh
# Builds the part of Open CASCADE Technology (OCCT, LGPL-2.1 with the OCCT exception) that Prinnit's
# STEP import uses: the STEP reader (TKDESTEP) and the mesher (TKMesh), with what they depend on.
#
#   scripts/build-occt.sh <occt-source> <work-dir> [slice ...]
#
# <occt-source> is a checkout of https://github.com/Open-Cascade-SAS/OCCT at tag V8_0_1.
# Slices: ios (device arm64), ios-sim (simulator arm64), macos (arm64, for `swift test`).
# Each slice is configured and built in <work-dir>/build-<slice>, installed into
# <work-dir>/install-<slice>, and its toolkits are merged into one static library
# <work-dir>/lib-<slice>/libOCCT.a.
set -euo pipefail

src=${1:?OCCT source directory}
work=${2:?work directory}
shift 2
slices=(${@:-ios ios-sim macos})

mkdir -p "$work"
src=$(cd "$src" && pwd)
work=$(cd "$work" && pwd)

for slice in $slices; do
  build="$work/build-$slice"
  install="$work/install-$slice"
  case $slice in
    ios)     platform=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0) ;;
    ios-sim) platform=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0) ;;
    macos)   platform=(-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0) ;;
    *) echo "unknown slice $slice" >&2; exit 1 ;;
  esac
  # Every module off; only the STEP reader and the mesher, plus their dependencies (OCCT's CMake adds
  # those itself). Static, sized for an app (-Os). OCCT's own checks stay on in release builds
  # (BUILD_RELEASE_DISABLE_EXCEPTIONS=OFF), so a malformed file raises an exception the shim catches
  # instead of reading out of bounds.
  cmake -S "$src" -B "$build" -G Ninja \
    $platform \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG" \
    -DCMAKE_C_FLAGS_RELEASE="-Os -DNDEBUG" \
    -DCMAKE_INSTALL_PREFIX="$install" \
    -DBUILD_LIBRARY_TYPE=Static \
    -DBUILD_RELEASE_DISABLE_EXCEPTIONS=OFF \
    -DBUILD_MODULE_FoundationClasses=OFF \
    -DBUILD_MODULE_ModelingData=OFF \
    -DBUILD_MODULE_ModelingAlgorithms=OFF \
    -DBUILD_MODULE_Visualization=OFF \
    -DBUILD_MODULE_ApplicationFramework=OFF \
    -DBUILD_MODULE_DataExchange=OFF \
    -DBUILD_MODULE_Draw=OFF \
    -DBUILD_ADDITIONAL_TOOLKITS="TKDESTEP TKMesh" \
    -DBUILD_DOC_Overview=OFF \
    -DBUILD_GTEST=OFF \
    -DUSE_FREETYPE=OFF -DUSE_TK=OFF -DUSE_OPENGL=OFF -DUSE_GLES2=OFF -DUSE_XLIB=OFF \
    -DUSE_FREEIMAGE=OFF -DUSE_RAPIDJSON=OFF -DUSE_DRACO=OFF -DUSE_TBB=OFF -DUSE_VTK=OFF \
    -DINSTALL_SAMPLES=OFF -DINSTALL_TEST_CASES=OFF
  cmake --build "$build" --parallel
  cmake --install "$build"
  mkdir -p "$work/lib-$slice"
  libtool -static -o "$work/lib-$slice/libOCCT.a" $(find "$install" -name "libTK*.a" | sort)
  echo "$slice: $(du -h "$work/lib-$slice/libOCCT.a" | cut -f1)"
done
