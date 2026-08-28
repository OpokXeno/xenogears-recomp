# Walkmesh, Movement, And Collision Runtime

## 1. Scope

This chapter describes how Field consumes walkmesh geometry to move actors. The
stored section layout, triangle records, vertices, normals, and adjacency arrays
are defined in
[`../graphics/03-resource-formats.md`](../graphics/03-resource-formats.md).
Here they are runtime collision data rather than rendering data.

## 2. Numeric Conventions

| Quantity | Representation |
|---|---|
| Actor X, Y, Z position | Signed 16.16 fixed point |
| Per-frame X and Z displacement | Signed 16.16 fixed point |
| Walkmesh vertices and sampled floor height | Signed integer map units |
| Heading and camera yaw | 12-bit turn, modulo `0x1000` |
| Sine/cosine table values | Signed fixed point with scale `0x1000` |
| No floor or actor candidate | `0x7FFFFFFF` |

A complete turn is `0x1000`, so `0x400` is 90 degrees, `0x800` is 180
degrees, and `0x100` is 22.5 degrees.

## 3. Actor Movement State

The physical state used by this subsystem lives primarily in `ActorData`.
`FieldActor` holds the installed object and current/cached transforms:

| Location | Meaning |
|---|---|
| `FieldActor+0x0C` | Current model transform |
| `FieldActor+0x2C` | Previous or cached model transform |
| `FieldActor+0x4C` | Pointer to physical `ActorData` |
| `ActorData+0x00` | Primary actor/script flags |
| `ActorData+0x04` | Physical, collision, and attachment flags |
| `ActorData+0x08` | Current triangle on each of four layers |
| `ActorData+0x10` | Active walkmesh layer |
| `ActorData+0x12` | Walkmesh traversal scratch |
| `ActorData+0x14` | Current triangle material flags |
| `ActorData+0x18` | Collision half-width X |
| `ActorData+0x1A` | Collision/interaction height |
| `ActorData+0x1C` | Collision half-width Z |
| `ActorData+0x1E` | Solid/contact range |
| `ActorData+0x20` | Committed physical position in 16.16 |
| `ActorData+0x30` | Physical delta for the current update |
| `ActorData+0x40` | Accumulated or pending movement vector |
| `ActorData+0x50` | Active surface normal |
| `ActorData+0x60` | Local movement or transition offset |
| `ActorData+0x68` | Previous integer X, Y, Z snapshot |
| `ActorData+0x72` | Actor elevation anchor |
| `ActorData+0x74` | Actor currently touched or connected |
| `ActorData+0x76` | Movement speed |
| `ActorData+0x104` | Player or scripted target heading |
| `ActorData+0x106` | Current physical heading |

The exact structure foundations are in
[`03-runtime-objects.md`](03-runtime-objects.md).

## 4. Per-Frame Movement Flow

`FieldRunActorSimulationPasses` at `0x8008110C` owns the ordered physical update:

1. Run actor scripts, which may create movement intent.
2. Snapshot every actor's current integer position.
3. For each actor in ascending index, refresh material state and merge movement
   sources into the current-frame XZ displacement.
4. Resolve the displacement against walkmesh boundaries.
5. For the controlled actor, resolve actor collision and then floor altitude.
6. Resolve floor altitude for each eligible noncontrolled actor.
7. Apply gravity, jump state, and transform translation.
8. Dispatch contact scripts after ordinary actors have committed.
9. Copy delayed party-follower states.

Position writes made directly by scripts before this pass become the starting
point of the same frame's collision work. Follower placement occurs after the
ordinary collision loops and is not resolved a second time.

## 5. Walkmesh Query Model

Field installs each layer as parallel arrays:

- Signed vertex triples.
- Triangle vertex indices.
- Three adjacency entries per triangle.
- Per-triangle normals and plane data.
- Per-triangle material/traversal attributes.

The actor keeps a current layer and triangle, allowing local movement to begin
from a known polygon instead of scanning the complete map.

### Point containment

`0x8007B478` tests whether an XZ point lies in a triangle. It evaluates the
oriented edge equations for all three sides. Values on an edge are accepted.

### Floor interpolation

For a triangle with plane normal `(nx, ny, nz)` and a vertex `(vx, vy, vz)`, the
runtime obtains floor Y at `(x, z)` from the plane equation:

```text
y = vy - ((x - vx) * nx + (z - vz) * nz) / ny
```

Walkmesh Y uses the map's coordinate convention; actor Y is converted between
integer and 16.16 at the boundary.

### Candidate selection

`FieldWalkmeshFindContainingTriangle` at `0x8007B1C4` scans one layer and returns
the containing triangle together with its interpolated position and normal.
The actor movement resolver queries the preferred layers and chooses the nearest
permitted surface.

Selection considers:

- XZ containment.
- Vertical distance from the actor's current Y.
- Preferred layer bits.
- Per-triangle traversal/material attributes.
- Actor physical flags and current triangle material state.
- Special movement modes that relax the normal filter.

No candidate returns `0x7FFFFFFF`, never an ordinary height.

## 6. Crossing Triangle Edges

`FieldTraceWalkmeshMoveWithSlopeRules` at `0x8007BEF4` and the related trace at
`0x8007C694` resolve an XZ segment beginning in a known triangle:

1. Test the requested endpoint against the three oriented triangle edges.
2. If it remains inside, accept the endpoint and current triangle.
3. Otherwise identify the crossed edge.
4. Read that edge's adjacency entry.
5. Reject a boundary edge or a neighbor prohibited by actor/material state.
6. Enter an acceptable adjacent triangle and repeat with the same endpoint.

The traversal has a finite iteration limit. Valid content normally reaches the
endpoint after crossing only a small number of polygons.

### Boundary response

When direct traversal stops at a blocked edge, the runtime obtains the edge
segment and redirects the original requested movement along it while preserving
the original XZ magnitude. It then attempts the sliding endpoint. This preserves
tangential movement instead of cancelling the complete step.

Camera target clamping uses a separate upper-walkmesh trace at `0x8007CD80`,
documented in [`09-camera-control.md`](09-camera-control.md).

## 7. Committing Planar Movement

`FieldResolveWalkmeshMoveAndEdgeSlide` at `0x8007B814` and
`FieldResolveWalkmeshMoveWithSlopeProjection` at `0x8007BAC0` combine generated
movement with walkmesh traces and select the direct, side-probe, sliding, or
slope-projected result.

These helpers return a resolved displacement and elevation. Final commit occurs
later in `FieldEntityResolveMovementAndAltitude` at `0x80084A40`, with the
position write and transform synchronization at `0x80085444`. That commit
updates:

- X and Z position.
- Active layer and triangle.
- Current floor and material state.
- Transform translation used after simulation.

If the requested endpoint is rejected, the commit uses the accepted direct or
sliding endpoint. If neither path advances, XZ remains at the previous committed
position while other actor state can still advance.

## 8. Movement Sources

All sources eventually contribute to the same per-frame XZ displacement.

### Step movement

Scripted step movement stores a heading and signed speed. Sine and cosine table
lookups convert the polar value to XZ. Negative speed naturally reverses the
direction.

### Continuous movement

Opcode `21`, implemented at `0x8009E094`, writes movement speed at
`ActorData+0x76`. Movement routines combine that speed with current or target
heading until another instruction changes or clears movement state.

### Player movement

The player-control opcode converts the directional-pad nibble through a heading
table, subtracts camera yaw, and stores the resulting target heading at
`ActorData+0x104`. The movement pass turns current heading toward that target
with a bounded angular step before XZ velocity is generated.

### Knockback and scripted accumulation

Knockback, pushes, and script-driven offsets contribute through the pending
movement vector at `ActorData+0x40` and the local offset at `+0x60`. The common
resolver merges them before testing the final segment.

## 9. Terrain Alignment And Sliding

The movement resolver samples nearby floor points to derive terrain-facing
state. `0x8007B6C4` converts an obstructed direction into a boundary-aligned
slide, while `0x8007BAC0` also handles slope projection.

The terrain sampler probes approximately four map units around the actor. It
uses the height differences to form pitch/roll-like physical orientation and
to reject directions that exceed the permitted terrain response. These values
affect movement and actor state before any presentation transform is consumed.

The slide path uses the blocking edge's XZ direction. Its sign is selected to
retain the original movement's forward component, avoiding a reversal when the
actor reaches a wall obliquely.

## 10. Actor Collision

Walkmesh collision constrains terrain; a separate pass constrains actors.

### Pair filtering

Candidate actors are filtered by:

- Active/physical flags.
- Collision-disable and scripted-state flags.
- Layer compatibility.
- Vertical interval overlap.
- Horizontal collision radii.
- Party and controlled-actor policy.

The pair search uses `0x7FFFFFFF` when no candidate qualifies.

### Horizontal resolution

The pair solver treats actor footprints as circles in XZ. When their centers are
closer than the sum of their radii, it computes a separating displacement. The
controlled actor is resolved first, so later contact selection sees the
corrected position.

### Vertical support and standing

The collision pass can treat an eligible actor as a support surface when the
controlled actor is above it and their horizontal footprints overlap. It records
the supporting actor and uses its top as the altitude target.

## 11. Moving Platforms

Platform actors use the same actor records with platform-specific flags.

When an actor stands on a moving platform, the runtime:

1. Identifies the supporting actor during collision/altitude resolution.
2. Computes the platform's current-frame position delta from its snapshot.
3. Adds that delta to the supported actor.
4. Revalidates floor and layer state at the transported position.
5. Preserves the standing relation while vertical and horizontal overlap remain
   valid.

This occurs before contact dispatch, so interaction routines observe transported
positions.

## 12. Altitude, Jump, And Gravity

`FieldEntityResolveMovementAndAltitude` at `0x80084A40` is the common altitude
boundary. It compares the actor's current Y with the selected walkmesh floor or
actor support.

### Grounded state

When the actor reaches a valid support surface:

- Y snaps to the support height in 16.16.
- Vertical velocity and falling state are cleared as appropriate.
- Active layer, triangle, and material state are committed.

### Airborne state

When no support is reached, gravity updates vertical velocity and then Y. Jump
state controls the initial vertical impulse and landing transition. A missing
walkmesh candidate remains distinguishable through the `0x7FFFFFFF` sentinel.

### Placement helpers

Scripted placement resolves XZ and searches for a compatible floor instead of
writing an arbitrary Y in the ordinary case. Arrival records and saved party
placement use the same physical fields, then actor initialization establishes a
valid layer/triangle before the first regular frame.

## 13. Contact Boundary

Movement and collision do not execute contact scripts directly. They populate
the committed positions, vertical intervals, and nearest-contact state consumed
by `0x8008399C` later in the frame. The interaction/contact policy is documented
in [`10-triggers-contact-and-dialogue.md`](10-triggers-contact-and-dialogue.md).

## 14. Function Index

| Address | Role |
|---:|---|
| `0x8007B07C` | Compute triangle-plane height and normal |
| `0x8007B1C4` | Find containing triangle on one layer |
| `0x8007B478` | Test XZ point containment in a triangle |
| `0x8007B6C4` | Redirect a blocked move along an edge |
| `0x8007B814` | Resolve movement with center/side probes and edge slide |
| `0x8007BAC0` | Resolve movement with slope projection |
| `0x8007BEF4` | Trace a move with boundary and slope rules |
| `0x8007C694` | Trace a move with directional and boundary rules |
| `0x8007CD80` | Trace and clamp a camera walkmesh probe |
| `0x8007D3D4` | Resolve one-layer movement altitude and normal |
| `0x8008110C` | Ordered actor simulation passes |
| `0x80083288` | Query transformed model collision surfaces |
| `0x8008399C` | Dispatch post-collision contact routines |
| `0x80084158` | Resolve actor/model collisions and moving platforms |
| `0x8008492C` | Test whether entity movement work is required |
| `0x80084A40` | Resolve movement, altitude, gravity, and position |
| `0x80085444` | Commit physical position and synchronize transforms |
| `0x8009E094` | Set actor movement speed |
| `0x8009E574` | Place actor on the selected walkmesh layer |
