#!/bin/zsh
# Links the OCCT that scripts/build-occt.sh built with Prinnit's STEP shim into
# PrinnitOCCT.framework, one per platform, as out/PrinnitOCCT.xcframework.
#
# OCCT and the shim ship as one dynamic framework that exports only the shim's C ABI (the ps_*
# functions), so the linker drops every part of OCCT the shim doesn't reach. The shim's source is
# published under the LGPL-2.1, so anyone can rebuild the framework with a changed OCCT.
#
#   scripts/package-occt.sh <work-dir>          (the work directory build-occt.sh used)
#
# PRINNIT_SHIM_DIR (the folder holding include/ and OCCT/) and PRINNIT_OCCT_OUT (where the
# xcframework goes) override the defaults below.
set -euo pipefail
work=${1:?work directory}
work=$(cd "$work" && pwd)
here=${0:A:h}
shim=${PRINNIT_SHIM_DIR:-$here/../StepShim}
vendor=${PRINNIT_OCCT_OUT:-$here/../out}
name=PrinnitOCCT
version=8.0.1
mkdir -p "$vendor"
vendor=$(cd "$vendor" && pwd)
staging=$(mktemp -d)
print -l "_ps_*" > "$staging/exports.txt"

plist() { # <path> <platform> <minimum-os-key> <minimum-os>
  cat > "$1" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>$name</string>
  <key>CFBundleIdentifier</key><string>app.prinnit.$name</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>$name</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleSupportedPlatforms</key><array><string>$2</string></array>
  <key>$3</key><string>$4</string>
</dict>
</plist>
EOF
}

frameworks=()
for slice in ios ios-sim macos; do
  library=$work/lib-$slice/libOCCT.a
  headers=$work/install-$slice/include/opencascade
  [[ -f $library ]] || { echo "missing slice $slice: run build-occt.sh first" >&2; exit 1; }
  case $slice in
    ios)     sdk=iphoneos;        target=arm64-apple-ios18.0;           platform=iPhoneOS;        minKey=MinimumOSVersion;      minimum=18.0 ;;
    ios-sim) sdk=iphonesimulator; target=arm64-apple-ios18.0-simulator; platform=iPhoneSimulator; minKey=MinimumOSVersion;      minimum=18.0 ;;
    macos)   sdk=macosx;          target=arm64-apple-macos26.0;         platform=MacOSX;          minKey=LSMinimumSystemVersion; minimum=26.0 ;;
  esac
  framework=$staging/$slice/$name.framework
  if [[ $slice == macos ]]; then
    # macOS frameworks are versioned bundles.
    contents=$framework/Versions/A
    mkdir -p "$contents/Resources"
    binary=$contents/$name
    installName=@rpath/$name.framework/Versions/A/$name
    plist "$contents/Resources/Info.plist" $platform $minKey $minimum
    ln -s A "$framework/Versions/Current"
    ln -s Versions/Current/$name "$framework/$name"
    ln -s Versions/Current/Resources "$framework/Resources"
  else
    mkdir -p "$framework"
    binary=$framework/$name
    installName=@rpath/$name.framework/$name
    plist "$framework/Info.plist" $platform $minKey $minimum
  fi
  xcrun --sdk $sdk clang++ -target $target -std=c++17 -Os \
    -I"$shim/include" -I"$headers" -Wno-deprecated-declarations \
    -c "$shim/OCCT/step_shim.cpp" -o "$staging/$slice-shim.o"
  xcrun --sdk $sdk clang++ -target $target -dynamiclib -install_name $installName \
    -compatibility_version 1 -current_version 1 \
    "$staging/$slice-shim.o" "$library" \
    -Wl,-dead_strip -Wl,-exported_symbols_list,"$staging/exports.txt" \
    -framework Foundation -o "$binary"
  xcrun strip -x "$binary"
  # Stripping drops the linker's ad-hoc signature; Apple silicon needs one to load it (Xcode signs
  # the app's copy with the app's identity).
  codesign --force --sign - "$binary" >/dev/null
  frameworks+=(-framework "$framework")
  echo "$slice: $(ls -l "$binary" | awk '{print $5}') bytes"
done

rm -rf "$vendor/$name.xcframework"
xcodebuild -create-xcframework $frameworks -output "$vendor/$name.xcframework"
rm -rf "$staging"
echo "$name.xcframework in $vendor ($(du -sh "$vendor/$name.xcframework" | cut -f1))"
