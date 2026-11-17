# Prinnit's OCCT shim

Prinnit, a 3D design app for iPad and iPhone by Low Hum Labs, imports STEP files with
[Open CASCADE Technology](https://github.com/Open-Cascade-SAS/OCCT) (OCCT) 8.0.1, which is licensed
under the GNU Lesser General Public License 2.1 with the Open CASCADE exception.

This repository holds the source of the part of Prinnit that links OCCT, so that you can rebuild it
with a changed OCCT and use that in the app, as the LGPL provides. It is published under the same
licence, the LGPL 2.1 (see `LICENSE`).

## What is here

| Path | What it is |
|---|---|
| `StepShim/include/step_shim.h` | The C interface the app calls (the `ps_*` functions). No OCCT type crosses it. |
| `StepShim/OCCT/step_shim.cpp` | The shim: reads a STEP file with OCCT's STEP reader and meshes it with OCCT's mesher. |
| `scripts/build-occt.sh` | Builds the OCCT toolkits the shim uses, as static libraries, for iOS, the iOS simulator and macOS. |
| `scripts/package-occt.sh` | Links the shim with that OCCT into `PrinnitOCCT.framework`, one per platform, as an xcframework. |

## How Prinnit uses OCCT

OCCT is used unmodified. The shim and OCCT are linked into one dynamic framework,
`PrinnitOCCT.framework`, which sits inside the app (`Prinnit.app/Frameworks/`) and exports only the
`ps_*` functions declared in `step_shim.h`. The rest of the app never links OCCT; it calls those
functions through the framework.

Only the STEP reader (`TKDESTEP`) and the mesher (`TKMesh`) are built, with the toolkits they depend
on. No third-party libraries are linked into OCCT.

## Building the framework

You need a Mac with Xcode, plus CMake and Ninja (`brew install cmake ninja`). Each platform takes
about 25 minutes on an M1.

```bash
# OCCT 8.0.1, or your changed copy of it.
git clone --depth 1 --branch V8_0_1 https://github.com/Open-Cascade-SAS/OCCT.git occt-src

# Static OCCT for each platform (ios, ios-sim, macos), in occt-work/.
scripts/build-occt.sh occt-src occt-work

# The shim + OCCT → out/PrinnitOCCT.xcframework.
scripts/package-occt.sh occt-work
```

## Using your build in Prinnit

1. Take `PrinnitOCCT.framework` from the `ios-arm64` folder of `out/PrinnitOCCT.xcframework`.
2. Replace `Prinnit.app/Frameworks/PrinnitOCCT.framework` with it, in a copy of the app (for
   example one unpacked from an `.ipa`).
3. Re-sign the framework and the app with your own signing identity (for example with `codesign`,
   or a re-signing tool), then install it on your device.

Your framework must export the same `ps_*` functions with the same signatures as `step_shim.h`;
the version of Prinnit you are replacing it in names the OCCT version it was built with in
Help › About › Acknowledgements.

## Contact

support@lowhumlabs.com
