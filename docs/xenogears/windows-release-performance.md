# Windows release optimization

The Windows release job uses llvm-mingw with runtime LTO and a two-pass PGO
build. It compiles `PSX_PGO=generate`, runs that Windows executable through
GE-Proton/UMU at 30 and 240 Native FPS, merges its Clang profiles with the **same
toolchain's** `llvm-profdata`, then rebuilds with `PSX_PGO=use` before packaging.
The final executable contains the optimized code, not profiling counters.
The final PGO-use executable also runs a 120-vblank smoke test at each FPS
setting before packaging, using the same slot-load and renderer checks.
The check also rejects a final executable that still contains the profiling
runtime. See [runner setup](release-runner.md) for the Linux pipeline
and common dependencies.

## Private runner inputs

Install `umu-run` and GE-Proton and provide an accessible X11/XWayland desktop.
Training renders a real window; headless mode skips the presentation paths we
want to optimize. If the runner service lacks `DISPLAY`, the script imports
only `DISPLAY`, `XAUTHORITY` and `XDG_RUNTIME_DIR` from its user's active
systemd session. Otherwise configure those variables explicitly. A logged-in
graphical session with working OpenGL is required; Xvfb alone did not expose
usable displays through this GE-Proton setup and is not an automatic fallback.
The Actions variable `XGR_PGO_PROTON_ROOT` selects the installed GE-Proton
directory; the default is the maintainer's GE-Proton10-34 installation. UMU's
installed Steam runtime is reused without checking for updates during training.

Place an OpenBIOS Disc 1 slot 0 savestate from an interpolation-heavy field
scene at `$XGR_ASSETS_DIR/pgo/slot00.pst`. The Actions variable
`XGR_PGO_SAVESTATE` can override that path. This file stays outside the runner
checkout and is never uploaded. The game EXE, disc and BIOS remain the existing
private build inputs. Training creates isolated memory cards and settings,
without changing the player's saves. The private state and profiles in
`build-win/pgo` are scrubbed after the job.

Each pass runs 1,800 vblanks with neutral input from the supplied state. The job
requires a confirmed slot load, completed replay, healthy Native/OpenGL work,
interpolated presentation at 240 FPS and fresh profile files from both runs.
Missing inputs or failed training stop the release; it cannot silently publish
an instrumented build or reuse an old profile. This scenario emphasizes field
rendering; additional gameplay scenarios can improve future profile coverage.

For an already instrumented local build:

```sh
python3 ci/train_windows_pgo.py --build-dir build-win \
  --llvm-root "$LLVM_MINGW_ROOT" --savestate /private/pgo/slot00.pst \
  --proton "$XGR_PGO_PROTON_ROOT" \
  --disc /private/disc1.cue
cmake -S . -B build-win -DPSX_RUNTIME_IPO=ON -DPSX_PGO=use
cmake --build build-win --target psx-runtime --parallel 4
```

PGO does not bake in the training machine's GPU or require its CPU model. It
guides compiler decisions using observed execution counts. Wine training is
useful profile data, but does not validate performance on native Windows.

## Workers and GPU support

The Native phase pool uses the OS CPU affinity mask on Linux and Windows,
with at most four workers. A process restricted to one logical CPU creates one
phase worker. The count is sampled when the pool starts; restart the game after
changing affinity to resize an existing pool. On Windows, an unavailable or
zero mask (including some multi-group configurations) falls back to SDL's CPU
count.

The fast Native GL path checks driver capabilities, not GPU brands. Its ordered
fragment path needs `GL_ARB_fragment_shader_interlock`; compute triangle setup
also needs compute shaders, shader storage buffers, FP64 and Int64 shader
extensions. The current image fingerprint compute path is initialized together
with that setup. Shader compilation must succeed as well. Integrated and
discrete GPUs are both eligible when their OpenGL driver supplies these
features. A recent GPU or OpenGL 4.6 alone does not guarantee every optional
extension. Unsupported optional paths retain the compatible renderer/CPU
setup, which may have a lower performance ceiling. Logs confirm activation:

```
Native GPU: compute triangle setup ready
Native GPU: image fingerprint compute ready
```
