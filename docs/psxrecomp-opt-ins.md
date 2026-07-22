# Inventario de opt-ins de psxrecomp

Documento de referencia rápida: todas las opciones "opt-in" (desactivadas por defecto) que expone psxrecomp, su origen (PR upstream cuando aplica), cómo se activan, qué datos aceptan, qué afectan y si merece la pena probarlas en XenogearsRecomp.

**NO se ha probado nada de esto** — es solo documentación.

Fuentes: `psxrecomp/recompiler/src/config_loader.{h,cpp}`, `psxrecomp/runtime/src/main.cpp`, `psxrecomp/docs/internal/upstream/*`, `psxrecomp/docs/ecosystem-watch.md`, `psxrecomp/ENHANCEMENTS.md`, `psxrecomp/docs/config_schema.md`, `game.toml` y `docs/pin-history.md` de este repo.

Leyenda de estado en XenogearsRecomp (XG):
- ✅ **activo** — configurado y funcionando en `game.toml`
- 🚫 **rechazado/shelved** — probado o decidido en contra, con evidencia
- ⏸️ **diferido** — pendiente de prerrequisitos
- ❓ **candidato** — disponible, nunca probado aquí

---

## 1. Aceleración de cargas y timing

| Opt-in | Tipo / datos | Qué afecta | Origen | Estado XG / interés |
|---|---|---|---|---|
| `runtime.turbo_loads` | bool | Quita el pacing de wall-clock durante cargas de CD (no XA/FMV): el guest corre a velocidad host. Debounce 4/6 frames | In-tree (LOAD_TIME_ZERO L1.1) | 🚫 Shelved deliberadamente (pin-history:42-45): el bug de black-polys (burndown 006) se sospecha desync de timing; reactivar tras estabilizar A4. **Interesante a futuro** |
| `runtime.turbo_audio_sink` | bool | Con turbo_loads activo, renderiza el presupuesto exacto de samples SPU pero los descarta (evita audio acelerado). En QA de audio | In-tree | 🚫 Mismo bloqueo que turbo_loads |
| `runtime.idle_skip` | bool (`PSX_IDLE_SKIP=1`) | Fast-forward de bucles de polling CPU sin stores/MMIO con estado de registros estable. Proof-gated | In-tree (LOAD_TIME_ZERO L1.2/E2) | 🚫 Mismo bloqueo. Smoke cross-game pasado en MMX5/6 |
| `runtime.disc_speed` | `"1x"`/`"2x"`/`"4x"`/`"instant"` | Timing del CD-ROM. `instant` colapsa seeks a 1 ciclo | In-tree (disc-speed.md) | 🚫 `"instant"` cuelga en el logo PS y crashea el worldmap. `"4x"` es estable en Tomba → **❓ candidato de bajo riesgo** (probar worldmap). Hoy `"1x"` |
| `runtime.instant_max_per_frame` | int 1..4096 | Presupuesto de IRQs de sector por vblank con `disc_speed="instant"` | In-tree | Irrelevante mientras instant cuelgue |
| `[[runtime.warm_cd_routes]]` | tabla: `arm_lba`, `lbas=[...]`, `instant_max_per_frame` | Aceleración acotada: un SetLoc en arm_lba seguido de la secuencia exacta de LBAs pasa solo esas lecturas a cadencia instant; falla cerrado a disc_speed. Máx 16 rutas, 1-64 LBAs | In-tree (config_schema: "intentionally opt-in") | ⏸️ Diferido: necesita evidencia de LBAs de Xenogears vía traza PCSX-Redux (SETUP Step 8). **Interesante a medio plazo** |
| `[load_accel.vsync_query]` | 5 direcciones hex + arrays de event-horizon (todo o nada) | Short-circuit de `VSync(mode)` PsyQ byte-verificado: `mode=-1` devuelve el contador saltándose 2 lecturas MMIO | In-tree | ❓ Sin investigar en XG. Requiere RE del VSync del juego |
| `[data_shards] funcs` | array de direcciones hex | Replay memoizado de funciones puras (hooks enter/return) | In-tree (LOAD_TIME_ZERO L1.4) | 🚫 **RECHAZADO upstream**: verificador temporal v1 insound, corrupción de texturas en Tomba. No tocar |

---

## 2. Boot y BIOS

| Opt-in | Tipo / datos | Qué afecta | Origen | Estado XG / interés |
|---|---|---|---|---|
| `runtime.bios_hle` | bool (`PSX_BIOS_HLE=0`) | Tier HLE de servicios del kernel BIOS; el resto cae a LLE recompilado. **Default ON** (opt-out) | In-tree (pivote 2026-07-06) | 🚫 Explícitamente `false` en XG (LLE puro). **❓ candidato de bajo riesgo**: el fallback a LLE lo hace seguro de probar; podría arreglar sutilezas de scheduler |
| `runtime.bios_hle_keep_intro` | bool (`PSX_BIOS_HLE_KEEP_INTRO`) | Con HLE, mantiene la animación de boot Sony/PS en vez de saltarla | In-tree | Cosmético; solo relevante con bios_hle=true |
| `runtime.hle_scheduler` | bool (`PSX_HLE_SCHEDULER=0`) | Scheduler TCB determinista vs host-fiber legacy. **Default ON** (opt-out) | In-tree | Activo por defecto incluso con bios_hle=false |
| `runtime.fast_boot` | bool | **DEPRECATED**: alias del shell-skip HLE (salta animación de boot, kernel init real sigue ejecutándose) | In-tree | Obsoleto; usar bios_hle |

---

## 3. Vídeo / presentación

| Opt-in | Tipo / datos | Qué afecta | Origen | Estado XG / interés |
|---|---|---|---|---|
| `video.supersampling` | int 1..4 | SSAA interno: renderiza a N× y downsampla. Coste ~N² en fill | In-tree | ❓ Candidato estético obvio para XG (3D poligonal). Probar 2 en escena de campo pesada |
| `video.texture_filtering` | `"nearest"`/`"bilinear"` | Suavizado de texturas y fondos 2D | In-tree | ❓ En XG mezcla fondos pre-renderizados + 3D; bilinear puede desentonar en fondos. Probar y comparar |
| `video.renderer` | `"software"`/`"opengl"`/`"vulkan"` | Backend de rasterizado. Vulkan experimental, oculto en launcher | In-tree; sync Vulkan de **PR #16** (shaneomac1337) NO integrada (corrupción AMD) | OpenGL default. Vulkan: build opt-in (`PSX_ENABLE_VULKAN=ON`), no recomendado |
| `video.crt_filter` | `"raw"`/`"crt"`/`"composite"`/`"trinitron"` (`PSX_SCREEN`) | LUT de color de pantalla (modelo CRT). `raw` = byte-idéntico | **Backport gbarecomp** (SHADOW_ENHANCEMENTS.md) | ❓ Cosmético, seguro. Sin valor para debugging |
| `video.frame_interpolation` + `video.frame_interpolation_fps` | bool + int (0=refresh, ≥90) | Interpolación 30→display en presentación; no toca timing guest/audio | In-tree | ❓ XG es 30fps; candidato estético. Ojo: distinto de smooth_60 |
| `PSX_SMOOTH_60FPS=1` | env | Blending temporal midpoint de frames duplicados 30→60 Hz | **PR #14 (kem0x)**, smooth60 — integrado, off por defecto | ❓ Alternativa más simple a frame_interpolation. Probar una, no ambas |
| `video.vsync` | `"on"`/`"off"`/`"adaptive"` (`PSX_VSYNC`) | Modo de swap; el pacer mantiene 59.94 Hz igualmente | In-tree | Default on; off solo para medir latencia |
| `video.low_latency_input` | bool (**default ON**) | Re-muestrea el pad tras el pacer, antes del present | In-tree | Ya activo por defecto |

### 3.1 FMV skip

| Opt-in | Tipo / datos | Qué afecta | Origen | Estado XG / interés |
|---|---|---|---|---|
| `video.auto_skip_fmv` | bool | Salta FMVs al detectarlos (START-hold genérico si no hay direcciones) | In-tree | 🚫 Shipped `false` (burndown 007) |
| `video.fmv_skip_total_table` / `fmv_skip_movie_id` / `fmv_skip_end_total` | 2 direcciones hex + int | Skip instantáneo vía la propia ruta end-of-movie del juego (escribe el total de frames a end_total) | In-tree | ⏸️ No hay direcciones en el EXE principal: el reproductor es el módulo Movie (id 6) cargado de disco. Pendiente de importar ese overlay a Ghidra (Track B2) |
| `video.fmv_skip_no_xa` + `fmv_skip_no_xa_hold` | bool + int 4..3600 vblanks | Detección de FMV solo por actividad MDEC (películas silenciosas); latch de fast-forward | In-tree | ❓ XG tiene logos MDEC; riesgo de falsos positivos en stills de carga |

---

## 4. Audio

| Opt-in | Tipo / datos | Qué afecta | Origen | Estado XG / interés |
|---|---|---|---|---|
| `audio.spu_hq` | bool (`PSX_AUDIO_SHADOW=1`) | Re-render SPU float-shadow: resample Catmull-Rom + headroom float. Off = byte-idéntico al hardware | **Backport gbarecomp** (SHADOW_ENHANCEMENTS.md) | ❓ Candidato seguro y verificado. Interesante para el OST de XG; validar a oído en batalla + worldmap |
| (interpolación Gaussiana SPU) | sin flag | 4-tap Gaussian hardware + tabla 512 en el camino canon | **PR #16 (shaneomac1337)** — integrada como default | Ya activo (es el camino canon) |

---

## 5. Widescreen (EXPERIMENTAL en launcher)

Config general: `video.aspect_ratio` (`"16:9"` … `"32:9"`; 21:9 "stubbed and hidden"). En XG el widescreen ya está muy trabajado (burndown 008/009). Solo se listan los opt-ins **no usados aún** o con matiz; los ya activos (`gte_game_mode`, `hud_sprt_squash`, `nw_hud_corners`, cull sites, `auto_screen_x`, `guard_pixels=64`) están en `game.toml`.

| Opt-in | Tipo / datos | Qué afecta | Origen | Estado XG / interés |
|---|---|---|---|---|
| `widescreen.native_wide` | bool (**default ON**, TCP `ws_nw`) | Render-target wide nativo vs GTE-squash+stretch | In-tree | Activo por defecto |
| `widescreen.full_2d` | bool (`PSX_WS_FORCE_2D=1`) | Trata cada frame como gameplay (juegos 2D puros) | In-tree | No aplica (XG es 3D; gte_game_mode lo cubre) |
| `widescreen.sprite_tag_funcs` + `sprite_anchor_addr` | array hex + hex scratchpad | Hook `psx_ws_sprite_tag` por prim de billboard; re-squash X. Requiere regen | In-tree (WIDESCREEN.md) | 🚫 Decidido en contra (burndown 008): sin evidencia de billboards; actores 100% poligonales |
| `widescreen.nw_backdrop` | bool | Estira quad screen-space full-frame (cielo/degradado) al frame wide | In-tree | ❓ XG usa backdrops poligonales; probablemente nw_flat/phase aplican mejor |
| `widescreen.nw_flat_backdrop` | bool | Estira primitivos planos sin textura en el espejo wide | In-tree | ❓ Candidato si el cielo del worldmap es polígono plano |
| `widescreen.nw_phase_backdrop` | bool | Estira texturados emitidos antes del primer primitivo 3D sombreado del frame | In-tree | ❓ Candidato para cielos texturizados de campo |
| `widescreen.nw_textured_edges` + `nw_textured_edge_scale` | bool + int 0/100..400 % | Expande vértices texturizados ya fuera del límite 4:3 | **PR #15 (douglasjv)** — esquema integrado, paridad SW renderer pendiente | ❓ Para mallas finitas de arena/fondo. XG tiene arenas de batalla finitas: **interesante** si aparecen bordes |
| `widescreen.nw_full_mirror` | bool | Renderiza el espejo wide completo en vez de splice del centro | **PR #15 (douglasjv)** | ❓ Solo si la interpolación de polígonos que cruzan el borde se rompe |
| `[[widescreen.signed_x_bound]]` | array `{address, expected}` (expected=LUI) | Constantes Q16 con signo escaladas al campo wide; identidad en 4:3 | **PR #15 (douglasjv)** — integrado con issues abiertos (clamp 64 sitios) | ❓ Requiere RE de sitios LUI concretos |
| `widescreen.clear_reveal` | bool | Limpieza sintética de márgenes wide en boundaries de mapa | In-tree | ❓ Si aparece basura en bordes en transiciones de mapa |
| `widescreen.nw_left_hud_packet_lo/hi` | par de direcciones (lo<hi) | Ancla por tercios de pantalla solo prims de un pool HUD identificado | In-tree | ❓ Requiere identificar pools de paquetes HUD de XG |
| `[widescreen.bg2d]` (count/startcol/startx/stream_*/bufbase/cap/init + strides) | ~15 campos hex/int | Widen de bucle de tiles de fondos 2D puros | In-tree | No aplica directamente (fondos de XG son 3D/pre-renderizados, no tile-loops) |
| `[widescreen.backdrop] x_sites / unsquash_funcs` | arrays hex | Un-squash de backdrops por sitio | In-tree | ❓ Requiere RE |
| `[widescreen.dome] call_sites` | array hex | Hooks de cúpula/sky-dome | In-tree | ❓ XG worldmap podría tener sky-dome; sin investigar |
| `widescreen.offer` / `offer_ultrawide` | bools (offer default ON) | Si el launcher muestra el toggle widescreen / opción 21:9 | In-tree | offer on; 21:9 nunca playtesteado |
| `widescreen.cull.mask_or_sites` | array hex | NOP de ORs de trim de quadrant-mask | In-tree | 🚫 Comentado en game.toml con evidencia: "mask NOP solo no cambió nada"; el fix real fue `plane_nx_sites` |
| `widescreen.cull.auto_backdrop` | bool | Auto-detección de PRELOAD de columnas de backdrop lejano (regen) | In-tree | ❓ Inercial con auto_screen_x; probar en regen de overlays |

---

## 6. Overlays nativos (cache de DLLs)

| Opt-in | Tipo / datos | Qué afecta | Origen | Estado XG / interés |
|---|---|---|---|---|
| `runtime.overlay_cache` | bool | Cache de DLLs de overlays + captura a `overlay_captures.json` | In-tree | ✅ Activo (113 shards en cg7) |
| `runtime.overlay_capture_history` | bool | Historial durable: append de snapshots a `overlay_captures.addendum.jsonl` (anti hard-kill) | In-tree | ❓ Útil durante captura intensiva; sin riesgo |
| `runtime.overlay_capture_persist_dir` | path relativo (DEV-only) | Un JSON inmutable por snapshot cambiado | In-tree | DEV-only; producción lo deja vacío |
| `runtime.overlay_autocompile_cmd` / `_tcc` | comando shell | Auto-compila capturas bajo presión de interp sostenida | In-tree | ✅ Activo (script local `compile_overlays_fixed.py`) |
| `runtime.overlay_backend` | `"auto"`/`"gcc"`/`"tcc"`/`"auto-no-gcc"` (`PSX_OVERLAY_BACKEND`) | Selección de toolchain para compilar shards | In-tree | ✅ `"auto"` |
| `runtime.overlay_native_block` | array hex | Funciones que deben quedarse en intérprete aunque exista DLL nativo (rutinas timing-sensibles) | In-tree | ❓ Herramienta de bisect si un shard nativo se comporta distinto |
| `PSX_OVERLAY_CACHE_INVENTORY=1` | env | Log del inventario de cache al arrancar | **PR #13 (NyperYuhgard)** | Diagnóstico puntual |
| `PSX_OVERLAY_NATIVE_OFF`, `PSX_NATIVE_BLOCK`, `PSX_NATIVE_RANK_LIMIT` | env | Forzar off nativo / bloquear por env / limitar rank (bisección) | In-tree | Diagnóstico de divergencias interp/nativo |
| `PSX_OVERLAY_DIFF` (+`_ADDR`), `PSX_OVERLAY_FP_LOG` | env | Shadow diff nativo/interp | In-tree | Diagnóstico |
| `PSX_OVERLAY_IRQ_*` (SUPPRESS, RATELIMIT, BUDGET, NO_CDROM, DEFER_CDROM, POST_PUMP) | env | Estrategias de chequeo de IRQs durante ejecución nativa | In-tree | Diagnóstico de cuelgues con overlays nativos |
| `PSX_OVERLAY_IMAGE_WARM=0`, `PSX_OVERLAY_UNIT_DEFER=N` | env | Desactivar warm preload / diferir carga de unidades | In-tree | Diagnóstico/perf |

---

## 7. Recompilador (codegen / discovery)

| Opt-in | Tipo / datos | Qué afecta | Origen | Estado XG / interés |
|---|---|---|---|---|
| `recompiler.discovery` | `"whole-image"`/`"reachable"` | `"reachable"`: parte del entry + seeds y sigue `jal` directos; indirectos caen al intérprete | **PR #15/PR #19 (douglasjv, MM8)** — integrado | 🚫 No interesa: whole-image funciona y no hay gaps de dispatch |
| `[[recompiler.patch]]` | array `{id, address, expected, replacement, note?}` | Reemplazo exacto de una palabra MIPS antes del discovery, guardado por opcode | **PR #15 (douglasjv)** — integrado vía PR #22 | ❓ Cero parches hoy. **Interesante** como mecanismo si se identifica un bug de código del juego a fijar en build |
| `game.text_size` | hex | Cota de análisis para discovery reachable | **PR #19 (douglasjv)** | ✅ Ya fijado (`0x4A000`, recortado deliberadamente) |
| `--ws-config <toml>` (CLI recompilador) | path | Carga solo las listas `[widescreen]` para compilar overlays | In-tree | Usado por compile_overlays |

---

## 8. Build (CMake)

| Opción | Default | Qué afecta | Origen |
|---|---|---|---|
| `PSX_ENABLE_VULKAN` | OFF | Compila el backend Vulkan experimental (SDK + glslc) | In-tree; sync de PR #16 no integrada |
| `PSX_DEBUG_TOOLS` | ON en Debug/RelWithDebInfo | Servidor TCP debug + heartbeat + recording | In-tree |
| `PSX_LAUNCHER` | ON | Launcher RmlUi integrado | In-tree |
| `PSX_STATIC_RUNTIME` | ON en MinGW Release | Enlace estático SDL2/libgcc | In-tree |
| `PSXRECOMP_STATIC_CLI` | OFF | MinGW estático en herramientas CLI | In-tree |
| `PSXRECOMP_SKIP_BIOS_STALE_CHECK` | OFF | Salta el chequeo de staleness de generated/ BIOS | In-tree |

Ninguna está modificada en el CMakeLists raíz de XenogearsRecomp.

---

## 9. Controles / launcher

| Opt-in | Tipo | Qué afecta | Estado XG |
|---|---|---|---|
| `controller.default_mode`/`p1_mode`/`p2_mode` | `"hybrid"`/`"analog"`/`"digital"` | Modo de pad por puerto | `controller = "digital"` en XG |
| `controller.allow_hybrid` | bool (default true) | Si el launcher ofrece Hybrid | Default |
| `controller.lock_mode` / `lock_device` | bools | Ocultan selectores de pad/dispositivo en launcher | Default (off) |
| `controller.deadzone` | int 0..32767 | Deadzone de stick (default 12000) | Default |
| `controller.legacy_pad_config` | bool | Protocolo de config de pad pre-98aa688 | Solo Tomba lo usa; no tocar |

---

## 10. Debug / diagnóstico (env, todo off por defecto)

Uso puntual, no features de juego. Los más relevantes para XG:

| Env var | Efecto |
|---|---|
| `PSX_DEBUG_SERVER=1` | Servidor TCP debug en builds Release |
| `PSX_RUNTIME_PERF_DIAG=1` (+`_MS`) | Informe de rendimiento por intervalo |
| `PSX_BENCH_WINDOW=start:end` | Ventana de frames para benchmark |
| `PSX_PARITY_TRACE=1` | Traza de control-flow para comparación cross-process |
| `PSX_DEVTRACE=1` | Traza de eventos de dispositivo por ciclo |
| `PSX_EXIT_HALT=1` | En salida anormal, halt-and-serve (inspeccionable en vivo) |
| `PSX_FORCE_INTERP=1` | Fuerza intérprete en todo |
| `PSX_MMIO_WAIT=0`, `PSX_LOAD_DELAY=0`, `PSX_ICACHE=0` | Apagan emulación de wait-states MMIO / load-delay / I-cache |
| `PSX_POLL_PROOF=N` | Nivel de poll-proof para aceleración de cargas |
| `PSX_KERNEL_BLESS=<level>` | Byte-verify de código kernel |
| `PSX_CD_TRAP_CMD/NTH`, `PSX_CD_DMA_TRACE=1` | Trampas/trazas de CD-ROM |
| `PSX_GL_FORCE_CPU_PRESENT=1`, `PSX_GL_PERF=1`, `PSX_GL_INTERP_DIAG=1` | Diagnóstico del renderer GL |
| `PSX_COSIM_PORT/STRIDE/START_CYCLE` | Co-simulación TCP |
| `PSX_FNTRACE_ALL=1`, `PSX_STACK_GUARD_KB=N`, `PSX_DISPLAY_RING`, `PSX_RECORD_FRAME=n`, `PSX_READ_WATCH=addr` | Instrumentación variada |
| `PSX_FIBER_STACK_KB`, `PSX_RECURSION_LIMIT`, `PSX_STARVATION_TIMEOUT_US`, `PSX_PRECISE_*`, `PSX_MIXED_*`, `PSX_XPROBE_*` | Tuning/diagnóstico del scheduler e intérprete |
| `PSXRECOMP_AUDIO_LEGACY=1` | SDL_QueueAudio legacy sin bridge |

CLI runtime: `--debug-port`, `--renderer`, `--window-title`, `--memcard-dir`, `--launcher`/`--no-launcher`, `--headless` (`PSX_HEADLESS=1`), `--bios`, `--game`, `--disc`.

---

## 11. Features upstream NO integradas (watchlist)

De `ecosystem-watch.md` — no existen como opt-in hoy pero están en cola; si alguna aterriza, conviene reevaluar:

| Feature | PR origen | Relevancia para XG |
|---|---|---|
| `feat/cdda-playback-kem0x` — CD-DA Red Book completo | PR #14 | **Alta**: XG usa audio XA mayormente, pero pistas CD-DA existen en algunos discos |
| `fix/sw-raster-exclusive-edges-kem0x` — edges half-open en raster SW | PR #14 | Baja (usamos GL) |
| `fix/gpu-primitive-size-reject-shaneomac` — rechazo 1023×511 completo (polylines) | PR #16 | Media: correctness hardware |
| `fix/dirty-text-page-mark-nyper` — invariante dirty/native dispatch | PR #13 | Media: robustez overlay cache |
| `feat/widescreen-hud-regions-douglas` — regiones HUD polígono/línea tipadas | PR #15 | **Alta** si el HUD wide necesita más precisión que hud_sprt_squash |
| `feat/widescreen-signed-bounds-douglas` — limpieza de hazards de signed bounds | PR #15 | Media |
| `fix/macos-core-gl-context-douglas` | PR #15 | Nula (Linux/Windows) |
| `feat/web-runtime-*` (Emscripten), Android runtime | PR #14/#15 | Nula hoy |
| Proyección con precisión + texturas perspectiva-correcta | PR #14 (kem0x) | **Alta potencial**: subpixel GTE + UV perspectiva-correcta mejoraría el warping típico de PS1 en XG; hoy ni siquiera integrado como experimento |
| CRT-Royale shader | PR #16 | Bloqueado por licencia |

---

## 12. Resumen: qué probaría en XenogearsRecomp (priorizado, sin probar aún)

1. **`audio.spu_hq = true`** — riesgo ~0 (verificado byte-idéntico en off), mejora audible potencial. Validar en batalla + worldmap.
2. **`video.supersampling = 2`** — mejora visual directa en un juego 3D; medir frametime en campo pesado.
3. **`runtime.disc_speed = "4x"`** — estable en Tomba; probar cargas + worldmap. `instant` sigue prohibido.
4. **`runtime.bios_hle = true`** — fallback a LLE lo hace seguro; observar scheduler/cargas.
5. **`video.frame_interpolation` o `PSX_SMOOTH_60FPS`** (uno solo) — estético, sin tocar timing.
6. **`[[runtime.warm_cd_routes]]`** — tras obtener LBAs reales (PCSX-Redux, SETUP Step 8). El mayor win de cargas con riesgo acotado.
7. **`widescreen.nw_textured_edges` / `nw_phase_backdrop` / `nw_flat_backdrop`** — solo si se observan artefactos de borde en 16:9.
8. **`runtime.overlay_capture_history = true`** — endurecer la captura de shards ante kills.
9. **turbo_loads / idle_skip / turbo_audio_sink** — mantener OFF hasta resolver el black-poly (burndown 006) y estabilizar A4.
10. **`[[recompiler.patch]]`** — tenerlo en el arsenal para cuando aparezca un bug de código del juego que convenga parchear en build.
