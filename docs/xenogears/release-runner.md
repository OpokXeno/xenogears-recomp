# Release runner

Both release jobs use the same `[self-hosted, linux]` runner. Linux is built
with GCC inside the glibc 2.31 Podman image; Windows is cross-compiled with
llvm-mingw and exercised through GE-Proton/UMU. This does not replace testing
on native Windows hardware.

## Host setup

Provide CMake, Git, Python 3.11+, Podman, Ninja, curl, tar and binutils (`nm`).
Windows training also needs `umu-run`, an installed GE-Proton and its cached
Steam runtime. The workflow downloads llvm-mingw and SDL2-mingw into
`XGR_DEPS_DIR` if they are absent. SDL3 is built from the project's dependency.

The runner must use an accessible graphical X11/XWayland session with working
OpenGL. When `DISPLAY` is missing, the trainers import only `DISPLAY`,
`XAUTHORITY` and `XDG_RUNTIME_DIR` from that user's systemd session. Otherwise
set those variables in the runner service. Rendering is required for training;
headless execution would skip the presentation code being optimized.
For a system service without `XDG_RUNTIME_DIR`, the helper first locates the
current user's owned `/run/user/<uid>/bus`, then queries that session.

Keep the legally owned inputs outside the checkout in `XGR_ASSETS_DIR`:

```text
slus_006.64
SCPH1001.BIN
boxart.tga
disc1.cue + disc1.bin (or disc1.bin / disc1.iso)
pgo/slot00.pst
```

The slot must be an OpenBIOS Disc 1 slot 0 saved in a field scene that exercises
Native interpolation. Use the Actions variables `XGR_DISC_DIR` and
`XGR_PGO_SAVESTATE` for separate disc/state locations. Set
`XGR_PGO_PROTON_ROOT` to select GE-Proton. These inputs, generated saves and
profiles stay on the runner; only the verified release archives are uploaded.

## Optimized builds

Both jobs explicitly build Release with runtime LTO and `PSX_PGO=generate`,
train for 1,800 vblanks at each of 240 and 30 Native FPS, then rebuild with
`PSX_PGO=use`. Profiles are regenerated for every release and target; neither
job relies on a previous CMake cache or profiles from another platform.

Linux uses `build-linux-release` to avoid sharing a host build's compiler cache.
Both Linux compilation passes use `/workspace` in the same glibc 2.31 image.
The instrumented executable runs on the host GPU, with `GCOV_PREFIX` and
`GCOV_PREFIX_STRIP` redirecting its counters back into the mounted build tree
without changing GCC's object names. Windows merges its counters using the
matching llvm-mingw `llvm-profdata`.
GCC training uses atomic profiling counters: the Native phase workers share
hot functions, and lost updates otherwise produce profiles rejected by GCC.

Before packaging, each optimized executable must load the slot, complete
120 vblanks at both FPS settings, report healthy Native/OpenGL work and
exercise interpolation at 240 FPS. The checks also reject executables that
still contain the profiling runtime. Linux retains the GLIBC_2.31 ABI ceiling;
Windows retains the system-DLL import check. Missing profiles or failed
training stop the build. `always()` cleanup scrubs staged inputs and private
PGO state, including on failure.

`XGR_BUILD_JOBS` defaults to 4 and bounds compilation, BIOS generation and LTO
link workers. Reduce it on a busy host. To run the Linux pipeline locally:

```sh
XGR_BUILD_JOBS=2 bash ci/build-linux-release.sh /private/xenogears-assets
```

The script restores any pre-existing staged private inputs on exit. Local
profiles remain in the selected build directory for inspection. The workflow
runs on tags matching `v*` or manual dispatch, and publishes a release after
both platform jobs succeed.
