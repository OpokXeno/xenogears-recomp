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

Original-resolution guest VRAM is rasterized on the CPU on all platforms and
at every FPS setting. Native VIEW geometry and temporal phases remain GPU work.
Guest VRAM also supplies CPU texture, palette and copy inputs; keeping it on the
CPU avoids repeated synchronous GPU readbacks before phase generation. The WGL
round trips can otherwise consume the interpolation window in feedback-heavy
scenes, despite a correct FPS selection. `PSX_NATIVE_GPU_CANONICAL=1` enables
the GPU canonical raster explicitly for diagnostic comparisons.

Static phase triangle parameters use CPU preparation for batches of at most
128 triangles; larger batches retain compute setup. The decision uses captured
triangle counts, including every face in a mesh command, before geometry is
prepared. This changes where the existing attribute/depth calculations run,
without changing their precision or the GPU rasterizer. `PSX_NATIVE_GPU_SETUP=0`
forces CPU preparation and `=1` forces available GPU setup for A/B diagnostics.
The hidden-GL `native_gpu_scale_test --setup-benchmark` measures geometry,
material packing, upload and compute completion together, and checks exact
CPU/GPU parameter-buffer equality. On this machine's single E-core under Linux,
64 triangles took about 19 us on CPU versus 35 us with compute setup; 128 took
37 versus 45 us, while 256 was already near the crossover (63 versus 60 us).
The conservative 128-face cutoff avoids moving large world meshes onto a
single CPU core and leaves margin for variation in GPU submission costs.

Native phase compilation, CPU reference bands, CPU recipe stripes and HD texture
decoding size their workers from the OS CPU affinity mask on Linux and Windows.
There is no fixed three/four-worker ceiling. Each subsystem uses at most one
worker per available logical CPU and no more workers than its independent work
items (phases, raster rows or pending/in-flight images). Recipe stripes include
the calling thread in that count. Phase and texture pools grow on demand and
retain idle workers for reuse; reference bands are created on their first draw.
Restart the game after changing affinity to resize retained pools. Failed thread
creation keeps a smaller working pool or falls back to serial rendering.

Performance checks must also launch with affinity restricted to one E-core,
verify the game threads retain that mask, and compare both Original and 240 FPS
in a field scene and the worldmap. Repeat the check without timing diagnostics.
Check guest vblank progress and interpolated phase selections together: a
240-Hz presentation counter alone can include repeated images while simulation
falls below 60 vblanks/s. Use the same affinity, state and settings for the
before/after comparison.

On Windows, an unavailable or zero mask (including some multi-group
configurations) falls back to SDL's CPU count for rendering and the C++ hardware
CPU count for texture decoding. Game simulation remains ordered on its guest
thread; additional workers do not change guest execution order or GL ownership.

Geometry preparation also splits a single mesh into primitive ranges. Endpoint
transforms, per-phase vertex projection, entity evaluation/copies, draw projection
and immutable texture copies use a shared pool, so their CPU parallelism is not
limited by the number of interpolation phases. Draw rejection is merged in
submission order before phase publication. Nested jobs help execute pending work
instead of waiting for another pool; idle threads are retained for reuse.

MDEC parses the compressed stream on the guest owner, then runs independent
IDCT/color-conversion blocks and native-frame tiles on that same shared pool.
The owner waits before publishing FIFO bytes, frame metadata or command
completion. CPU count and task scheduling do not change serialized MDEC state.
For diagnostic A/B runs, `PSX_HOST_PARALLEL=0` executes these shared-pool ranges
on the caller; the default uses all available CPUs. Restart after changing it.

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

## Native GPU measurement and image checks

Normal play does not allocate Native GPU timestamp queries. Set
`PSX_NATIVE_GPU_TIMING=1`, or request `PSX_NATIVE_TIMING_OUT`, to collect them;
`PSX_NATIVE_GPU_TIMING=0` explicitly disables them. These spans include queue
idle time between source slices and measure latency rather than GPU utilization.
For a controlled comparison in the same executable,
`PSX_NATIVE_GENERIC_RASTER=1` restores the generic shader arithmetic and eager
attribute loads, and `PSX_NATIVE_FLUSH_SLICES=1` restores per-slice submission.
Keep the replay, FPS, resolution and quality fixed, and report GPU clocks and
power alongside utilization because the driver changes its operating frequency.

The Native GPU scale test compares full RGBA and integer depth images between
both shaders at 1x through 8x, with fractional coordinates, perspective, Gouraud
shading, raw textures, masks and blending. `--raster-benchmark` additionally
measures repeated GPU draws. Its UI check confirms that clipping the overlay to
menu/popup bounds preserves the game pixels outside those bounds. Custom ImGui
draw callbacks retain full-window composition.
