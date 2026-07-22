# 009 — Widescreen: de RAM-pokes a transformaciones de instrucción (2026-07-20)

## Contexto

Upstream (mstan/psxrecomp) cerró el PR agregado #49 y lo partió en PRs
enfocados: #50 (range interp, abierto), #51 negsub, #52 vxrange, #53 depth
(los tres mergeados). **`xclip_globals` y `[[widescreen.cull.poke]]` NO se
aceptaron** como API de framework: el poke reescribe palabras de código en
RAM del guest → solo funciona en el intérprete; los shards nativos no lo ven
(comportamiento divergente interp/nativo), y el swap de un global RAM a
INT32_MAX se consideró demasiado acoplado al renderer de un juego concreto.

Este documento registra la migración de Xenogears a knobs que sí cumplen el
criterio upstream: **transformaciones de instrucción game-agnostic, identidad
a 4:3, con paridad intérprete/nativo**.

## Framework: rama `feat/widescreen-cull-xform` (submódulo psxrecomp)

Base `upstream/master` (incluye #51/#52/#53) + cherry-pick de #50 (range
mirror en intérprete; sigue abierto upstream) + 3 knobs nuevos, cada uno con:
parseo en `config_loader`, emit nativo en `code_generator` (strict: mismatch
= error; overlay: deja el código intacto), hook en `dirty_ram_interp`,
setter/predicado/helper en `gpu.c`, forwarder en el preámbulo de overlays
(**ABI v16**, codegen ver **7**) y tests en `recompiler_patch_test.cpp`.

### `xclip_load_sites` — sustituye a `xclip_globals`

Idiom: `lw rt,off(rs)` que carga el bound de rechazo-X por primitiva que luego
se compara (`sltu`) contra el screen-X enmascarado a 16 bits. Mientras los
márgenes están revelados, el load produce `INT32_MAX` (reject desactivado; el
scissor de la superficie wide recorta el overflow; los coords off-left
envueltos a 655xx pasan). Valor vanilla a 4:3. **Mismo efecto que el poke al
global 0x800500F8, pero aplicado en cada load en vez de en RAM.**

Sites (20 = xref-completo en Ghidra, todos `lw v1,0xF8(v1)` byte-idénticos):
los 17 de los renderers RTPT @0x8002E0A4-0x800307A4 + los 3 de FUN_80030EE8
(0x80030F50/0x80030FA8/0x80031004). El global Y @0x800500FC sigue intacto.

**Cobertura real: 16/20 nativa + 4 código muerto probado.** Las direcciones
0x8002E4DC/0x8002E6E0/0x8002E948/0x8002EB4C caen en un hueco de discovery del
recompiler (alcanzadas solo vía jump table @0x8004FED0/0x8004FF70 — los
targets indirectos no se descubren). Hueco preexistente e idéntico con el
recompiler viejo (2215) y el nuevo (2219): si alguna se ejecutara, el runtime
haría FAIL-FAST unknown-dispatch (no hay fallback a intérprete para el EXE
principal) — como el juego funciona, nunca se ejecutan.

### `plane_nx_sites` — sustituye a los 2 pokes de normales de plano

Idiom: `lw rt,off(rs)` que carga el componente X de la normal de un plano
lateral del frustum usado en un sign test (`dot = nx*px + nz*pz` por esquina).
Se escala por el **factor inverso de aspecto** `(4*den)/(3*num)` con
redondeo, mientras hay reveal (identidad a 4:3): el semi-ángulo queda
`atan((3*num)/(4*den)*tan θ)` — el widening exacto. A 16:9:
`±3474 → ±3474*36/48 = ±2606` — **idéntico al valor hardcodeado del poke**,
pero generaliza a cualquier aspecto (21:9 → 1985) y no reescribe código.

Sites: `0x80098850` (nx=+3474) y `0x80098974` (nx=-3474), los dos loads
dentro de computeTileVisibility (los dots se recomputan cada frame; solo
transformar el load del consumidor funciona — igual que el poke).

### `mask_or_sites` — sustituye a los 2 pokes NOP del merge de máscaras

Idiom: `or rd,rs,rt` que hace OR-merge de una máscara estática de trim (rt)
sobre la visibilidad computada (rs). Mientras hay reveal, rt se fuerza a 0
(merge suprimido: los planos escalados + los rejects por vértice deciden la
visibilidad). Identidad a 4:3 — mismo efecto que el NOP poke.

Sites: `0x8009874C`, `0x80098764` (loop de merge de
worldmapGroundPrepareRenderingTable).

Verificado en Ghidra que ni field_module.exe ni field_module2.exe tienen
instrucciones en esas 4 direcciones (no hay colisión entre variantes del
módulo @0x8006F000).

## game.toml

Fuera: `xclip_globals`, los 4 `[[widescreen.cull.poke]]` y la nota TOML de
array-of-tables. Dentro: `xclip_load_sites` (20), `plane_nx_sites` (2),
`mask_or_sites` (2), con la evidencia Ghidra en comentarios. El resto de
knobs (bias/negsub/range/vxrange/slti/depth) sin cambios.

## Verificación (2026-07-20)

- `recompiler_patch_test`: PASS en los 8 tests nuevos (parseo + emit + no-
  misfire en overlay) y en todo lo anterior. 1 FAIL preexistente en
  upstream/master puro, no relacionado ("parser rejects absolute capture
  history directory").
- Regen del EXE con `strict = true`: limpio; los 16 sites vivos emiten
  `psx_ws_xclip_bound(psx_cyc_load_word(...))`.
- Runtime rebuild (build-dbg) limpio; símbolos de los 3 helpers presentes.
- Caché de overlays regenerada (namespace `cg7_*`): 113 shards ok, 48 failed
  — todos en la región 0x80199000 (instrucciones no soportadas, preexistente,
  sin cull sites; corren interpretados como antes).
- `tools/boot_smoke.py build-dbg/XenogearsRecomp 40`: PASS (3176 frames, sin
  crash dumps).

## Nota operativa (sin cambios respecto a antes)

El módulo worldmap (@0x8006F000) aún no está en `overlay_captures.json` → al
entrar correrá **interpretado** (los hooks del intérprete cubren los knobs;
igual que cuando el fix era poke, que también era interp-only). Tras una
sesión que lo capture, recompilar como siempre:

```sh
cd build-dbg && python3 ../tools/compile_overlays_fixed.py \
  --captures overlay_captures.json --game-toml ../game.toml \
  --recompiler ../psxrecomp/recompiler/build/psxrecomp-game \
  --runtime-include ../psxrecomp/runtime/include \
  --out-dir cache --gcc /usr/bin/gcc --cps
```

Los shards nativos resultantes ya llevan las transformaciones (ver 7).
