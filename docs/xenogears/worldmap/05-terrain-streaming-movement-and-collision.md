# Terrain, Streaming, Movement, And Collision

## 1. Toroidal World Space

Every configuration is `16 x 16` terrain slots. One slot spans `0x800000` live
fixed-point units on each axis, so the complete wrapped width and depth are:

```text
world_extent = dimension * 0x800000
```

`WorldMapWrapPosition` at `0x80093354` wraps live fixed-point X/Z coordinates.
Separate helpers wrap integer model positions and choose camera-relative or
model-relative shortest deltas. This makes both axis edges continuous for
travel, followers, cameras, models, effects, and collision.

## 2. Terrain Hierarchy

```text
16 x 16 world slots
    |
    +-- one slot: four quadrants
            |
            +-- one quadrant: 9 x 9 samples
                    |
                    +-- rendered area: 8 x 8 quads
```

The four quadrant payloads occupy `4 * 0x144 = 0x510` bytes. A query selects the
world slot, one of four quadrants from the half-slot coordinates, and a sample or
cell inside that quadrant.

## 3. Packed Terrain Samples

Each `uint32` sample combines geometry and rendering metadata:

| Bits | Meaning used by World |
|---:|---|
| `0..7` | Signed height, scaled by eight. |
| `8..10` | Ground texture page. |
| `11` | Fog-palette bank. |
| `12` | Animated-water displacement. |
| `13..14` | UV rotation. |
| `15` | Quad diagonal. |
| `16..19` | Base U cell. |
| `20..23` | Base V cell. |
| `26..29` | Encounter region `0..15`. |

`WorldMapGetAltitude` at `0x80093978` chooses the triangle selected by bit 15,
builds its plane from neighboring heights, and solves Y at the query point.
`WorldMapCalculateTerrainNormal` at `0x80093740` returns the corresponding
normalized surface normal. `WorldMapGetElevationWithWater` at `0x80093A5C` adds
the animated wave terms for water-aware vehicle contact.

## 4. Material Cells And Ground Type

Each slot has a `16 x 16` table of `uint16` materials at `+0x510`. Each terrain
quadrant covers one `8 x 8` subset of this slot-wide table:

| Bits | Meaning |
|---:|---|
| `0..3` | Partition shape/orientation. |
| `4..6` | Ground class on one side. |
| `7..9` | Ground class on the other side. |

`WorldMapGetGroundType` at `0x80093F18` uses the low-nibble shape to select a
partition line, evaluates the query point against it, and returns one of the two
three-bit classes. The same shape indexes edge directions used for sliding.

## 5. Travel Permissions

`WorldMapLookupTerrainPermission` at `0x80094060` returns the signed 16-bit entry
from a matrix indexed by numeric travel mode and ground class. Callers treat a
nonzero entry as permission to continue and zero as a blocked sample. Walking
and Gear movement select different rows; Yggdrasil landing validates
disembarkation with the Gear row, travel mode 2. The same material cell can
therefore admit one travel form and block another.

Permission is tested at every crossed material-cell boundary, spaced `0x80000`
live units apart, rather than only at the final point. Fast movement checks the
relevant X and Z crossings in path order and cannot skip a narrow blocked
partition.

## 6. Movement Resolution

Ordinary movement follows this sequence:

```text
current position + normalized step * speed
                    |
                    v
wrap candidate and sample terrain altitude
                    |
                    v
continue or test static-model collision mesh
                    |
          +---------+------------------+
          |                            |
   model response                no model response
          |                            |
          |                            v
          |              test crossed terrain boundaries
          |                    and final ground class
          |                            |
          +-------------+--------------+
                    v
          accept or derive slide vector
                    |
                    v
       caller retries slide once, then
             tests dynamic cylinders
```

`WorldMapCheckPosition` at `0x80095414` performs candidate wrapping and altitude
sampling before static-model handling. A continuing model triangle or blocking
model response can return without terrain traversal. When the model path falls
through, `WorldMapClassifyTerrainTraversal` at `0x80094A5C` performs boundary tests
and `WorldMapResolveTerrainCollision` at `0x800951A8` returns accepted movement or
a slide direction. The actor task retries once with that slide vector. A second
failure clears horizontal movement for the frame.

## 7. Edge Sliding

The material shape provides a direction tangent to its partition. On collision,
`WorldMapChooseTerrainSlideVector` at `0x80094088` and
`WorldMapOrientSlideVector` at `0x800952B0` orients that tangent with the requested
motion. Cardinal cell-boundary collisions instead produce an axis-aligned unit
vector.

This preserves motion along coastlines and diagonal terrain boundaries while
removing the blocked component. The resulting vector passes through the same
terrain checks before acceptance.

## 8. Static Model Collision

`WorldMapCheckStaticModelCollisions` at `0x80084D00` visits models whose runtime
collision-enable bit is set. For each candidate:

1. Perform a coarse wrapped X/Z distance check.
2. Compose the model transform and transform the query into model space.
3. Build each collision-triangle plane.
4. Test the point against the plane and classify it against triangle edges.
5. Publish the colliding model and response class.

Model collision is integrated with `WorldMapCheckPosition` at `0x80095414` for
walking and Gears, and with `WorldMapCheckYggdrasilPosition` at `0x80095CD4` for
flight.

The collision mesh is indexed rather than expanded into runtime triangles. Each
14-byte triangle contains three vertex indices, three neighboring-triangle
indices, and a surface type. A query can therefore continue across an indicated
edge instead of rescanning the complete mesh. For a blocking edge, World builds
normalized horizontal vectors from the transformed triangle sides and chooses
the edge that faces the attempted movement. The caller receives a surface class
and a slide vector; it does not receive a generic sphere or box response.

## 9. Dynamic Collision Cylinders

Moving objects and landmark tasks publish cylinders into a 32-entry per-frame
ring. Each `0x18`-byte record identifies the owner task, center position, radius,
and height.
`WorldMapProcessDynamicCollisions` at `0x8008C040` compares a candidate against
these cylinders using wrapped horizontal distance.

Vertical intervals must overlap before horizontal distance is considered. A
distance inside `candidate_radius + object_radius` returns contact class 2; the
next `0x10` units form proximity class 1. Both classes publish the low byte of the
owner ID. Party actors use that owner to board the Yggdrasil, while the Yggdrasil
uses configured owners as internal destinations. A travel task consumes cylinders
left by the preceding dispatch and resets the ring. Higher-numbered object tasks
then repopulate it later in the current dispatch for travel on the next frame.

This ring is not the entity-shadow marker queue. Moving tasks may publish the
same position to both systems, but collision records carry radius and height,
whereas marker records are consumed only by the ground presentation pass.

## 10. Streaming Grid

World keeps two `9 x 9` arrays of slot IDs: the desired grid and the prior grid.
The 256-entry chunk-pointer table maps each logical slot ID to its allocated
`0x710`-byte payload.

`WorldMapSetupInitialGrid` at `0x80097BC0` builds the initial wrapped IDs.
`WorldMapTriggerInitialLoading` at `0x80097DC0` loads the central `3 x 3` cells
synchronously, then queues the surrounding cells that complete the initial
`9 x 9` grid. Initialization drains that queue before terrain users start.

During travel:

1. `WorldMapSetGridUpdateMask` at `0x800980D4` detects camera-centered edge
   crossings and normalizes the local streaming origin.
2. `WorldMapUpdateGridIndices` at `0x800981C8` rebuilds all 81 wrapped IDs.
3. `WorldMapUpdateStreamingCells` at `0x80098CC0` frees departed cells and
   allocates entering slots.
4. Entering rows use file `+9`; entering columns use file `+10`.
5. Reads are sorted by sector and published as one queue batch.
6. `WorldMapServiceStreaming` at `0x800967E4` advances asynchronous CD reads and
   copies each arriving `0x710`-byte payload to its destination.

## 11. Streaming Queues

There are 16 pending batch slots and independent producer and consumer indices,
both wrapped with `& 0x0F`. The modular difference is the queue depth used by
initial loading, ordinary travel, and forced scripted reloads.

A batch builder accepts at most 88 entries. CD entries are `0x0C` bytes and PC
entries are `0x10` bytes; a zero first word terminates either list. Publishing a
batch fails if it is empty or if the selected ring position is still occupied.
Before publication, `WorldMapSortCDStreamingEntries` at `0x800963E4` orders CD
entries by sector. The PC sorter orders its four-word records by their file-offset
key. Both use insertion-style adjacent swaps, preserving the other words of each
record.

The CD callbacks handle command completion, sector delivery, gaps between sorted
ranges, partial final sectors, retry state, and batch completion. A separate PC
path uses file offsets but preserves the same logical queue and chunk ownership.

`WorldMapServiceStreaming` selects the backend from host-file availability:

| Backend | Service behavior |
|---|---|
| CD | Starts one queued batch when idle and advances a shared state machine through command issue, sector delivery, completion delay, and queue release. |
| PC | Processes every entry in the current batch synchronously, retrying open, read, and close up to eight times each, then releases the queue slot immediately. The intervening seek is attempted once. |

The CD status helper reports idle, busy, completed, or invalid state. Completion
clears the consumed ring pointer and advances the consumer index. Blocking drains
used during startup and mode-17 reload repeatedly service the backend and wait
until the modular queue depth reaches zero.

## 12. Visibility Versus Residency

Residency uses the full `9 x 9` grid. `WorldMapPrepareTerrainVisibility` at
`0x800983A0` derives a camera-centered `5 x 5` coarse set and per-quadrant masks
from frustum planes. `WorldMapDrawGround` then emits only visible quadrants.

This separation supplies terrain queries and imminent movement with a wider
resident margin than the renderer needs. Crossing one streaming boundary changes
only an entering strip instead of rebuilding every visible slot.
