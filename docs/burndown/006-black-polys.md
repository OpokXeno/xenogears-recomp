# Burn-down #6 — intermittent black / untextured terrain & building polys

## Symptom (user report, 2026-07-18)

While playing (launcher flow), in SOME frames polygons in the terrain — and
sometimes the 3D buildings — turn black or lose their texture. Intermittent,
per-frame. Sprites (2D characters) not reported affected.

## Play configuration (from build/settings.toml — matters!)

- renderer = **opengl** (NOT software), supersampling = 2, antialiasing on
- aspect_ratio = 16:9 (native-wide), frame_interpolation = true
- texture_filtering = nearest, crt_filter = raw

## Hypotheses (ranked)

| # | Hypothesis | Discriminator |
|---|---|---|
| H1 | Guest-side shading: packets carry black vertex colors on bad frames (GTE lighting path; prime suspect = compiled-vs-interpreter divergence as overlay compiles land) | GP0 stream of bad frame has c[]=0 / wrong colors |
| H2 | VRAM-side texture problem: CLUT/texture region transiently black/garbage at draw time (upload timing / dirty-rect miss in GL texture path) | GP0 stream fine, vram_peek of CLUT/tpage black |
| H3 | GL renderer/wide-compositor state leak | `--renderer software` A/B; gl_fbo_peek vs CPU VRAM |

Ruled out by code review: GTE regfile sync round-trip (verified faithful:
SXYP push, IRGB lossy, FLAG mask all correct); upstream lm/sf lighting fix
(already in pin 678c71f); oversized-prim reject (latches texpage per hw).

## Capture harness — tools/glitch_capture.py

Continuous recorder against debug server :4370 (**requires build-dbg binary,
PSX_DEBUG_TOOLS=ON; build/ release has no server**). settings.toml copied to
build-dbg/ so video config matches the user's play config.

- screenshot ring @12.5 Hz + auto-detect (temporal-median black-patch,
  PIL+numpy; validated on synthetic frames: flags 1-frame black poly,
  ignores fades/cuts) -> auto burst: GP0 dump flagged frame ±4, both VRAM
  halves, full VRAM hex, gpu/gte/overlay/sljit/dma state
- manual burst: touch /tmp/xg_cap/TRIGGER
- output: /tmp/xg_cap/capture_* + ring.log

Run: `~/xenogears-port/.venv/bin/python -u tools/glitch_capture.py monitor`

### Protocol lesson (2026-07-18, cost one wasted play session)

The debug server is STRICTLY one-command-per-connection
(`io_thread_main`: accept -> recv ONE line -> reply -> close). The first
monitor version held one connection open and died on its first command —
the user then played a full session unrecorded. Client rewritten to
per-command connections (upstream `debug_client.py query()` model); the
lock-free `ping` fast-path always returns `id:0`, so never match replies
by id. Validated headless: ping/frame/screenshot/gpu_frame_dump/
vram_peek/dump_buffer all OK.

## Classification plan (when captures land)

1. Eyeball flagged PNGs -> confirm true bad frames.
2. Diff gp0_f<bad>.jsonl vs adjacent good frame (same prim src addresses):
   - colors differ/zero -> H1 (guest-side: GTE/packet build)
   - streams identical -> check vram_full.hex CLUT/tpage -> H2 (VRAM timing)
   - both clean -> H3 (renderer) -> A/B `--renderer software`
3. H1: check sljit_async compile times vs bad frames; force full-interp of
   the field module (overlay_native_block) -> if bug vanishes, recompiler
   bug in that fn -> twin-test -> fix recompiler, regenerate.
4. H2: wtrace the black CLUT/tpage VRAM addrs; mmio_dump DMA2/CD timing.

## Evidence (2026-07-18, session 2, frames ~4900-5344)

GP0 ring diffed per-frame, same-OT-parity (game renders 30fps into 2 OTs;
captured frames N and N+4 share an OT). Results across both big manual
captures (4902-5200, 5044-5344):

- **transient black vertex colors: 0** (every frame). No blended/shaded
  textured poly ever goes to all-zero colors for one frame. H1 (guest-side
  GTE/packet shading) effectively RULED OUT.
- **transient opcode (texture-bit) loss: 0**. The guest never emits an
  untextured variant of a textured poly for one frame.
- transient tpage/clut word changes: frequent (up to ~55/frame) but fire
  on 10-20% of frames — far more than the user's rare sightings — and
  repeat the same (from,to) pairs across many frames = the game's own
  texture animation, not the bug.

CPU-side VRAM at f5300 (vram_full.hex): texture regions 82-100% nonzero,
CLUTs 15-16/16 nonzero. Data side is fine.

=> Corruption is AFTER the command stream + data: presentation path.
User settings: renderer=opengl, frame_interpolation=true, 16:9 wide,
2x supersampling. Prime suspects: GL renderer texture-upload/dirty-rect
coherency (gpu_gl_renderer.c — has a documented prior stale-CPU-VRAM
coherency bug, s_up_rects), and/or frame interpolation fabricating
frames (Xenogears fields are 30fps -> half of displayed frames are
interpolated).

## Decisive A/B (no rebuild; launch-flag overrides)

- A — disable interpolation:
  `PSX_FRAME_INTERPOLATION=0 ./build-dbg/XenogearsRecomp --game game.toml --bios psxrecomp/bios/SCPH1001.BIN`
- B — software renderer:
  `./build-dbg/XenogearsRecomp --game game.toml --bios psxrecomp/bios/SCPH1001.BIN --renderer software`

Whichever clears the artifact isolates the layer. Monitor upgraded to
also capture gl_vram_diff / gl_coh_ring / gl_present_ring / gl_diag on
each burst -> if it reproduces under GL, FBO-vs-CPU-VRAM mismatch is the
smoking gun for the coherency bug.

## Session 3 — A/B result + overlay hypothesis (2026-07-18)

Both A (no interpolation) and B (software renderer) STILL GLITCH. =>
NOT interpolation, NOT GL-renderer-specific. Since the SW renderer
samples CPU VRAM directly at draw time (no cache/FBO), a black poly
under SW = VRAM content or latched GPU texture state wrong at that
draw instant.

User observational details (decisive fingerprint):
- PURE SOLID BLACK (not garbage/holes) -> texel = 0x8000 (black-opaque
  CLUT entry); latched texture state points to a wrong region
- A FEW FRAMES (2-10, not 1) -> latched state wrong then re-syncs
- WHILE STANDING STILL -> guest logic idle/deterministic; trigger is
  in the RUNTIME's timing/scheduling layer, not game code
- RANDOM DIFFERENT POLYS each time -> latched texpage/clut/TW wrong,
  hits whichever polys sample during the bad window
- NON-REPEATABLE ("fails once, that's it") -> a one-time transition

=> Overlay hypothesis (user's call, 99%): overlay code running on the
interpreter (before async compile lands) has different timing than
compiled code. Xenogears' field loop is vsync/DMA-timing-sensitive.
When a function transitions interpreted->compiled, the timing shifts
for a few frames -> GPU DMA completion desyncs -> latched E1/E2 state
lands on the wrong frame -> random polys sample wrong -> solid black.
Non-repeatable because the transition happens once; once compiled,
timing is stable.

Telemetry commands (correct names at pin 678c71f — sljit_async /
overlay_state are NOT registered):
- overlay_native_ring: dispatch events tagged with FRAME NUMBER ->
  correlate install/transition with glitch frame
- overlay_diff_on + overlay_shadow_dump: native-vs-interp codegen
  divergence detector (runs each function both ways)
- overlay_dump / overlay_loader_status: compile/load status

Capture harness fixed to call these. Decisive capture: SW renderer,
trigger WHILE seeing the glitch (lasts a few frames = time to react).
The native ring in that burst shows whether an overlay install event
landed on the glitch frame.

---

## POSTSCRIPT (2026-07-18, A3): bug hunt shelved by owner; A3 produced the framework fix + native overlays

- The black-poly hunt was shelved (owner decision). Evidence collected
  remains valid if it's re-opened.
- A3 Step 1 done (capture on, 3 overlays captured incl. field module).
- A3 found + fixed an UPSTREAM regression: split-gen (41370a6) broke
  compile_overlays.py (monolithic _full.c no longer emitted; tool only
  read that name). Fixed locally with read_generated_c() (shard-aware).
  PENDING UPSTREAM PR (game-agnostic framework fix).
- 2/3 captured overlays compiled + cache-verified (native dispatch at
  BIOS boot >1M calls). Field module capture unusable (mixed variants:
  intro+field share 0x80199000 in one session) — needs a clean
  field-only capture in a later session.

## A3 session 2 (2026-07-18)

- Second capture session: [1] title/menu clean (470/472 valid seeds) ->
  whole-region shard built (2nd variant); [2] field still mixed-variant
  (46/183) -> whole-region skip, but the fragment pass recovered ~30
  interior-island shards; ~70 failed fragment triage files (1.6 GB)
  cleaned up.
- Cache now: 81 native shards. Step 2 verdict confirmed: session-1 and
  session-2 [1] have different CRCs but both compile cleanly as the same
  module -> bytes differ only in volatile data slots, code stable
  (Outcome A, statically compilable).
- Remaining: Step 4 coverage measurement in gameplay (native vs interp
  dispatch counters); clean field-only capture needs a boot->load-save
  route that skips the intro module from the 0x80199000 slot.
