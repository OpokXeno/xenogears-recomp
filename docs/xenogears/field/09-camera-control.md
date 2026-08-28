# Field Camera Control

## 1. Scope

Field camera control selects a tracked actor, derives a walkmesh-constrained
target, applies manual or automatic orbit policy, smooths target and eye, and
maintains script completion state. The later conversion of this logical state
into presentation matrices is outside this chapter.

## 2. State Model

The camera has three operating modes at `0x800AF934`:

| Mode | Meaning |
|---:|---|
| 0 | Normal follow camera |
| 1 | Script-controlled camera |
| 2 | Reacquire normal follow camera |

Mode 0 follows the tracked actor. Mode 1 lets scripts move target, eye, yaw,
dip, depth, and scale independently. Mode 2 keeps smoothing toward the normal
follow solution, then returns to mode 0 when both target and eye XZ errors are
less than 128 units or when its reacquisition counter exceeds `0x40`.

The camera state includes:

- Current and desired target vectors.
- Current and desired eye vectors.
- Current yaw, desired yaw, and angular step.
- Current and desired dip.
- Scaled camera depth and scale.
- Independent target and eye smoothing divisors.
- Automatic-rotation and blocked-sector masks.
- One-shot movement counters and completion flags.
- Shake interpolation counters, per-axis amplitudes, and offsets.

Yaw uses the same 12-bit turn as actor headings. The runtime masks arithmetic to
`0x0FFF` and compares signed shortest-path angular differences.

## 3. Per-Frame Pipeline

The logical camera pipeline is:

1. `FieldUpdateEntitiesAndCameraMatrices` at `0x800739C0` runs actor simulation.
2. `FieldUpdateCameraTrackingMode` at `0x80073230` applies script, follow, or
   reacquisition policy.
3. `FieldUpdateCameraOrbitRotation` at `0x800726E8` advances manual or automatic
   orbit.
4. `FieldComputeTrackedCameraPose` at `0x80072A38` clamps the follow target and
   derives the desired eye from yaw, dip, depth, and scale.
5. `FieldCameraInterpolationUpdate` at `0x80072D74` advances target/eye
   interpolation, depth, and shake.
6. `FieldMatrixLookAt` at `0x80073750` consumes the completed logical vectors.

Camera logic therefore sees positions after ordinary actor collision but before
party followers are presented.

## 4. Follow Target

The tracked actor index is stored at `0x800B233E`. It normally matches the
controlled actor but `FE AA` can change it independently.

In normal mode the desired target begins at the tracked actor's committed
position. Actor and map state can add a vertical target offset. The desired eye
is then derived from target, yaw, dip, and camera depth.

The derivation uses signed sine/cosine table entries and the 12-bit yaw. Camera
depth is scaled before being projected into XZ and Y components, keeping orbit
orientation independent from zoom.

## 5. Walkmesh-Constrained Target

`FieldComputeTrackedCameraPose` prevents a following target from crossing a
blocked walkmesh edge:

1. Clamp the camera probe to map bounds.
2. Call `FieldCameraTraceWalkmeshBoundary` at `0x8007CD80` to traverse marked
   triangles in the upper walkmesh.
3. When traversal stops, obtain the boundary edge and clamped endpoints.
4. Intersect the relevant XZ lines through `0x800723E4` and project the target
   onto the permitted side of that segment.
5. Use the clipped XZ as the follow target.

This dedicated camera trace consumes walkmesh adjacency but does not alter actor
position, active layer, or triangle state.

## 6. Manual Orbit

In normal follow mode, logical input `0x0004` requests left orbit and `0x0008`
requests right orbit. A rotation proceeds by `0x40` per frame for eight frames,
covering one `0x200` sector.

Manual orbit is suppressed while camera-state flag `0x8000` at `0x800AF9D8` is
set. Script camera mode and active one-shot rotations use this lock to avoid
mixing player and script angular changes.

## 7. Eight-Sector Masks

Camera direction is divided into eight sectors. Sector bits are not stored in
ascending bit order:

| Sector | Mask |
|---:|---:|
| 0 | `0x10` |
| 1 | `0x20` |
| 2 | `0x40` |
| 3 | `0x80` |
| 4 | `0x01` |
| 5 | `0x02` |
| 6 | `0x04` |
| 7 | `0x08` |

`FieldScriptSetAutomaticCameraRotationSectorMask` at `0x8009A634` sets the mask
that may initiate automatic orbit correction. The camera compares actor motion
and current view direction, chooses a permitted sector, and starts the ordinary
eight-frame rotation.

`FieldScriptSetBlockedCameraSectorMask` at `0x8009A670` sets the mask used for
manual orbit. A requested destination in the blocked mask is rejected or
redirected toward the permitted neighboring direction.

These are distinct policy masks. Neither is a generic camera-enable flag.

## 8. Target And Eye Smoothing

Target and eye use independent integer divisors. For each coordinate, the
ordinary step is proportional to:

```text
(desired - current) / divisor
```

The updater stops changing a coordinate once its integer part matches the
destination's integer part; it does not force the remaining fixed-point fraction
to the exact destination. A requested divisor of zero is normalized to one.

`FieldScriptSetCameraSmoothingDivisors` at `0x8008FD40` writes the independent
target and eye values.

## 9. Script Camera Mode

`FieldScriptEnterScriptCameraMode` at `0x8008FB98`:

1. Selects mode 1.
2. Snapshots current yaw, dip, and scaled depth as script baselines.
3. Resets camera scale.
4. Initializes target and eye smoothing divisors to 12.
5. Sets the manual-orbit lock.

Once active, script operations can set or interpolate target and eye vectors,
move either vector toward actor positions, change yaw/dip/depth, or wait for a
one-shot operation to complete.

`FieldScriptLeaveOrReacquireFollowCamera` at `0x8008FC4C` has two paths:

- In script mode, a zero duration leaves immediately, releases the manual-orbit
  lock, and advances six bytes. Existing target or eye movement flags are not
  cleared by this path.
- A nonzero duration starts asynchronous mode-2 reacquisition, uses that value
  for both smoothing divisors, and advances three bytes.
- An invocation made while mode 2 is already active leaves its PC unchanged.

Reacquisition completes after both eye and target XZ errors satisfy the 128-unit
threshold, or when the counter exceeds `0x40` on the 65th update.

## 10. Scripted Motion And Completion

Scripted camera moves store a destination, a finite frame count, and an active
flag. Each update advances the relevant vector or scalar and decrements its
counter. The final update clears the flag after adding the last precomputed
integer delta; division truncation can leave a fixed-point remainder.

Waiting camera opcodes inspect those active flags. If work remains, the VM slot
rewinds to the same instruction and yields. Once the flag clears, the opcode
advances normally. This is the standard blocking pattern described in
[`04-scheduler-and-vm.md`](04-scheduler-and-vm.md).

Target and eye are separate channels. A script can move one while leaving the
other attached to its existing destination, and their waits observe the
corresponding channel rather than all camera work globally.

## 11. Yaw, Dip, Depth, And Scale

Yaw interpolation follows the shortest signed path in the 12-bit turn. Dip is a
signed vertical orbit angle. Depth controls target-to-eye distance, while scale
multiplies the stored depth baseline.

Script setup snapshots current depth after applying scale. Subsequent scale or
depth changes therefore compose from the captured value rather than repeatedly
rescaling an already rounded eye vector.

Orbit rotation advances at `0x800726E8`; tracked pitch and scale transitions
advance at `0x80072A38`; vector interpolation, depth, and shake advance at
`0x80072D74`. `0x80073230` orchestrates script target/eye movement and clears
the corresponding active flags that release VM wait operations.

## 12. Camera Shake

Camera shake maintains one XYZ offset state with:

- An amplitude-interpolation counter.
- Current and target per-axis amplitudes.
- Per-axis interpolation steps.

Each frame interpolates the amplitudes and multiplies each axis by a fresh random
sample to derive the offset. Reaching zero on a nonzero interpolation counter
finishes the amplitude change but does not disable shake. A setup targeting zero
on every axis follows the clear path. `FieldUpdateEntitiesAndCameraMatrices`
adds the same resulting XYZ offset to both eye and target before the look-at
boundary.

## 13. Entry And Reset

Field initialization establishes a deterministic follow camera before ordinary
frames begin:

1. Select the party leader as tracked actor.
2. Install map or arrival camera parameters.
3. Initialize yaw, dip, depth, scale, and sector policy.
4. Set current target and eye from the first follow solution.
5. Clear scripted move, reacquisition, and shake state.

Map transitions may preserve selected camera values, but a fresh Field entry
does not depend on uninitialized state from the preceding module.

## 14. Opcode Boundary

The primary and extended opcode catalogs list every camera instruction and its
operand layout:

- [`05-primary-opcodes.md`](05-primary-opcodes.md)
- [`06-extended-opcodes.md`](06-extended-opcodes.md)

Those instructions manipulate the logical state described here. `FE 8A` is not
a camera-control opcode and does not modify this state.

## 15. Function Index

| Address | Function |
|---:|---|
| `0x800723E4` | Intersect two XZ lines for boundary clipping |
| `0x800726E8` | Advance constrained camera orbit rotation |
| `0x80072A38` | Compute collision-constrained tracked camera pose |
| `0x80072D74` | Advance camera interpolation, depth, and shake |
| `0x80073230` | Apply script/follow mode and orchestrate camera updates |
| `0x80073750` | Consume eye and target in the look-at boundary |
| `0x800739C0` | Actor and camera simulation boundary |
| `0x8007CD80` | Trace and clamp a camera probe through upper walkmesh |
| `0x8008DB2C` | Change camera-tracked actor |
| `0x8008FB98` | Enter script camera mode |
| `0x8008FC4C` | Leave or reacquire follow camera |
| `0x8008FD40` | Set target and eye smoothing divisors |
| `0x8009A634` | Set automatic-rotation sector mask |
| `0x8009A670` | Set blocked manual-orbit sector mask |
