# Complete Operation Catalog

This file is generated from `tools/field_opcode_table.json`. It lists all
256 primary opcodes and 227 extended opcodes known to the dispatcher.
The DSL column shows the usual rendering. Instructions with special syntax
may appear as assignments, conditions, or control-flow statements.

Opcodes and handler addresses remain hexadecimal because they are technical
identities. Arguments emitted in `script.xgs` are displayed in decimal.

## Summary By Object

| Object | Operations | Responsibility |
|---|---:|---|
| [`actor`](#actor) | 104 | Entities, characters, party members, sprites, and actor control. |
| [`movement`](#movement) | 66 | Position, rotation, walkmesh, movement, and collision. |
| [`world`](#world) | 21 | Maps, encounters, triggers, transitions, and scene state. |
| [`camera`](#camera) | 48 | Camera position, projection, tracking, and view geometry. |
| [`dialogue`](#dialogue) | 13 | Text, portraits, windows, and dialogue choices. |
| [`audio`](#audio) | 21 | Music, sound, channels, volume, and tempo. |
| [`visual`](#visual) | 38 | Fades, lighting, models, particles, effects, and video. |
| [`battle`](#battle) | 2 | Battle handoffs, Battling results, and return destinations. |
| [`inventory`](#inventory) | 19 | Inventory, items, currency, and menus. |
| [`input`](#input) | 7 | Button state and accumulated input history. |
| [`state`](#state) | 31 | Script-variable reads, writes, arithmetic, and bit operations. |
| [`flow`](#flow) | 41 | Jumps, calls, waits, yields, returns, and termination. |
| [`event`](#event) | 72 | Operations that do not belong exclusively to another subsystem. |

## `actor`

Entities, characters, party members, sprites, and actor control.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `0B` | 3 | `actor.initialize_npc_actor(...)` | `0x800A1624` | `InitializeNpcActor` | initializes the current NPC from a selected field graphic, synchronizes placement, enables updates and visibility, and advances three bytes. |
| `0C` | 1 | `actor.process_player_control_if_owned_preserve_ip()` | `0x8009F5A8` | `UpdatePlayerCharacterPreserveIP` | runs the player-control update and restores the caller's VM instruction pointer afterward. |
| `16` | 3 | `actor.bind_playable_character(character: value)` | `0x800A08B8` | `InitializePlayableActor` | initializes a playable-character actor, updates player and party mappings, selects party or replacement graphics, applies entry placement, and handles missing-party fallback. |
| `1E` | 1 | `actor.reset_actor_elevation_tracking(...)` | `0x8009E208` | `ResetActorElevationTracking` | clears the actor elevation offset, anchors elevation to the current Y position, and enables elevation updates. |
| `1F` | 2 | `actor.set_actor_low_flags_from_packed_byte(...)` | `0x8009E1A0` | `SetActorLowFlagsFromPackedByte` | replaces low actor flags from a packed byte. |
| `20` | 3 | `actor.set_current_actor_flags(...)` | `0x8009E10C` | `SetCurrentActorFlags` | decodes a script mask into current-actor flags. |
| `22` | 1 | `actor.self.visible = true` | `0x8009DF10` | `ShowActor` | shows the current actor. |
| `23` | 1 | `actor.self.visible = false` | `0x8009E040` | `HideActor` | hides the current actor. |
| `24` | 2 | `actor.visible = true` | `0x8009DDEC` | `ShowActorById` | shows an actor by ID. |
| `25` | 2 | `actor.visible = false` | `0x8009DE94` | `HideActorById` | hides an actor by ID. |
| `27` | 2 | `actor.stop_and_disable_actor_vm(...)` | `0x8009DC4C` | `StopAndDisableActorVM` | stops movement and disables actor VM state. |
| `28` | 2 | `actor.enable_actor_vm(...)` | `0x8009DBC8` | `EnableActorVM` | enables an actor VM. |
| `29` | 2 | `actor.disable_and_hide_actor_vm(...)` | `0x8009DAC4` | `DisableAndHideActorVM` | disables, hides, and closes actor dialogue. |
| `2A` | 1 | `actor.self.dialogue_enabled = false` | `0x8009DA1C` | `DisableDialogActivation` | disables dialog. |
| `2B` | 1 | `actor.self.dialogue_enabled = true` | `0x8009DA44` | `EnableDialogActivation` | enables dialog. |
| `2F` | 3 | `actor.write_cur_character_id(...) -> state` | `0x80099EF8` | `WriteCurCharacterID` | writes current character ID. |
| `30` | 3 | `actor.write_party_leader_character_id(...) -> state` | `0x80099F48` | `WritePartyLeaderCharacterID` | writes leader ID. |
| `52` | 2 | `actor.follow_actor(...)` | `0x800980FC` | `FollowActor` | continues planar walking toward a selected actor until contact range is reached. |
| `53` | 4 | `actor.follow_actor_with_step_limit(...)` | `0x80098038` | `FollowActorWithStepLimit` | continues planar walking toward a selected actor until contact range or the scripted step limit is reached. |
| `5C` | 3 | `actor.bind_party_slot(slot: value)` | `0x800A0228` | `InitializePartySlotActor` | binds the current actor to a selected party slot, restores its saved map placement when applicable, initializes its sprite, and hides unavailable or off-map members. |
| `6B` | 3 | `actor.rotate_actor_clockwise(...)` | `0x8009AB5C` | `RotateActorClockwise` | rotates actor clockwise. |
| `6C` | 3 | `actor.rotate_actor_counter_clockwise(...)` | `0x8009ABAC` | `RotateActorCounterClockwise` | rotates actor counter-clockwise. |
| `6F` | 2 | `actor.face_actor(...)` | `0x8009A2A8` | `FaceActor` | turns the current actor toward the selected actor when that actor is valid. |
| `70` | 2 | `actor.face_party_member(...)` | `0x8009A1E4` | `FacePartyMember` | turns the current actor toward the mapped party member when that member is present. |
| `7B` | 4 | `actor.decrease_party_hp(...)` | `0x800969FC` | `DecreasePartyHp` | VM party-HP decrease handler. |
| `7C` | 4 | `actor.increase_party_mp(...)` | `0x80096F18` | `IncreasePartyMp` | increases party MP. |
| `7D` | 4 | `actor.decrease_party_mp(...)` | `0x80097010` | `DecreasePartyMp` | decreases party MP. |
| `7E` | 4 | `actor.increase_party_hp(...)` | `0x80097108` | `IncreasePartyHp` | increases HP for each valid party member selected by the script mask, caps each result at maximum HP, and advances four bytes. |
| `89` | 6 | `actor.check_actor_distance_or_goto(label)` | `0x80095E48` | `CheckActorDistance` | tests actor distance. |
| `8A` | 4 | `actor.check_actor_on_screen_or_goto(label)` | `0x80095C00` | `CheckActorOnScreen` | tests actor screen visibility. |
| `91` | 4 | `actor.check_party_member_or_goto(label)` | `0x800964B0` | `CheckPartyMember` | tests party membership. |
| `92` | 1 | `actor.initialize_actor_scripts(...)` | `0x800A19B0` | `InitializeActorScripts` | resets all actor script slots. |
| `93` | 3 | `actor.add_current_actor_to_mecha_list(...)` | `0x800A1364` | `AddCurrentActorToMechaList` | initializes the current actor's base graphic, synchronizes its transform, assigns a mecha-list slot and model identifier, and increments the mecha count. |
| `A7` | 1 | `actor.process_player_control_if_owned()` | `0x8009F5F4` | `UpdatePlayerCharacter` | processes player movement eligibility, idle detection, directional input, movement triggers, and facing before advancing one byte. |
| `B9` | 4 | `actor.check_available_party_member_or_goto(label)` | `0x80096534` | `CheckAvailablePartyMember` | advances four bytes when the requested character is available, otherwise branches to the encoded destination. |
| `BA` | 2 | `actor.add_available_party_member(...)` | `0x800965A8` | `AddAvailablePartyMember` | sets the requested character's availability bit and advances two bytes. |
| `BC` | 1 | `actor.initialize_actor_sprite(...)` | `0x800A0D3C` | `InitializeActorSprite` | creates and initializes actor sprite state. |
| `C4` | 2 | `actor.rotate_actor_and_set_state_flag(...)` | `0x80093E30` | `RotateActorAndSetStateFlag` | rotates an actor over 30 steps and sets its state flag. |
| `C5` | 2 | `actor.rotate_actor_and_clear_state_flag(...)` | `0x80093FC0` | `RotateActorAndClearStateFlag` | applies the inverse rotation and clears its state flag. |
| `DD` | 6 | `actor.set_current_actor_blend_parameters(...)` | `0x80091E00` | `SetCurrentActorBlendParameters` | replaces rendering blend bits 5 through 6 from the low two operand bits, stores the accompanying rendering value, and advances six bytes. |
| `E0` | 7 | `actor.set_selected_actor_blend_parameters(...)` | `0x80091E98` | `SetSelectedActorBlendParameters` | resolves an actor and, when valid, replaces its rendering blend bits and accompanying rendering value before advancing seven bytes. |
| `F8` | 4 | `actor.update_current_actor_flag_quarter(...)` | `0x8008E59C` | `UpdateCurrentActorFlagQuarter` | sets or clears the selected lower or upper 16-bit quarter of either current actor flag group and advances four bytes. |
| `F9` | 2 | `actor.set_parent_actor(...)` | `0x8008DE64` | `SetParentActor` | assigns an actor parent. |
| `FE 02` | 5 | `actor.field_actor_inner_proximity_predicate_or_goto(label)` | `0x80095B3C` | `FieldActorInnerProximityPredicate` | checks the near-screen actor range. |
| `FE 03` | 4 | `actor.set_current_actor_uniform_scale(...)` | `0x8008D0F4` | `SetCurrentActorUniformScale` | sets uniform actor scale, applies a three-quarter sprite scale, refreshes rotation, and advances three bytes. |
| `FE 04` | 4 | `actor.set_current_actor_sprite_geometry_scale(...)` | `0x8008D26C` | `SetCurrentActorSpriteGeometryScale` | doubles the supplied value into the current actor's sprite geometry scale and advances three bytes. |
| `FE 07` | 3 | `actor.set_current_actor_flag400_from_mode(...)` | `0x8008D604` | `SetCurrentActorFlag400FromMode` | clears actor flag 0x400 for mode zero, sets it for mode one, and advances two bytes. |
| `FE 08` | 8 | `actor.set_current_actor_axis_scales(...)` | `0x8008D180` | `SetCurrentActorAxisScales` | sets independent actor X, Y, and Z scales, restores the sprite scale to 0xC00, refreshes rotation, and advances seven bytes. |
| `FE 09` | 4 | `actor.toggle_actor_mecha_suppression(...)` | `0x8008D078` | `ToggleActorMechaSuppression` | clears actor flag 0x800 for a zero operand and sets it otherwise, controlling mecha and shadow suppression. |
| `FE 15` | 6 | `actor.initialize_actor_graphic_variant(...)` | `0x800A14F0` | `InitializeActorGraphicVariant` | initializes the current actor from a selected field graphic and variant, synchronizes placement, enables updates and visibility, and advances five bytes. |
| `FE 17` | 4 | `actor.face_actor_toward_actor(...)` | `0x8009AA00` | `FaceActorTowardActor` | turns the first selected actor toward the second selected actor when both are valid. |
| `FE 18` | 5 | `actor.add_immediate_party_character(...)` | `0x8008BDD8` | `AddImmediatePartyCharacter` | reserves a staging slot and begins loading an immediate party character, or marks an already staged character as present. |
| `FE 1A` | 2 | `actor.finalize_party_character_load(...)` | `0x8008B894` | `FinalizePartyCharacterLoad` | waits for staged loading, decompresses the character resource, releases compressed data, and initializes the first entry actor targeting that character. |
| `FE 1E` | 3 | `actor.switch_map_to_gears(...)` | `0x8009FB98` | `SwitchMapToGears` | synchronizes assets and marks the Field map for Gear mode. |
| `FE 1F` | 2 | `actor.mount_current_party_actor(...)` | `0x8009FDD4` | `MountCurrentPartyActor` | maps the current actor to a party slot, mounts it when not already riding a gear, and advances one byte. |
| `FE 20` | 3 | `actor.dismount_party_character(...)` | `0x8009FE4C` | `DismountPartyCharacter` | resolves the selected character to a party slot, dismounts it when currently riding a gear, and advances two bytes. |
| `FE 21` | 4 | `actor.initialize_party_character_actor(...)` | `0x800A06E8` | `InitializePartyCharacterActor` | resolves a character into the active party, binds its party graphics and synchronized transform, or creates a disabled placeholder when absent. |
| `FE 24` | 2 | `actor.gather_party_at_leader(...)` | `0x8009B210` | `GatherPartyAtLeader` | moves all three party slots to the leader, stalls until every valid member arrives, then resets follow history. |
| `FE 28` | 4 | `actor.write_current_actor_flags1(...) -> state` | `0x8008E4EC` | `WriteCurrentActorFlags1` | writes current actor flag word 1 to the indexed script variable and advances three bytes. |
| `FE 29` | 4 | `actor.write_current_actor_flags2(...) -> state` | `0x8008E518` | `WriteCurrentActorFlags2` | writes current actor flag word 2 to the indexed script variable and advances three bytes. |
| `FE 2A` | 4 | `actor.write_current_actor_flags3(...) -> state` | `0x8008E544` | `WriteCurrentActorFlags3` | writes current actor flag word 3 to the indexed script variable and advances three bytes. |
| `FE 2B` | 4 | `actor.write_current_actor_flags4(...) -> state` | `0x8008E570` | `WriteCurrentActorFlags4` | writes current actor flag word 4 to the indexed script variable and advances three bytes. |
| `FE 2C` | 4 | `actor.write_actor_flags1(...) -> state` | `0x8008DEBC` | `WriteActorFlags1` | writes actor flag group 1. |
| `FE 2D` | 4 | `actor.write_actor_flags2(...) -> state` | `0x8008DF44` | `WriteActorFlags2` | writes actor flag group 2. |
| `FE 2E` | 4 | `actor.write_actor_flags3(...) -> state` | `0x8008DFCC` | `WriteActorFlags3` | writes actor flag group 3. |
| `FE 2F` | 4 | `actor.write_actor_flags4(...) -> state` | `0x8008E054` | `WriteActorFlags4` | writes actor flag group 4. |
| `FE 38` | 6 | `actor.write_actor_distance(...) -> state` | `0x8008E1B4` | `WriteActorDistance` | resolves two actor selectors, computes their planar distance from fixed-point X/Z positions, writes zero if either actor is absent, and stores the result in the selected script variable. |
| `FE 39` | 4 | `actor.set_actor_animation_offset_scale(...)` | `0x8008D230` | `SetActorAnimationOffsetScale` | sets the global multiplier used to convert animation displacement samples into planar actor offsets and advances three bytes. |
| `FE 3A` | 4 | `actor.set_party_frame_mask(...)` | `0x8008CED0` | `SetPartyFrameMask` | resolves a character selector and sets that character's bit in the party frame mask. |
| `FE 3B` | 4 | `actor.clear_party_frame_mask(...)` | `0x8008CE64` | `ClearPartyFrameMask` | resolves a character selector and clears that character's bit in the party frame mask. |
| `FE 41` | 4 | `actor.party_member_ride_gear(...)` | `0x8009FC48` | `PartyMemberRideGear` | mounts a party member in Gear. |
| `FE 42` | 4 | `actor.party_member_disembark_gear(...)` | `0x8009FCAC` | `PartyMemberDisembarkGear` | disembarks a party member. |
| `FE 43` | 2 | `actor.disable_party_follow(...)` | `0x8009B15C` | `DisablePartyFollow` | disables party members following the leader. |
| `FE 44` | 2 | `actor.enable_party_follow(...)` | `0x8009B184` | `EnablePartyFollow` | enables party following, clears follow state, and seeds the complete movement history with the leader's current state. |
| `FE 49` | 2 | `actor.clear_current_actor_parent(...)` | `0x8008DAFC` | `ClearCurrentActorParent` | sets the current actor's parent identifier to 0xFF and advances one byte. |
| `FE 5C` | 3 or 5 | `actor.load_current_actor_mecha(...)` | `0x800A0FD8` | `LoadCurrentActorMecha` | waits for I/O, hides or frees the indexed mecha, asynchronously loads replacement files, then constructs and binds the replacement with the actor's scale and position. |
| `FE 5E` | 4 | `actor.set_current_actor_transparency_mode(...)` | `0x8008F2D8` | `SetCurrentActorTransparencyMode` | applies the selected transparency mode to the current actor and advances three bytes. |
| `FE 69` | 6 | `actor.get_party_progress_total(...) -> state` | `0x8008A6E0` | `GetPartyProgressTotal` | writes a selected character's base-plus-remainder progression to a script variable, or zero when no character resolves. |
| `FE 6B` | 6 | `actor.set_party_progress_remainder(...)` | `0x8008A640` | `SetPartyProgressRemainder` | sets a selected character's progression remainder to the nonnegative difference between a requested total and its base progression. |
| `FE 8B` | 4 | `actor.write_current_actor_party_slot(...) -> state` | `0x80089B54` | `WriteCurrentActorPartySlot` | writes the current actor's party slot from the three active mappings or 0xFF when absent, then advances three bytes. |
| `FE 9F` | 5 | `actor.set_party_frame_lock(...)` | `0x800883D4` | `SetPartyFrameLock` | resolves a character and sets or clears that character's party-frame-lock bit according to the mode byte, then advances four bytes. |
| `FE A1` | 6 | `actor.set_character_gear(...)` | `0x80088360` | `SetCharacterGear` | assigns a character Gear. |
| `FE A4` | 2 | `actor.restore_all_gear_fuel_and_ether(...)` | `0x80088198` | `RestoreAllGearFuelAndEther` | restores fuel and ether to their maxima for all twenty Gears and advances one byte. |
| `FE AB` | 5 | `actor.increase_party_gear_hp(...)` | `0x8008DC74` | `IncreasePartyGearHp` | VM Gear-HP increase handler. |
| `FE AC` | 5 | `actor.decrease_party_gear_hp(...)` | `0x8008DD6C` | `DecreasePartyGearHp` | VM Gear-HP decrease handler. |
| `FE AD` | 5 | `actor.write_party_member_hp(...) -> state` | `0x80096B58` | `WritePartyMemberHp` | writes party-member HP. |
| `FE AF` | 19 | `actor.transform_actor_joint_offset(...) -> state` | `0x8008800C` | `TransformActorJointOffset` | resolves an actor joint transform, applies it to a script-supplied vector, writes the transformed XYZ coordinates to three script variables, and advances eighteen bytes. |
| `FE B2` | 5 | `actor.set_party_member_hp(...)` | `0x80096D28` | `SetPartyMemberHp` | sets party-member HP. |
| `FE B3` | 5 | `actor.set_party_member_mp(...)` | `0x80096E20` | `SetPartyMemberMp` | sets party-member MP. |
| `FE B4` | 5 | `actor.write_party_member_mp(...) -> state` | `0x80096C40` | `WritePartyMemberMp` | writes party-member MP. |
| `FE B5` | 2 | `actor.increment_party_convergence_override(...)` | `0x80087FA4` | `IncrementPartyConvergenceOverride` | increments the party-convergence override counter and advances one byte. |
| `FE B6` | 3 | `actor.set_controlled_and_tracked_actor(...)` | `0x80087E98` | `SetControlledAndTrackedActor` | resolves the requested actor, assigns it as both the physically controlled and camera-tracked actor, records whether control differs from the normal party leader, clears control flags from every actor, assigns control to the selected actor, and advances two bytes. |
| `FE C1` | 8 | `actor.query_party_sprite_animation_status(...) -> state` | `0x80088508` | `QueryPartySpriteAnimationStatus` | resolves a party actor, writes its sprite-animation status and actor index to script variables, clears nonterminal status values, adds four VM cycles, and advances seven bytes. |
| `FE C3` | 2 | `actor.set_current_actor_flags02000800(...)` | `0x8009E014` | `SetCurrentActorFlags02000800` | sets flags 0x02000000 and 0x800 on the current actor. |
| `FE C4` | 3 | `actor.set_actor_flags02000800_by_id(...)` | `0x8009DF78` | `SetActorFlags02000800ById` | sets actor flags 0x02000000 and 0x800 by ID. |
| `FE C6` | 4 | `actor.queue_variable_party_character_load(...)` | `0x8008BC80` | `QueueVariablePartyCharacterLoad` | resolves an immediate-or-variable character, reserves a staging slot, and begins loading when no duplicate or conflicting load exists. |
| `FE C7` | 6 | `actor.write_actor_character_gear_id(...) -> state` | `0x800882B8` | `WriteActorCharacterGearId` | resolves an actor selector, writes its character's Gear ID or 0xFF to a script variable, and advances five bytes. |
| `FE CA` | 3 | `actor.release_current_actor_mecha(...)` | `0x800A0EE8` | `ReleaseCurrentActorMecha` | clears the actor's active-mecha flag, then either hides its indexed mecha or frees it and decrements the loaded-mecha count before yielding. |
| `FE D0` | 6 | `actor.clone_gear_and_ability_state(...)` | `0x8008764C` | `CloneGearAndAbilityState` | copies one Gear's persistent state and associated ability blocks to another slot and marks special destination slots present. |
| `FE D4` | 3 or 11 | `actor.manage_sprite_overlay_list(...)` | `0x80086FD0` | `ManageSpriteOverlayList` | mode 0 allocates and initializes a 33-entry sprite-overlay list, mode 1 links an indexed entry at evaluated screen coordinates, mode 2 frees the list, and mode 3 sets an indexed entry's RGB color. |
| `FE DB` | 4 | `actor.restore_character_hp_and_mp(...)` | `0x80097200` | `RestoreCharacterHpAndMp` | restores HP and MP. |
| `FE DC` | 6 | `actor.set_party_sprite_column_offset(...)` | `0x800873C4` | `SetPartySpriteColumnOffset` | writes an indexed byte controlling a party sprite's horizontal render column. |
| `FE DE` | 6 | `actor.or_character_record_flags(...)` | `0x80087148` | `OrCharacterRecordFlags` | applies bitwise OR with a script-supplied mask to the selected character's persistent flag field. |
| `FE E1` | 6 | `actor.copy_gear(...)` | `0x80087580` | `CopyGear` | copies Gear state from a Field script. |

## `movement`

Position, rotation, walkmesh, movement, and collision.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `10` | 2 or 9 | `movement.move_actor_to_position(...)` | `0x80098C00` | `MoveActorToPosition` | initializes unrestricted interpolated actor movement. |
| `11` | 13 | `movement.move_actor_to_position_with_limit(...)` | `0x80098C3C` | `MoveActorToPositionWithLimit` | initializes bounded interpolated actor movement. |
| `17` | 18 | `movement.setup_movement_bounding_zone(...)` | `0x8009E91C` | `SetupMovementBoundingZone` | allocates and loads four movement-boundary vertices. |
| `18` | 5 | `movement.set_actor_collision_dimensions(...)` | `0x8009E83C` | `SetActorCollisionDimensions` | sets actor collision bounds. |
| `19` | 6 | `movement.set_coordinates_and_clear_movement_state(...)` | `0x8009E4BC` | `SetCoordinatesAndClearMovementState` | applies coordinates and clears movement state. |
| `1A` | 2 | `movement.set_walkmesh_at_current_position(...)` | `0x8009E428` | `SetWalkmeshAtCurrentPosition` | changes walkmesh at the actor's current position. |
| `1B` | 7 | `movement.set_walkmesh_and_coordinates(...)` | `0x8009E35C` | `SetWalkmeshAndCoordinates` | assigns walkmesh and evaluated actor coordinates. |
| `1C` | 4 | `movement.apply_single_coordinate(...)` | `0x8009E2C8` | `ApplySingleCoordinate` | applies one evaluated actor coordinate. |
| `1D` | 7 | `movement.apply_three_immediate_coordinates(...)` | `0x8009E248` | `ApplyThreeImmediateCoordinates` | applies three immediate actor coordinates. |
| `21` | 3 | `movement.set_actor_movement_speed(...)` | `0x8009E094` | `SetActorMovementSpeed` | writes the evaluated speed to ActorData+0x76, propagates it to the actor's bound field entity, and advances three bytecode bytes. |
| `2D` | 8 | `movement.get_actor_position(...) -> state` | `0x8009A024` | `GetActorPosition` | gets actor position. |
| `2E` | 3 | `movement.get_actor_direction(...) -> state` | `0x80099FC4` | `GetActorDirection` | gets actor direction. |
| `44` | 5 | `movement.move_actor_along_angle_with_step_limit(...)` | `0x80098184` | `MoveActorAlongAngleWithStepLimit` | continues planar walking along a scripted angle until the step limit expires or the generated target is reached. |
| `45` | 8 | `movement.move_actor_along_angle3d_with_step_limit(...)` | `0x80097864` | `MoveActorAlongAngle3DWithStepLimit` | continues three-axis walking toward a point 32 units along a scripted angle with a vertical offset until the destination or step limit is reached. |
| `46` | 1 | `movement.start_yaw_directed_movement(...)` | `0x80092808` | `StartYawDirectedMovement` | derives current-actor X and Z movement components from the field entity's Y rotation, marks scripted movement active, and advances one byte. |
| `4A` | 6 | `movement.move_actor_to_position_unlimited(...)` | `0x80099980` | `MoveActorToPositionUnlimited` | starts planar movement toward evaluated absolute coordinates with no step limit and waits for arrival. |
| `4B` | 8 | `movement.move_actor_to_position_with_step_limit(...)` | `0x80098430` | `MoveActorToPositionWithStepLimit` | continues planar walking toward scripted absolute coordinates until the destination or step limit is reached. |
| `4C` | 8 | `movement.move_actor_to_position3d(...)` | `0x800979F0` | `MoveActorToPosition3D` | continues three-axis walking toward scripted absolute coordinates until the destination is reached. |
| `4D` | 10 | `movement.move_actor_to_position3d_with_step_limit(...)` | `0x80097954` | `MoveActorToPosition3DWithStepLimit` | continues three-axis walking toward scripted absolute coordinates until the destination or step limit is reached. |
| `4E` | 6 | `movement.move_actor_by_offset(...)` | `0x80098370` | `MoveActorByOffset` | continues planar walking toward coordinates relative to the starting position until the destination is reached. |
| `4F` | 8 | `movement.move_actor_by_offset_with_step_limit(...)` | `0x80098274` | `MoveActorByOffsetWithStepLimit` | continues planar walking toward coordinates relative to the starting position until the destination or step limit is reached. |
| `50` | 8 | `movement.move_actor_by_offset3d(...)` | `0x800977A4` | `MoveActorByOffset3D` | continues three-axis walking toward a position relative to the starting coordinates until the destination is reached. |
| `51` | 10 | `movement.move_actor_by_offset3d_with_step_limit(...)` | `0x800976A8` | `MoveActorByOffset3DWithStepLimit` | continues three-axis walking toward a position relative to the starting coordinates until the destination or scripted step limit is reached. |
| `54` | 5 | `movement.move_toward_actor_unlimited(...)` | `0x800975C0` | `MoveTowardActorUnlimited` | copies a selected actor's position into the movement target, uses an unlimited step count, moves toward the target, and advances five bytes upon completion. |
| `55` | 7 | `movement.move_toward_actor_with_step_limit(...)` | `0x8009749C` | `MoveTowardActorWithStepLimit` | copies a selected actor's position into the movement target, initializes the current slot's step limit, moves toward the target, and advances seven bytes upon completion. |
| `57` | 2 or 11 | `movement.continue_ballistic_actor_move(...)` | `0x80099214` | `ContinueBallisticActorMove` | initializes or continues a timed parabolic move to scripted coordinates, optionally derives elevation from the walkmesh, updates facing and position each frame, and supports walkmesh-only refresh mode. |
| `58` | 4 | `movement.set_current_actor_axis_rotation(...)` | `0x80094918` | `SetCurrentActorAxisRotation` | assigns one current-actor axis. |
| `59` | 1 | `movement.random_turn_with_special_direction(...)` | `0x8009F4CC` | `RandomTurnWithSpecialDirection` | handles signed random turns. |
| `5A` | 1 | `movement.reset_actor_movement_state(...)` | `0x8009524C` | `ResetActorMovementState` | clears current movement state, marks movement inactive, yields, and advances one byte. |
| `5F` | 2 | `movement.set_immediate_actor_cardinal_direction(...)` | `0x8009AD6C` | `SetImmediateActorCardinalDirection` | rotates the current actor to the immediate world-relative cardinal direction. |
| `67` | 4 | `movement.set_actor_direction(...)` | `0x8009ABFC` | `SetActorDirection` | sets actor direction. |
| `69` | 3 | `movement.set_cur_actor_rotation(...)` | `0x8009AC7C` | `SetCurActorRotation` | sets current rotation. |
| `7F` | 3 | `movement.set_walkmesh_material_motion_angle_offset(...)` | `0x80095300` | `SetWalkmeshMaterialMotionAngleOffset` | sets the global angular offset added to the current walkmesh material's motion direction before its XZ vector is accumulated. |
| `80` | 5 | `movement.set_walkmesh_material_byte(...)` | `0x80092664` | `SetWalkmeshMaterialByte` | writes a resolved value to the selected byte lane of a walkmesh material flag and advances five bytes. |
| `81` | 5 | `movement.or_walkmesh_material_byte(...)` | `0x800926C8` | `OrWalkmeshMaterialByte` | applies a bitwise OR with a resolved mask to the selected byte lane of a walkmesh material flag and advances five bytes. |
| `82` | 5 | `movement.write_walkmesh_normal_byte(...) -> state` | `0x80093664` | `WriteWalkmeshNormalByte` | reads a selected byte from a packed walkmesh-normal entry, writes it to the requested script variable, and advances five bytes. |
| `83` | 5 | `movement.and_walkmesh_material_byte(...)` | `0x80092768` | `AndWalkmeshMaterialByte` | applies a bitwise AND with a resolved mask to the selected byte lane of a walkmesh material flag and advances five bytes. |
| `BB` | 2 | `movement.remove_available_party_member(...)` | `0x800965F4` | `RemoveAvailablePartyMember` | clears the requested character's availability bit and advances two bytes. |
| `BD` | 3 | `movement.increase_current_actor_rotation_x(...)` | `0x80094A5C` | `IncreaseCurrentActorRotationX` | increments actor rotation X. |
| `BE` | 3 | `movement.decrease_current_actor_rotation_x(...)` | `0x80094ACC` | `DecreaseCurrentActorRotationX` | decrements actor rotation X. |
| `BF` | 3 | `movement.increase_current_actor_rotation_y(...)` | `0x80094B3C` | `IncreaseCurrentActorRotationY` | increments actor rotation Y. |
| `C0` | 3 | `movement.decrease_current_actor_rotation_y(...)` | `0x80094BAC` | `DecreaseCurrentActorRotationY` | decrements actor rotation Y. |
| `C1` | 3 | `movement.increase_current_actor_rotation_z(...)` | `0x80094C1C` | `IncreaseCurrentActorRotationZ` | increments actor rotation Z. |
| `C2` | 3 | `movement.decrease_current_actor_rotation_z(...)` | `0x80094C8C` | `DecreaseCurrentActorRotationZ` | decrements actor rotation Z. |
| `E1` | 14 | `movement.move_or_clear_vram_rectangle(...)` | `0x80091BBC` | `MoveOrClearVramRectangle` | clears the rectangle defined by the final four operands when the first coordinate pair is zero, otherwise moves the rectangle defined by the first four operands to the final coordinate pair, then advances fourteen bytes. |
| `E8` | 7 | `movement.move_actor_and_set_state_flag(...)` | `0x80094158` | `MoveActorAndSetStateFlag` | moves an actor for a scripted step count and sets its state flag. |
| `E9` | 7 | `movement.move_actor_and_clear_state_flag(...)` | `0x800943AC` | `MoveActorAndClearStateFlag` | applies complementary movement and clears its state flag. |
| `EB` | 20 | `movement.compute_orbit_point_from_coordinates(...) -> state` | `0x800910C0` | `ComputeOrbitPointFromCoordinates` | builds a camera-scale-adjusted orbit point around explicit coordinates from yaw, pitch, and magnitude inputs, writes its X, Z, and Y coordinates, and advances twenty bytes. |
| `F6` | 2 | `movement.configure_actor_rotation_lock_mode(...)` | `0x8008E8C8` | `ConfigureActorRotationLockMode` | clears rotation locking and residual motion for mode zero, locks and snapshots rotation for mode one, enables deferred motion cleanup for mode two, and advances two bytes. |
| `FA` | 5 | `movement.adjust_actor_axis_rotation(...)` | `0x800947B0` | `AdjustActorAxisRotation` | adjusts a selected actor rotation axis. |
| `FE 05` | 7 | `movement.check_actor_walkmesh_id_or_goto(label)` | `0x80095CC4` | `CheckActorWalkmeshId` | advances six bytes when the selected actor uses the requested walkmesh, otherwise branches to the encoded destination. |
| `FE 06` | 7 | `movement.check_actor_walkmesh_material_or_goto(label)` | `0x80095D6C` | `CheckActorWalkmeshMaterial` | advances six bytes when the selected actor's current triangle has the requested material byte, otherwise branches to the encoded destination. |
| `FE 16` | 2 | `movement.free_movement_bounding_zone(...)` | `0x8008C7D8` | `FreeMovementBoundingZone` | releases the current actor's allocated movement-boundary vertices and clears their ownership flag. |
| `FE 19` | 3 | `movement.remove_party_character(...)` | `0x8008C334` | `RemovePartyCharacter` | removes a resolved party member, compacts party resources and slot metadata, and reactivates shifted actors. |
| `FE 1C` | 9 | `movement.set_actor_position3d_immediate(...)` | `0x80098A7C` | `SetActorPosition3DImmediate` | places the current actor at scripted XYZ coordinates and synchronizes its rendered and physical positions. |
| `FE 23` | 21 | `movement.move_party_to_formation(...)` | `0x8009B398` | `MovePartyToFormation` | moves the three party slots toward separate scripted positions and facings until all arrive, while the sentinel mode immediately normalizes their rotations. |
| `FE 46` | 3 | `movement.set_mecha_rotation_authority(...)` | `0x8008AE5C` | `SetMechaRotationAuthority` | selects whether the current actor follows its mecha root rotation or drives that rotation. |
| `FE 68` | 7 | `movement.walk_player_to_position_and_wait(...)` | `0x80092C20` | `WalkPlayerToPositionAndWait` | waits for transition systems to become ready, walks the player toward resolved X and Z coordinates, preserves the player's preexisting control flag, and completes after reaching or failing to reach the destination. |
| `FE 71` | 4 | `movement.write_current_actor_rotation_angle(...) -> state` | `0x8009899C` | `WriteCurrentActorRotationAngle` | writes the current actor rotation modulo one revolution to script memory. |
| `FE 75` | 5 | `movement.write_actor_rotation_angle(...) -> state` | `0x800989F0` | `WriteActorRotationAngle` | writes a selected actor rotation modulo one revolution to script memory when that actor is valid. |
| `FE AE` | 8 | `movement.configure_special_movement_animation(...)` | `0x80096AF4` | `ConfigureSpecialMovementAnimation` | sets the special-movement enable value, animation identifier, and countdown reload, clears the active countdown, and advances seven bytes. |
| `FE B9` | 10 | `movement.write_complete_world_map_position(...) -> state` | `0x80087B5C` | `WriteCompleteWorldMapPosition` | writes all four saved world-map position components to script variables and advances nine bytes. |
| `FE BA` | 11 | `movement.set_world_map_position(...)` | `0x80087C34` | `SetWorldMapPosition` | evaluates and stores all four saved world-map position components and advances ten bytes. |
| `FE D5` | 6 | `movement.write_world_map_position(...) -> state` | `0x80087960` | `WriteWorldMapPosition` | writes both persistent world-map position values to script variables. |
| `FE D7` | 7 | `movement.set_world_map_marker_position_xz(...)` | `0x80087AB8` | `SetWorldMapMarkerPositionXZ` | writes evaluated X and Z coordinates into the saved world-map marker position, clears Y and padding, marks the position valid, and advances six bytes. |
| `FE D9` | 3 | `movement.set_random_turn_direction_table(...)` | `0x80087A7C` | `SetRandomTurnDirectionTable` | stores a script byte selecting the direction table used for random actor turns. |

## `world`

Maps, encounters, triggers, transitions, and scene state.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `0A` | 4 | `if (inside_trigger_2d(...)) call label` | `0x8009533C` | `CallTriggerZone2D` | conditionally calls the encoded target when the physically controlled actor lies inside the indexed trigger's inclusive XZ quadrilateral, otherwise advances four bytes. |
| `12` | 9 | `world.start_custom_field_transition(...)` | `0x80093200` | `StartCustomFieldTransition` | waits for coordinator readiness, stages the destination field and entry parameter, records the transition mode and fade length, and yields for in-place map replacement. |
| `14` | 1 | `world.encounters.enabled = false` | `0x80093C48` | `DisableRandomEncounters` | disables encounters. |
| `15` | 1 | `world.encounters.enabled = true` | `0x80093C6C` | `EnableRandomEncounters` | enables random encounters. |
| `47` | 6 | `world.walk_player_and_change_field(...) -> state` | `0x80092EA0` | `WalkPlayerAndChangeField` | waits for transition systems, enables scripted player control, and walks the player toward the current actor's offset exit point while committing the pending field transition. |
| `56` | 10 | `world.start_world_map_transition(...)` | `0x80093014` | `StartWorldMapTransition` | waits for loading and audio readiness, persists current Field state, disables encounters, stores the return field, initial position, camera yaw, and World Map mode, requests the World Map handoff, and advances ten bytes. |
| `76` | 1 | `world.clear_scene_flag8000(...)` | `0x80093A68` | `ClearSceneFlag8000` | clears scene flag 0x8000. |
| `77` | 1 | `world.set_scene_flag8000(...)` | `0x80093A98` | `SetSceneFlag8000` | sets scene flag 0x8000. |
| `98` | 5 | `world.change_field_when_ready(...) -> state` | `0x800932D0` | `ChangeFieldWhenReady` | waits for coordinator readiness, optionally saves the current field and directions, stages the destination field and entry parameter, marks replacement pending, then yields and advances five bytes. |
| `9D` | 4 | `world.interpolate_scene_scale(...)` | `0x8009A34C` | `InterpolateSceneScale` | starts interpolation from the current scene scale to the scripted scale over the encoded duration. |
| `C9` | 4 | `if (!inside_trigger_2d(...)) goto label` | `0x80095734` | `BranchUnlessInsideTriggerZone2D` | advances four bytes when the physically controlled actor lies inside the indexed trigger's inclusive XZ quadrilateral, otherwise branches to the encoded target. |
| `CB` | 4 | `if (!inside_trigger_3d(...)) goto label` | `0x800958C0` | `BranchUnlessInsideTriggerZone3D` | advances four bytes when the physically controlled actor straddles the indexed trigger's Y plane and lies inside its inclusive XZ quadrilateral, otherwise branches to the encoded target. |
| `CC` | 4 | `if (inside_trigger_3d(...)) call label` | `0x80095520` | `CallTriggerZone3D` | conditionally calls the encoded target when the physically controlled actor straddles the indexed trigger's Y plane and lies inside its inclusive XZ quadrilateral, otherwise advances four bytes. |
| `F7` | 5 | `world.configure_random_encounter_timers(...)` | `0x8008E85C` | `ConfigureRandomEncounterTimers` | evaluates the encounter timer range and active-timer count, caps the count at 32, and regenerates that many unique randomized countdowns. |
| `FE 51` | 2 | `world.enable_compass(...)` | `0x80093BFC` | `EnableCompass` | enables the Field compass. |
| `FE 52` | 2 | `world.disable_compass(...)` | `0x80093C20` | `DisableCompass` | disables the Field compass. |
| `FE 53` | 2 | `world.enable_encounters_field_menu_and_compass(...)` | `0x80093AC8` | `EnableEncountersFieldMenuAndCompass` | enables random encounters, the player-opened Field menu, and the Field compass. |
| `FE 54` | 2 | `world.disable_encounters_field_menu_and_compass(...)` | `0x80093B10` | `DisableEncountersFieldMenuAndCompass` | disables random encounters, the player-opened Field menu, and the Field compass. |
| `FE 6E` | 5 | `world.set_scene_angle_y(...)` | `0x8008FABC` | `SetSceneAngleY` | assigns both scene Y-angle fields. |
| `FE BB` | 4 | `world.write_world_map_vehicle_state(...) -> state` | `0x80087D30` | `WriteWorldMapVehicleState` | writes the saved world-map vehicle state to a script variable and advances three bytes. |
| `FE BC` | 5 | `world.set_world_map_vehicle_state(...)` | `0x80087D80` | `SetWorldMapVehicleState` | evaluates and stores the saved world-map vehicle state and advances four bytes. |

## `camera`

Camera position, projection, tracking, and view geometry.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `60` | 1 | `camera.reset_camera_target_movement(...)` | `0x8008FDD0` | `ResetCameraTargetMovement` | resets camera-target movement. |
| `61` | 8 | `camera.set_camera_target_movement_from(...)` | `0x8008FE2C` | `SetCameraTargetMovementFrom` | sets camera-target origin. |
| `62` | 2 | `camera.set_camera_target_movement_dest_to_actor(...)` | `0x8008FF04` | `SetCameraTargetMovementDestToActor` | targets an actor. |
| `63` | 8 | `camera.set_camera_target_movement_dest(...)` | `0x8008FF90` | `SetCameraTargetMovementDest` | sets camera-target destination. |
| `64` | 1 | `camera.reset_camera_pos_movement(...)` | `0x80090068` | `ResetCameraPosMovement` | resets camera-position movement. |
| `65` | 8 | `camera.set_camera_pos_movement_from(...)` | `0x800900C4` | `SetCameraPosMovementFrom` | sets camera-position origin. |
| `66` | 2 | `camera.set_camera_pos_movement_dest_to_actor(...)` | `0x8009019C` | `SetCameraPosMovementDestToActor` | moves camera to an actor. |
| `68` | 4 | `camera.set_target_actor_camera_relative_direction(...)` | `0x8009AC34` | `SetTargetActorCameraRelativeDirection` | rotates a selected actor to a scripted cardinal direction relative to the camera. |
| `6A` | 3 | `camera.set_camera_relative_actor_direction(...)` | `0x8009ACB4` | `SetCameraRelativeActorDirection` | maps a direction to camera-relative actor rotation. |
| `97` | 3 | `camera.set_automatic_camera_rotation_sector_mask(...)` | `0x8009A634` | `SetAutomaticCameraRotationSectorMask` | sets the eight-sector mask that can trigger automatic camera orbit correction. |
| `99` | 1 | `camera.enter_script_camera_mode(...)` | `0x8008FB98` | `EnterScriptCameraMode` | enters script-controlled camera mode, snapshots yaw, dip, and scaled depth, resets scale, initializes target and eye smoothing to twelve, and locks manual orbit. |
| `9A` | 3 | `camera.leave_or_reacquire_follow_camera(...)` | `0x8008FC4C` | `LeaveOrReacquireFollowCamera` | in script mode, zero duration returns immediately to normal follow and advances six bytes, while nonzero duration starts asynchronous mode-2 reacquisition and advances three; an invocation already in mode 2 leaves the PC unchanged. |
| `9B` | 5 | `camera.set_camera_smoothing_divisors(...)` | `0x8008FD40` | `SetCameraSmoothingDivisors` | sets independent target and eye smoothing divisors, normalizing either zero operand to one. |
| `9E` | 1 | `camera.snapshot_camera_geometry(...)` | `0x8009B9A0` | `SnapshotCameraGeometry` | waits for camera rotation to finish, then saves the current camera direction, projection distance, and depth-cue parameter and advances one byte. |
| `9F` | 1 | `camera.restore_camera_geometry(...)` | `0x8009BA0C` | `RestoreCameraGeometry` | waits for camera rotation to finish, then interpolates camera direction, projection distance, and depth-cue parameter back to their saved values over 32 frames. |
| `A1` | 3 | `camera.set_blocked_camera_sector_mask(...)` | `0x8009A670` | `SetBlockedCameraSectorMask` | sets the eight-sector mask used to reject or redirect manual camera orbit. |
| `A3` | 8 | `camera.set_camera_pos_movement_dest(...)` | `0x80090228` | `SetCameraPosMovementDest` | sets camera-position destination. |
| `A4` | 4 | `camera.interpolate_camera_dip(...)` | `0x8009A490` | `InterpolateCameraDip` | starts interpolation from the current camera dip to the scripted dip over the encoded duration. |
| `A5` | 3 | `camera.write_camera_direction(...) -> state` | `0x8009A534` | `WriteCameraDirection` | writes camera direction. |
| `AA` | 2 | `camera.set_immediate_camera_relative_actor_direction(...)` | `0x8009ACEC` | `SetImmediateCameraRelativeActorDirection` | rotates the current actor to an immediate cardinal direction relative to the camera. |
| `AB` | 1 | `camera.reset_camera_movements(...)` | `0x80090300` | `ResetCameraMovements` | resets Field camera movements. |
| `AC` | 4 | `camera.start_camera_movement(...)` | `0x800903BC` | `StartCameraMovement` | starts Field camera movement. |
| `AD` | 7 | `camera.write_camera_tween_target(...) -> state` | `0x80090B18` | `WriteCameraTweenTarget` | writes tween camera target. |
| `AE` | 7 | `camera.write_camera_tween_position(...) -> state` | `0x80090B9C` | `WriteCameraTweenPosition` | writes tween camera position. |
| `AF` | 4 | `camera.read_or_write_camera_yaw(...) -> state in read mode` | `0x80090C20` | `ReadOrWriteCameraYaw` | writes the captured camera yaw to a variable when the control byte is zero or replaces it from the operand otherwise, then advances four bytes. |
| `B0` | 4 | `camera.read_or_write_camera_dip(...) -> state in read mode` | `0x80090CB8` | `ReadOrWriteCameraDip` | writes the captured projection dip to a variable when the control byte is zero or replaces it from the operand otherwise, then advances four bytes. |
| `B1` | 4 | `camera.read_or_write_camera_depth(...) -> state in read mode` | `0x80090D50` | `ReadOrWriteCameraDepth` | writes the captured scaled projection depth to a variable when the control byte is zero or replaces it with the unsigned operand otherwise, then advances four bytes. |
| `B5` | 5 | `camera.set_camera_direction_timed(...)` | `0x8009B8E4` | `SetCameraDirectionTimed` | snaps the camera direction when interpolation is disabled, otherwise waits for an active rotation or starts a timed direction change, then advances five bytes and yields. |
| `B6` | 5 | `camera.interpolate_projection_depth(...)` | `0x8009B6AC` | `InterpolateProjectionDepth` | applies or interpolates projection depth to the scripted value over the scripted duration. |
| `B7` | 1 | `camera.disable_camera_height_check(...)` | `0x8009ADDC` | `DisableCameraHeightCheck` | disables automatic camera height checking and advances the script. |
| `B8` | 1 | `camera.enable_camera_height_check(...)` | `0x8009AE0C` | `EnableCameraHeightCheck` | enables automatic camera height checking and advances the script. |
| `C7` | 3 | `camera.start_camera_dolly_mode0(...)` | `0x8009B824` | `StartCameraDollyMode0` | starts a positive one-eighth-turn camera dolly rotation over the requested duration when no rotation is active, then advances three bytes and yields. |
| `C8` | 3 | `camera.start_camera_dolly_mode0_alias(...)` | `0x8009B884` | `StartCameraDollyMode0Alias` | starts a positive one-eighth-turn camera dolly rotation over the requested duration when no rotation is active, then advances three bytes and yields. |
| `E6` | 9 | `camera.set_camera_limits(...)` | `0x80091A08` | `SetCameraLimits` | stores the signed camera-limit origin and extents while negating the final extent and advances nine bytes. |
| `EC` | 15 | `camera.compute_camera_orbit_point(...) -> state` | `0x80091318` | `ComputeCameraOrbitPoint` | computes a rotated camera endpoint. |
| `ED` | 8 | `camera.write_camera_movement_parameter(...)` | `0x800915C4` | `WriteCameraMovementParameter` | writes a camera parameter. |
| `EE` | 3 | `camera.set_camera_movement_parameter(...)` | `0x80091720` | `SetCameraMovementParameter` | sets a camera parameter. |
| `F0` | 7 | `camera.write_camera_projection_parameters(...) -> state` | `0x80090DEC` | `WriteCameraProjectionParameters` | writes captured camera yaw, projection dip, and scaled projection depth to three variables and advances seven bytes. |
| `F2` | 9 | `camera.setup_camera_shake(...)` | `0x8008F90C` | `SetupCameraShake` | configures target X, Z, and Y shake offsets and per-frame deltas for a nonzero duration, with an extended return-to-zero phase when every target is zero. |
| `F3` | 7 | `camera.write_camera_orbit_parameters(...) -> state` | `0x80090E70` | `WriteCameraOrbitParameters` | writes yaw, pitch, and half-distance. |
| `FE 22` | 4 | `camera.write_projection_depth(...) -> state` | `0x8009B664` | `WriteProjectionDepth` | writes the current projection depth to script memory. |
| `FE 25` | 3 | `camera.set_camera_follow_height_mode(...)` | `0x8008D5C8` | `SetCameraFollowHeightMode` | loads the camera follow-height mode from the next byte and advances two bytes. |
| `FE 48` | 9 | `camera.set_camera_projection_angles(...)` | `0x8008B518` | `SetCameraProjectionAngles` | sets the three signed camera projection angles. |
| `FE 6D` | 2 | `camera.snapshot_camera_projection_baseline(...)` | `0x8008FB28` | `SnapshotCameraProjectionBaseline` | snapshots the current scaled projection depth, projection dip, and camera yaw as script baselines, resets camera scale to 0x1000, and advances one byte. |
| `FE A8` | 8 | `camera.write_cur_camera_target(...) -> state` | `0x80090A10` | `WriteCurCameraTarget` | writes current camera target. |
| `FE A9` | 8 | `camera.write_cur_camera_position(...) -> state` | `0x80090A94` | `WriteCurCameraPosition` | writes current camera position. |
| `FE AA` | 3 | `camera.set_camera_tracked_actor(...)` | `0x8008DB2C` | `SetCameraTrackedActor` | resolves the encoded actor and stores it as the camera tracking subject, then advances two bytes. |
| `FE B1` | 2 | `camera.initialize_camera_direction_gauge(...)` | `0x80087FD4` | `InitializeCameraDirectionGauge` | loads the direction-gauge texture, allocates double-buffered packets, initializes 109 textured quads, and advances one byte. |

## `dialogue`

Text, portraits, windows, and dialogue choices.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `03` | 4 | `dialogue.open_dialogue_mode2(...)` | `0x8009C104` | `OpenDialogueMode2` | opens the selected dialogue block through the common creation path in fixed-size mode 2 using the trailing control byte. |
| `CF` | 5 | `dialogue.set_dialogue_window_immediate(...)` | `0x8009CE48` | `SetDialogueWindowImmediate` | stores immediate forced X and Y positions, width in three-pixel character units, and height, then advances five bytes. |
| `D0` | 11 | `dialogue.set_dialogue_window_evaluated(...)` | `0x8009CEE0` | `SetDialogueWindowEvaluated` | evaluates and stores forced X and Y positions, width in three-pixel character units, height, and window flags, then advances eleven bytes. |
| `D2` | 4 | `dialogue.open_actor_dialogue_mode0(...)` | `0x8009C0B4` | `OpenActorDialogueMode0` | opens the selected dialogue block in actor-anchored mode 0 using the trailing control byte. |
| `D3` | 4 | `dialogue.open_dialogue_mode1(...)` | `0x8009C0DC` | `OpenDialogueMode1` | opens the selected dialogue block in window mode 1 using the trailing control byte. |
| `D4` | 5 | `dialogue.open_dialogue_at_actor_mode0(...)` | `0x8009C01C` | `OpenDialogueAtActorMode0` | resolves the selected actor as the placement anchor for the current actor's mode-zero dialogue, skips six bytes for an invalid actor, or rewinds one byte to retry deferred creation. |
| `D6` | 3 | `dialogue.set_dialog_animation_speed(...)` | `0x800925A0` | `SetDialogAnimationSpeed` | stores the dialog animation mode and selects eight, six, or four opening frames for modes zero, one, or two before advancing three bytes. |
| `E7` | 7 | `dialogue.set_extended_background_clear_color(...)` | `0x80091A78` | `SetExtendedBackgroundClearColor` | stores the red, green, and blue background clear components and advances seven bytes. |
| `F4` | 2 | `dialogue.close_dialogue_or_reset_window_config(...)` | `0x8009BE9C` | `CloseDialogueOrResetWindowConfig` | control value zero requests closure of the actor's owned dialogue, while a nonzero value clears configured width, height, forced position, and window flags. |
| `F5` | 4 | `dialogue.open_centered_dialogue_mode3(...)` | `0x8009C12C` | `OpenCenteredDialogueMode3` | opens the selected dialogue block in centered mode 3 using the trailing control byte. |
| `FC` | 5 | `dialogue.open_dialogue_at_actor_with_copied_portrait(...)` | `0x8009BF8C` | `OpenDialogueAtActorWithCopiedPortrait` | resolves the selected actor, copies its portrait ID into the current actor, and opens the current actor's mode-zero dialogue anchored to the selected actor; an invalid selector skips six bytes and deferred creation retries. |
| `FE 0D` | 4 | `dialogue.set_portrait(character: value)` | `0x8008CF9C` | `SetDialogPortraitCharacter` | resolves a character selector and stores it as the current actor's dialog portrait identifier. |
| `FE CF` | 6 | `dialogue.open_menu_mode1_with_field_context(...) -> state` | `0x80093888` | `OpenMenuMode1WithFieldContext` | disables field controls, saves field and direction state, installs the requested field and variable 2 value, queues menu mode 1, yields, and advances five bytes. |

## `audio`

Music, sound, channels, volume, and tempo.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `72` | 3 | `audio.play_music_mode_zero(...)` | `0x8008F724` | `PlayMusicModeZero` | waits for music authorization then selects request mode zero and processes the requested music ID. |
| `74` | 3 | `audio.play_sound_effect(...)` | `0x8008F668` | `PlaySoundEffect` | starts or stops the requested sound effect on channel 3 and advances three bytes. |
| `75` | 3 | `audio.play_music_mode_minus_one(...)` | `0x8008F76C` | `PlayMusicModeMinusOne` | waits for music authorization then selects request mode minus one and processes the requested music ID. |
| `FE 0E` | 6 | `audio.fade_music_volume(...)` | `0x8008C84C` | `FadeMusicVolume` | transitions music volume to the requested target over the requested duration and retries when music startup is incomplete. |
| `FE 0F` | 7 | `audio.fade_music_pitch(...)` | `0x8008C938` | `FadeMusicPitch` | transitions music pitch to a signed target over the requested duration and retries when music startup is incomplete. |
| `FE 10` | 6 | `audio.fade_music_tempo(...)` | `0x8008CA60` | `FadeMusicTempo` | transitions music tempo to the requested target over the requested duration and retries when music startup is incomplete. |
| `FE 11` | 7 | `audio.fade_music_pan(...)` | `0x8008CB4C` | `FadeMusicPan` | transitions music pan to a signed target over the requested duration and retries when music startup is incomplete. |
| `FE 12` | 4 | `audio.set_music_channel_mask(...)` | `0x8008CC74` | `SetMusicChannelMask` | applies a channel-enable mask to the active music instance and updates affected voices. |
| `FE 13` | 6 | `audio.configure_actor_positional_sound(...)` | `0x8008CD48` | `ConfigureActorPositionalSound` | assigns the current actor's positional sound identifier and volume in mode 0, stops its previous channel, and marks zero identifiers inactive. |
| `FE 14` | 6 | `audio.configure_actor_positional_sound_mode80(...)` | `0x8008CDD4` | `ConfigureActorPositionalSoundMode80` | assigns the current actor's positional sound identifier and volume with mode 0x80, stops its previous channel, and marks zero identifiers inactive. |
| `FE 5D` | 8 | `audio.play_sound_effect_with_parameters(...)` | `0x8008F6AC` | `PlaySoundEffectWithParameters` | plays the requested sound effect on channel 3 with resolved volume and pan values and advances seven bytes. |
| `FE 62` | 6 | `audio.set_sound_channel_volume(...)` | `0x8008F444` | `SetSoundChannelVolume` | immediately sets the selected sound channel volume and advances five bytes. |
| `FE 63` | 6 | `audio.set_sound_channel_pan(...)` | `0x8008F4A0` | `SetSoundChannelPan` | immediately sets the selected sound channel pan and advances five bytes. |
| `FE 65` | 6 | `audio.play_or_stop_sound_effect_channel(...)` | `0x8008F4FC` | `PlayOrStopSoundEffectChannel` | stops the selected channel when the effect identifier is zero, otherwise starts that effect with default volume and pan, then advances five bytes. |
| `FE 66` | 10 | `audio.play_sound_effect_channel_customized(...)` | `0x8008F558` | `PlaySoundEffectChannelCustomized` | restarts the selected channel with the supplied effect identifier, volume, and pan, then advances nine bytes. |
| `FE 8A` | 4 | `audio.set_spatial_audio_listener_source(...)` | `0x80089F18` | `SetSpatialAudioListenerSource` | selects the controlled actor, camera eye, or camera target as the spatial-audio listener source and advances three bytes. |
| `FE 8C` | 8 | `audio.ramp_sound_channel_volume(...)` | `0x8008F3D0` | `RampSoundChannelVolume` | ramps the selected sound channel toward a supplied volume over a supplied duration and advances seven bytes. |
| `FE 8D` | 4 | `audio.set_sound_channel_preserve_mask(...)` | `0x8008F394` | `SetSoundChannelPreserveMask` | sets the mask that exempts selected sound channels from the next field audio reset and advances three bytes. |
| `FE 98` | 4 | `audio.set_spatial_audio_falloff_distance(...)` | `0x800884CC` | `SetSpatialAudioFalloffDistance` | stores the evaluated maximum distance used for spatial-audio attenuation and advances three bytes. |
| `FE B0` | 3 or 7 | `audio.load_wds_sound_bank_slot(...)` | `0x8008AACC` | `LoadWdsSoundBankSlot` | replaces a selected WDS sound-bank slot through asynchronous loading and finalizes the bank on re-entry. |
| `FE B8` | 5 | `audio.set_transition_music_id(...)` | `0x80087DE0` | `SetTransitionMusicId` | writes an evaluated music ID to the field-transition slot when the selector is zero or the battle-transition slot otherwise, then advances four bytes. |

## `visual`

Fades, lighting, models, particles, effects, and video.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `73` | 2 or 8 | `visual.initialize_particle_system_command(...)` | `0x80086C34` | `InitializeParticleSystemCommand` | skips the disabled subcommand or initializes default particle banks and global particle parameters from three script arguments. |
| `B3` | 3 | `visual.fade_out(...)` | `0x8009731C` | `FadeOut` | starts a script fade-out. |
| `B4` | 3 | `visual.fade_in(...)` | `0x80097364` | `FadeIn` | starts a script fade-in. |
| `DA` | 17 | `visual.create_line_scroll_effect(...)` | `0x800921E8` | `CreateLineScrollEffect` | allocates and fills a line-scroll byte buffer, allocates and initializes its descriptor from six geometry parameters, registers up to 32 effects, and advances seventeen bytes. |
| `FE 1B` | 6 | `visual.adjust_current_model_red_green(...)` | `0x8008B5D4` | `AdjustCurrentModelRedGreen` | adds signed red and green deltas to every current-model vertex and mirrors the colors into the alternate render buffer. |
| `FE 1D` | 9 | `visual.set_global_model_translation_step(...)` | `0x800984EC` | `SetGlobalModelTranslationStep` | sets the global XYZ model translation increments and enables their application. |
| `FE 3F` | 8 | `visual.set_background_clear_color(...)` | `0x8008B0E8` | `SetBackgroundClearColor` | sets the background clear red, green, and blue components. |
| `FE 47` | 4 | `visual.set_default_model_turn_rate(...)` | `0x8008B144` | `SetDefaultModelTurnRate` | sets the default angular step used when rotating field models toward their targets. |
| `FE 5B` | 4 | `visual.set_actor_model_turn_rate(...)` | `0x8008B210` | `SetActorModelTurnRate` | sets the current actor's model-specific angular rotation step. |
| `FE 5F` | 9 | `visual.set_current_actor_dual_lighting_colors(...)` | `0x8008F1C8` | `SetCurrentActorDualLightingColors` | conditionally assigns either or both RGB lighting triplets to the current actor and advances eight bytes. |
| `FE 6F` | 9 | `visual.set_global_model_rotation(...)` | `0x8008B45C` | `SetGlobalModelRotation` | sets the three signed global model-rotation angles used to rebuild the rendering matrix. |
| `FE 70` | 4 | `visual.set_background_model_render_mode(...)` | `0x80089F54` | `SetBackgroundModelRenderMode` | stores the script byte minus 0x80 as the mode controlling camera-offset application and forced rendering for background models, then advances three bytes. |
| `FE 82` | 26 | `visual.set_panorama_color_parameters(...)` | `0x8008A148` | `SetPanoramaColorParameters` | stores three RGB triplets and three additional panorama values, enables panorama rendering, and advances twenty-five bytes. |
| `FE 85` | 4 | `visual.write_video_playback_frame(...) -> state` | `0x8008A2A0` | `WriteVideoPlaybackFrame` | writes the current video playback frame to a script variable and advances three bytes. |
| `FE 86` | 3 | `visual.set_video_transition_mode(...)` | `0x80089F94` | `SetVideoTransitionMode` | stores the raw video-transition mode byte and advances two bytes. |
| `FE 88` | 19 | `visual.configure_proximity_light_gradient(...)` | `0x80089BF0` | `ConfigureProximityLightGradient` | stores indexed near and far RGB triplets plus the interpolation range used for distance-dependent model lighting, then advances eighteen bytes. |
| `FE 89` | 12 | `visual.set_proximity_light_anchor(...)` | `0x80089DCC` | `SetProximityLightAnchor` | stores an indexed XYZ lighting anchor and its resolved actor association, using -1 when resolution fails, then advances eleven bytes. |
| `FE 8F` | 9 | `visual.reset_particle_config_resolved_actor(...)` | `0x80088790` | `ResetParticleConfigResolvedActor` | resolves an actor with zero fallback, stores three particle modifiers, resets its default particle banks, normalizes mode values 1 through 3 to 0x10 through 0x30, adds four VM cycles, and advances eight bytes. |
| `FE 90` | 10 | `visual.initialize_particle_bank(...)` | `0x80089004` | `InitializeParticleBank` | initializes a script particle bank. |
| `FE 91` | 15 | `visual.set_particle_bank_position(...)` | `0x80089174` | `SetParticleBankPosition` | sets particle-bank position. |
| `FE 92` | 15 | `visual.set_particle_bank_physics(...)` | `0x80089374` | `SetParticleBankPhysics` | sets particle-bank physics. |
| `FE 93` | 12 | `visual.set_particle_bank_parameters(...)` | `0x80089574` | `SetParticleBankParameters` | sets particle-bank parameters. |
| `FE 94` | 11 | `visual.set_particle_bank_scale(...)` | `0x800896D4` | `SetParticleBankScale` | sets particle-bank scale. |
| `FE 95` | 15 | `visual.set_particle_bank_color(...)` | `0x80089880` | `SetParticleBankColor` | sets particle-bank color. |
| `FE 96` | 2 | `visual.particles_initialize(...)` | `0x80089A80` | `ParticlesInitialize` | initializes script particles. |
| `FE 97` | 3 | `visual.stop_particle_actor(...)` | `0x80089AE4` | `StopParticleActor` | stops an actor particle effect. |
| `FE 9A` | 10 | `visual.set_target_actor_dual_lighting_colors(...)` | `0x8008F0B4` | `SetTargetActorDualLightingColors` | conditionally assigns either or both RGB lighting triplets to a resolved actor and advances nine bytes. |
| `FE 9B` | 4 | `visual.start_transition_effect_mode1(...)` | `0x8008EF5C` | `StartTransitionEffectMode1` | selects transition effect mode 1 with the supplied effect parameter and advances three bytes. |
| `FE 9C` | 4 | `visual.start_transition_effect_mode2(...)` | `0x8008EFA0` | `StartTransitionEffectMode2` | selects transition effect mode 2 with the supplied effect parameter and advances three bytes. |
| `FE 9D` | 4 | `visual.start_transition_effect_mode3(...)` | `0x8008F070` | `StartTransitionEffectMode3` | selects transition effect mode 3 with the supplied effect parameter and advances three bytes. |
| `FE A5` | 8 | `visual.set_particle_rotation_angle(...)` | `0x80088C1C` | `SetParticleRotationAngle` | stores the active particle slot value, merges evaluated flags into its high byte, stores its rotation angle, adds four VM cycles, and advances seven bytes. |
| `FE BD` | 8 | `visual.set_particle_attachment_mode(...)` | `0x80088B68` | `SetParticleAttachmentMode` | sets particle flag 0x80 for mode 1 or 0x40 for mode 2, adds four VM cycles, and advances seven bytes. |
| `FE BE` | 2 | `visual.enable_video_frame_tile_animation(...)` | `0x80087C0C` | `EnableVideoFrameTileAnimation` | enables animated video-frame tile cycling and advances one byte. |
| `FE C2` | 5 | `visual.reset_particle_config_immediate_actor(...)` | `0x80088674` | `ResetParticleConfigImmediateActor` | selects an immediate actor with zero fallback, stores three particle modifiers, resets its default particle banks, normalizes mode values 1 through 3 to 0x10 through 0x30, adds four VM cycles, and advances nine bytes. |
| `FE C5` | 6 | `visual.set_model_animation(...)` | `0x80086F7C` | `SetModelAnimation` | assigns a model animation. |
| `FE C8` | 19 | `visual.set_particle_directions0_to3(...)` | `0x80088CF8` | `SetParticleDirections0To3` | sets particle directions 0 through 3. |
| `FE C9` | 19 | `visual.set_particle_directions4_to7(...)` | `0x80088D18` | `SetParticleDirections4To7` | sets particle directions 4 through 7. |
| `FE D8` | 3 | `visual.set_sprite_lighting_bypass(...)` | `0x80087A40` | `SetSpriteLightingBypass` | stores a script byte that enables or bypasses dynamic sprite color lighting. |

## `battle`

Battle handoffs, Battling results, and return destinations.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `71` | 3 | `battle.start_battle(...)` | `0x80093568` | `StartBattle` | waits for coordinator readiness, stores the requested battle configuration, marks the battle handoff pending, yields, and advances three bytes. |
| `FE 84` | 10 | `battle.start_battle_with_return_field(...) -> state` | `0x800933F8` | `StartBattleWithReturnField` | waits for coordinator readiness, stores the requested battle and optional post-battle Field destination and variable 2 value, marks the battle handoff pending, yields, and advances nine bytes. |

## `inventory`

Inventory, items, currency, and menus.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `34` | 5 | `inventory.write_inventory_object_quantity(...) -> state` | `0x80096214` | `WriteInventoryObjectQuantity` | writes the encoded object's quantity to the requested script variable, using zero when the object is absent, and advances five bytes. |
| `8B` | 5 | `inventory.check_inventory_object_or_goto(label)` | `0x800962C0` | `CheckInventoryObject` | advances five bytes when the encoded object is present, otherwise branches to the encoded destination. |
| `8C` | 3 | `inventory.add_inventory_object(...)` | `0x8009631C` | `AddInventoryObject` | adds one unit of the encoded object and advances three bytes. |
| `8D` | 3 | `inventory.remove_inventory_object(...)` | `0x8009640C` | `RemoveInventoryObject` | decrements an encoded object's quantity, clears its identifier when the quantity reaches zero, and advances three bytes. |
| `8E` | 7 | `inventory.check_gold_amount_or_goto(label)` | `0x80095F24` | `CheckGoldAmount` | tests the current gold amount. |
| `8F` | 3 | `inventory.increase_gold(...)` | `0x80095FB8` | `IncreaseGold` | increases party gold. |
| `90` | 3 | `inventory.decrease_gold(...)` | `0x8009601C` | `DecreaseGold` | decreases party gold. |
| `FE 4F` | 2 | `inventory.enable_field_menu(...)` | `0x80093BB0` | `EnableFieldMenu` | allows the player to open the normal Field menu. |
| `FE 50` | 2 | `inventory.disable_field_menu(...)` | `0x80093BD4` | `DisableFieldMenu` | prevents the player from opening the normal Field menu. |
| `FE 55` | 2 | `inventory.open_normal_menu(...)` | `0x80093740` | `OpenNormalMenu` | queues menu mode 0 with the configured menu argument, yields, increments the open-menu count, and advances one byte. |
| `FE 56` | 4 | `inventory.open_menu_mode1_with_selection(...) -> state` | `0x80093930` | `OpenMenuMode1WithSelection` | copies the evaluated selection into script variable 1 and persistent menu state, queues menu mode 1, yields, and advances three bytes. |
| `FE 57` | 2 | `inventory.open_load_game_menu(...)` | `0x800937E0` | `OpenLoadGameMenu` | queues menu mode 2, yields, increments the open-menu count, and advances one byte. |
| `FE 58` | 4 | `inventory.open_enter_name_menu(...)` | `0x80093824` | `OpenEnterNameMenu` | queues the Enter Name menu for the selected character or name record and yields the current script. |
| `FE 59` | 4 | `inventory.open_shop_menu(...)` | `0x800939A0` | `OpenShopMenu` | queues the selected shop inventory and yields the current script. |
| `FE 5A` | 4 | `inventory.open_gear_shop_menu(...)` | `0x80093A04` | `OpenGearShopMenu` | queues the selected Gear shop inventory and yields the current script. |
| `FE 99` | 3 | `inventory.set_menu_open_argument(...)` | `0x8008848C` | `SetMenuOpenArgument` | stores the inverse of the supplied one-bit menu argument and advances two bytes. |
| `FE A6` | 6 | `inventory.configure_single_key_sprite_motion(...)` | `0x800888A4` | `ConfigureSingleKeySpriteMotion` | initializes current-actor sprite motion mode 2, stores one nibble selector and one packed 9-bit key value, marks the motion active, and advances five bytes. |
| `FE A7` | 10 | `inventory.configure_dual_key_sprite_motion(...)` | `0x800889BC` | `ConfigureDualKeySpriteMotion` | initializes current-actor sprite motion mode 3, stores two nibble selectors and two packed 9-bit key values, marks the motion active, and advances nine bytes. |
| `FE DA` | 2 | `inventory.open_menu_mode6(...)` | `0x80093790` | `OpenMenuMode6` | queues menu mode 6 with argument 1, yields, increments the open-menu count, and advances one byte. |

## `input`

Button state and accumulated input history.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `31` | 5 | `if ((input.held & mask) == 0) goto label` | `0x800961A0` | `CheckCurrentInputMask` | advances five bytes when the evaluated mask intersects currently held controller input, otherwise branches to the encoded destination. |
| `32` | 5 | `if ((input.accumulated & mask) == 0) goto label` | `0x800961C8` | `CheckAccumulatedInputMask` | advances five bytes when the evaluated mask intersects accumulated controller input, otherwise branches to the encoded destination. |
| `33` | 1 | `input.accumulated = 0` | `0x800961F0` | `ResetAccumulatedInput` | clears the accumulated controller-input mask and advances one byte. |
| `D5` | 3 | `input.set_controller_btn_mask(...)` | `0x80092628` | `SetControllerBtnMask` | sets controller mask. |
| `E2` | 5 | `input.check_current_input_exact_or_goto(label)` | `0x80096150` | `CheckCurrentInputExact` | conditionally branches on exact current input. |
| `E3` | 5 | `input.check_accumulated_input_exact_or_goto(label)` | `0x80096178` | `CheckAccumulatedInputExact` | conditionally branches on exact accumulated input. |
| `FE 6C` | 2 | `input.clear_controller_enable_flag(...)` | `0x8008A5A0` | `ClearControllerEnableFlag` | clears the controller enable byte when the operand is zero, then advances one byte. |

## `state`

Script-variable reads, writes, arithmetic, and bit operations.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `35` | 6 | `state = value` | `0x8009D9A4` | `VariableAssign` | assigns a variable. |
| `36` | 3 | `state = true` | `0x8009D960` | `VariableSetTrue` | sets a variable true. |
| `37` | 3 | `state = false` | `0x8009D91C` | `VariableSetFalse` | clears a variable. |
| `38` | 6 | `state += value` | `0x8009D890` | `VariableAdd` | adds to a variable. |
| `39` | 6 | `state -= value` | `0x8009D804` | `VariableSub` | subtracts from a variable. |
| `3A` | 6 | `state \|= 1 << value` | `0x8009D644` | `VariableSetBit` | evaluates a zero-based bit index, masks it to five bits, and sets that bit in the selected script variable. |
| `3B` | 6 | `state &= ~(1 << ...) value` | `0x8009D408` | `VariableUnsetBit` | evaluates a zero-based bit index, masks it to five bits, and clears that bit in the selected script variable. |
| `3C` | 3 | `state++` | `0x8009D340` | `IncVariable` | increments a variable. |
| `3D` | 3 | `state--` | `0x8009D3A4` | `DecVariable` | decrements a variable. |
| `3E` | 6 | `state &= value` | `0x8009D5B8` | `VariableAND` | applies variable AND. |
| `3F` | 6 | `state \|= value` | `0x8009D52C` | `VariableOR` | applies variable OR. |
| `40` | 6 | `state ^= value` | `0x8009D4A0` | `VariableXOR` | applies variable XOR. |
| `41` | 5 | `state <<= value` | `0x8009D2D0` | `LShiftVariable` | shifts a variable left. |
| `42` | 5 | `state >>= value` | `0x8009D260` | `RShiftVariable` | shifts a variable right. |
| `43` | 3 | `state.rand_variable(...) -> state` | `0x8009D198` | `RandVariable` | writes a random variable. |
| `48` | 7 | `state.write_script_u8_to_variable(...) -> state` | `0x80093CD0` | `WriteScriptU8ToVariable` | writes a script byte to a variable. |
| `49` | 8 | `state.write_script_s16_to_variable(...) -> state` | `0x80093D48` | `WriteScriptS16ToVariable` | writes a script halfword to a variable. |
| `84` | 5 | `state.check_scenario_flags_less_than_or_goto(label)` | `0x80096644` | `CheckScenarioFlagsLessThan` | compares scenario flags. |
| `85` | 5 | `state.check_scenario_flags_greater_than_or_goto(label)` | `0x800966B4` | `CheckScenarioFlagsGreaterThan` | compares scenario flags. |
| `86` | 5 | `state.check_scenario_flags_equal_or_goto(label)` | `0x80096724` | `CheckScenarioFlagsEqual` | compares scenario flags. |
| `87` | 3 | `state.set_scenario_flags(...) -> state` | `0x80096790` | `SetScenarioFlags` | writes scenario flags. |
| `88` | 3 | `state.get_scenario_flags(...) -> state` | `0x800967E8` | `GetScenarioFlags` | reads scenario flags. |
| `A8` | 5 | `state.mul_variable_with_rand(...) -> state` | `0x8009D1F0` | `MulVariableWithRand` | multiplies by random. |
| `DC` | 5 | `state.swap(left, right)` | `0x80092044` | `SwapVariables` | exchanges the values of two script variables and advances five bytes. |
| `DE` | 6 | `state *= value` | `0x8009D6D8` | `VariableMul` | multiplies a variable. |
| `DF` | 6 | `state /= value` | `0x8009D768` | `VariableDiv` | divides a variable. |
| `FE 01` | 2 | `state.random_turn(...)` | `0x8009F424` | `RandomTurn` | periodically selects a random turn. |
| `FE 0A` | 4 | `state \|= (1 << bit)` | `0x8008D684` | `VariableSetIndexedBit` | sets an indexed script-variable bit. |
| `FE 0B` | 4 | `state &= ~(1 << bit)` | `0x8008D700` | `VariableClearIndexedBit` | clears an indexed script-variable bit. |
| `FE 74` | 4 | `state.debug_print_variable_hex_and_decimal(...)` | `0x800985BC` | `DebugPrintVariableHexAndDecimal` | evaluates the encoded operand and prints it in hexadecimal and decimal when Field debug output is enabled. |
| `FE D1` | 2 | `state.set_game_state_flag4000(...)` | `0x8008754C` | `SetGameStateFlag4000` | sets game-state flag 0x4000. |

## `flow`

Jumps, calls, waits, yields, returns, and termination.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `00` | 1 | `stop` | `0x800A1B70` | `Stop` | stops or yields the current Field script. |
| `01` | 3 | `goto label` | `0x800A1E74` | `Jmp` | unconditional VM jump handler. |
| `02` | 8 | `if (!(condition)) goto label` | `0x800A1BD0` | `ConditionalJmp` | conditional VM jump handler. |
| `05` | 3 | `call label` | `0x800A17F4` | `CallRelative3` | pushes IP+3 and calls a relative script routine. |
| `06` | 5 | `call label` | `0x800A1730` | `CallRelative5` | pushes IP+5 and calls a relative script routine. |
| `07` | 3 | `start actor.routine priority async` | `0x8009EB78` | `StartActorScript` | starts a target actor script without waiting. |
| `08` | 3 | `start actor.routine priority wait` | `0x8009ED68` | `StartActorScriptAndWait` | starts and waits for actor script completion. |
| `09` | 3 | `start actor.routine priority wait_extended` | `0x8009F0A0` | `StartActorScriptAndWaitExtended` | extended start/wait state machine. |
| `0D` | 1 | `return` | `0x800A18B8` | `Return` | pops the Field script call stack. |
| `13` | 1 | `nop` | `0x800A2FC0` | `Nop` | Field script no-op handler. |
| `26` | 3 | `flow.sleep(duration)` | `0x8009DD34` | `Sleep` | initializes the current slot's byte timer from the evaluated operand, yields on every dispatch, and advances after N+1 scheduler selections for timer value N. |
| `5B` | 1 | `stall_forever` | `0x80095284` | `ParkActorMovementUpdate` | clears movement vectors and render offsets, marks movement and rotation inactive, and yields without advancing, so repeated selection parks the invocation on this opcode without releasing its slot. |
| `5E` | 1 | `flow.wait_for_animation_completion(...)` | `0x8009A1AC` | `WaitForAnimationCompletion` | stalls until the actor animation completion latch is set, then removes the forced animation and advances. |
| `78` | 4 | `flow.wait_for_archive_file(...)` | `0x800973A4` | `WaitForArchiveFile` | requests or waits for the selected archive file. |
| `9C` | 1 | `flow.wait_for_owned_text_box(...) -> state` | `0x8009BB0C` | `WaitForOwnedTextBox` | waits for the actor's owned text box, conditionally requests its closure, and copies the confirmed choice line to VM variable 0x14 after the slot is released. |
| `A2` | 2 | `flow.wait_for_camera_animation_mask(...)` | `0x8009A58C` | `WaitForCameraAnimationMask` | advances only after every camera animation flag selected by the immediate mask has cleared. |
| `A6` | 3 | `flow.dispatch_triplet_table(index: value)` | `0x80097410` | `SkipScriptTriplets` | advances by three plus three times the evaluated signed count. |
| `B2` | 2 | `flow.yield_until_camera_animation_mask_clears(...)` | `0x8009A5E0` | `YieldUntilCameraAnimationMaskClears` | yields script execution while any camera animation flag selected by the immediate mask remains set. |
| `C3` | 1 | `flow.yield_current_cycle(...)` | `0x800972F4` | `YieldCurrentCycle` | requests a VM yield and advances one byte. |
| `C6` | 1 | `flow.yield32(...)` | `0x800A1E9C` | `Yield32` | adds 32 instructions to the VM budget and yields. |
| `D1` | stall | `stall_forever` | `0x8009CF70` | `ImmediateNoop` | returns immediately without changing script or field state. |
| `E4` | stall | `stall_forever` | `0x80091AD4` | `OpcodeE4NoOp` | returns immediately without changing state or advancing script execution. |
| `EF` | 3 | `flow.wait_for_camera_movement(...)` | `0x8008FA38` | `WaitForCameraMovement` | waits for camera movement. |
| `FB` | 5 | `flow.jump_if_indexed_bit_clear_or_goto(label)` | `0x8008D780` | `JumpIfIndexedBitClear` | conditionally branches on an indexed bit. |
| `FD` | 1 | `nop` | `0x800A2FC0` | `Nop` | Field script no-op handler. |
| `FF` | 1 | `nop` | `0x800A2FC0` | `Nop` | Field script no-op handler. |
| `FE 30` | 6 | `flow.jump_unless_current_actor_flags1_set_or_goto(label)` | `0x8008E3E8` | `JumpUnlessCurrentActorFlags1Set` | branches to the encoded destination when the requested mask does not intersect current actor flag word 1, otherwise advances five bytes. |
| `FE 31` | 6 | `flow.jump_unless_current_actor_flags2_set_or_goto(label)` | `0x8008E414` | `JumpUnlessCurrentActorFlags2Set` | branches to the encoded destination when the requested mask does not intersect current actor flag word 2, otherwise advances five bytes. |
| `FE 32` | 6 | `flow.jump_unless_current_actor_flags3_set_or_goto(label)` | `0x8008E440` | `JumpUnlessCurrentActorFlags3Set` | branches to the encoded destination when the requested mask does not intersect current actor flag word 3, otherwise advances five bytes. |
| `FE 33` | 6 | `flow.jump_unless_current_actor_flags4_set_or_goto(label)` | `0x8008E46C` | `JumpUnlessCurrentActorFlags4Set` | branches to the encoded destination when the requested mask does not intersect current actor flag word 4, otherwise advances five bytes. |
| `FE 34` | 7 | `flow.jump_unless_target_actor_flags1_set_or_goto(label)` | `0x8008E298` | `JumpUnlessTargetActorFlags1Set` | branches to the encoded destination when the requested mask does not intersect target actor flag word 1, otherwise advances six bytes. |
| `FE 35` | 7 | `flow.jump_unless_target_actor_flags2_set_or_goto(label)` | `0x8008E2EC` | `JumpUnlessTargetActorFlags2Set` | branches to the encoded destination when the requested mask does not intersect target actor flag word 2, otherwise advances six bytes. |
| `FE 36` | 7 | `flow.jump_unless_target_actor_flags3_set_or_goto(label)` | `0x8008E340` | `JumpUnlessTargetActorFlags3Set` | branches to the encoded destination when the requested mask does not intersect target actor flag word 3, otherwise advances six bytes. |
| `FE 37` | 7 | `flow.jump_unless_target_actor_flags4_set_or_goto(label)` | `0x8008E394` | `JumpUnlessTargetActorFlags4Set` | branches to the encoded destination when the requested mask does not intersect target actor flag word 4, otherwise advances six bytes. |
| `FE 61` | 2 | `flow.wait_for2d_presentation_ready(...)` | `0x8008E9F8` | `WaitFor2DPresentationReady` | rewinds while presentation readiness is clear, otherwise clears readiness and advances one byte, yielding after every check. |
| `FE 64` | 4 | `flow.wait_for_sound_channel_mask_clear(...)` | `0x8008F5E4` | `WaitForSoundChannelMaskClear` | rewinds while any selected sound channel remains active, otherwise advances three bytes, yielding after every check. |
| `FE 7F` | 2 | `flow.wait_for_video_playback(...)` | `0x8008A244` | `WaitForVideoPlayback` | yields and repeats while video playback remains active, advances when playback finishes, and stops the current VM cycle. |
| `FE 87` | 2 | `flow.wait_for_menu_close(...)` | `0x800936E4` | `WaitForMenuClose` | yields while a menu remains open by rewinding one byte, otherwise advances one byte. |
| `FE A2` | 2 | `flow.wait_for_music_load(...)` | `0x8008825C` | `WaitForMusicLoad` | yields and repeats while music loading is pending, advances when loading finishes, and stops the current VM cycle. |
| `FE CC` | 2 | `flow.wait_for_director_mode_end(...)` | `0x800A0E54` | `WaitForDirectorModeEnd` | advances when director-only mode is inactive, otherwise rewinds one byte and yields so the instruction is retried. |
| `FE D2` | 4 | `flow.skip_three_byte_instruction(...)` | `0x8008752C` | `SkipThreeByteInstruction` | advances past a three-byte no-op instruction. |

## `event`

Operations that do not belong exclusively to another subsystem.

| Opcode | Bytes | DSL form | Handler | Original function | Behavior |
|---|---:|---|---|---|---|
| `04` | 1 | `event.stop_and_redirect_waiters(...)` | `0x800A1A8C` | `StopAndRedirectWaiters` | redirects waiters before stopping. |
| `0E` | 1 | `event.opcode0e_advance(...)` | `0x80092404` | `Opcode0EAdvance` | advances one byte without changing other state. |
| `0F` | 1 | `event.opcode0f_advance(...)` | `0x800923E4` | `Opcode0FAdvance` | advances one byte without changing other state. |
| `2C` | 2 | `event.play_animation(...)` | `0x8009A130` | `PlayAnimation` | starts an actor animation. |
| `5D` | 2 | `event.play_animation_and_clear_completion(...)` | `0x8009A174` | `PlayAnimationAndClearCompletion` | starts the immediate actor animation and clears its completion latch. |
| `6D` | 8 | `event.cos(...) -> state` | `0x8009A6AC` | `Cos` | Field script cosine helper. |
| `6E` | 8 | `event.sin(...) -> state` | `0x8009A768` | `Sin` | Field script sine helper. |
| `79` | 1 | `event.restore_hp(...)` | `0x80097264` | `RestoreHp` | restores HP. |
| `7A` | 1 | `event.restore_mp(...)` | `0x800972AC` | `RestoreMp` | restores MP. |
| `94` | 5 | `event.set_and_pause_event_timer(...) -> state` | `0x800945D4` | `SetAndPauseEventTimer` | packs two evaluated bytes into VM variable 0x0A, pauses the event timer, and resets its update divider. |
| `95` | 2 | `event.configure_event_timer(...)` | `0x80094650` | `ConfigureEventTimer` | configures event-timer pause and count direction from the supplied control byte. |
| `96` | 1 | `event.pause_and_reset_event_timer_divider(...)` | `0x8009468C` | `PauseAndResetEventTimerDivider` | pauses the event timer and resets its update divider. |
| `A0` | 7 | `event.set_screen_geometry(...)` | `0x8009BA7C` | `SetScreenGeometry` | sets depth cue, camera direction, and projection distance from three script operands, updates the geometry screen distance, and advances seven bytes. |
| `A9` | 2 | `event.setup_multichoice(...)` | `0x8009BC98` | `SetupMultichoice` | waits for the current dialogue to accept choices, configures the packed first and last choice indices, initializes cursor state, and advances two bytes. |
| `CA` | 8 | `event.atan2(...) -> state` | `0x8009A824` | `Atan2` | Field script atan2 helper. |
| `CD` | 1 | `event.disable_automatic_contact_script(...)` | `0x8009DA70` | `DisableAutomaticContactScript` | suppresses automatic contact routine 3 while leaving explicit interaction routine 2 available. |
| `CE` | 1 | `event.enable_automatic_contact_script(...)` | `0x8009DA98` | `EnableAutomaticContactScript` | re-enables automatic contact routine 3. |
| `D7` | 3 | `event.set_object_swivel_x_axis(...)` | `0x800946BC` | `SetObjectSwivelXAxis` | selects object swivel around X and stores the evaluated angle. |
| `D8` | 3 | `event.set_object_swivel_y_axis(...)` | `0x80094710` | `SetObjectSwivelYAxis` | selects object swivel around Y and stores the evaluated angle. |
| `D9` | 3 | `event.set_object_swivel_z_axis(...)` | `0x80094764` | `SetObjectSwivelZAxis` | selects object swivel around Z and stores the evaluated angle. |
| `DB` | 5 | `event.set_deformation_strength(...)` | `0x80091F84` | `SetDeformationStrength` | caps a deformation strength at 0xFFF, stores it in the selected deformation slot when the current model supports deformation, and advances five bytes. |
| `E5` | 17 | `event.configure_fog(...)` | `0x80091944` | `ConfigureFog` | stores near and far RGB colors plus near and far fog distances, enables fog, applies the new configuration, and advances seventeen bytes. |
| `EA` | 6 | `event.walk_player_to_aligned_exit(...) -> state` | `0x80092DFC` | `WalkPlayerToAlignedExit` | waits for transition systems, enables scripted player control, and walks the player toward the current actor's aligned exit point while committing any pending field transition. |
| `F1` | 11 | `event.setup_rgb_calculation_mode1(...)` | `0x8008B248` | `SetupRgbCalculationMode1` | configures RGB calculation mode 1 from five script parameters. |
| `FE` | prefix | `event.extended_dispatch(...)` | `0x800869B8` | `ExtendedDispatch` | executes the secondary Field script VM. |
| `FE 00` | fallback | `event.extended_opcode_zero_no_op(...)` | `0x8008D2D8` | `ExtendedOpcodeZeroNoOp` | returns immediately without changing runtime state. |
| `FE 0C` | 14 | `event.set_shared_geometry_parameters(...)` | `0x8008CFEC` | `SetSharedGeometryParameters` | sets six signed shared geometry parameters from script operands. |
| `FE 26` | 16 | `event.setup_screen_distortion(...)` | `0x8008B2F0` | `SetupScreenDistortion` | initializes screen distortion mode 0 from seven script parameters. |
| `FE 27` | 3 or 5 | `event.control_screen_distortion(...)` | `0x8008B328` | `ControlScreenDistortion` | starts distortion fade-out, waits for completion, stops updates, or releases distortion resources according to its subcommand. |
| `FE 3C` | 6 | `event.play_mecha_animation(...)` | `0x8008B180` | `PlayMechaAnimation` | starts the selected animation on a mecha slot and records its current animation identifier. |
| `FE 3D` | 11 | `event.set_primary_mecha_matrix_row(...)` | `0x8008AEC8` | `SetPrimaryMechaMatrixRow` | writes three signed values into a selected row of the primary mecha field matrix. |
| `FE 3E` | 11 | `event.set_secondary_mecha_matrix_row(...)` | `0x8008AFD8` | `SetSecondaryMechaMatrixRow` | writes three signed values into a selected row of the secondary mecha field matrix. |
| `FE 40` | 8 | `event.write_line_scroll_byte(...)` | `0x80092148` | `WriteLineScrollByte` | writes one byte at the requested offset of a line-scroll buffer when the offset is below its configured length and advances seven bytes. |
| `FE 45` | 3 | `event.set_animation_override(...)` | `0x8009A0FC` | `SetAnimationOverride` | replaces the current actor animation override with the immediate animation identifier. |
| `FE 4A` | 4 | `event.begin_special_animation_load(...)` | `0x8008ACE8` | `BeginSpecialAnimationLoad` | replaces the current actor's special-animation allocation and begins loading the selected animation resource. |
| `FE 4B` | 2 | `event.finalize_special_animation_load(...)` | `0x8008A9AC` | `FinalizeSpecialAnimationLoad` | waits for loading to finish, attaches the special animation to the current sprite, and retries while busy. |
| `FE 4C` | 3 | `event.set_forced_animation_and_release_override(...)` | `0x8008A974` | `SetForcedAnimationAndReleaseOverride` | sets the complemented forced-animation identifier and clears actor flag 0x10000. |
| `FE 4D` | 3 | `event.set_forced_animation_complement(...)` | `0x8008A93C` | `SetForcedAnimationComplement` | stores the bitwise complement of an immediate animation identifier as the current actor's forced animation. |
| `FE 4E` | 2 | `event.free_special_animation(...)` | `0x8008AA60` | `FreeSpecialAnimation` | releases the current actor's special-animation allocation and resets its resource identifier. |
| `FE 60` | 10 | `event.start_preset2d_presentation(...)` | `0x8008EC30` | `StartPreset2DPresentation` | starts a 2D presentation using resource, offset, limit, and mode arguments with mode-specific viewport defaults, or rewinds until resources are available. |
| `FE 67` | 20 | `event.start_explicit2d_presentation(...)` | `0x8008EE14` | `StartExplicit2DPresentation` | starts a 2D presentation with explicit resource, offsets, mode, dimensions, and scale values, then advances 19 bytes. |
| `FE 6A` | 4 | `event.set_link_ordering_table_index(...)` | `0x8008A604` | `SetLinkOrderingTableIndex` | sets the ordering-table link index from an immediate or variable operand. |
| `FE 72` | 11 | `event.write_interpolated_angle(...) -> state` | `0x800988B8` | `WriteInterpolatedAngle` | interpolates between two scripted angles by a scripted step and writes the resulting angle to script memory. |
| `FE 73` | 13 | `event.write_distance_between2d_points(...) -> state` | `0x8009861C` | `WriteDistanceBetween2DPoints` | computes the planar distance between two scripted points and writes the result to script memory. |
| `FE 76` | 17 | `event.write_distance_between3d_points(...) -> state` | `0x80098738` | `WriteDistanceBetween3DPoints` | computes the spatial distance between two scripted points and writes the result to script memory. |
| `FE 77` | 3 or 12 | `event.manage_overlay_image_asset(...)` | `0x8008A2E8` | `ManageOverlayImageAsset` | waits for archive I/O, then loads an indexed image into memory, uploads it with optional VRAM backup, or releases it according to the mode, and stops the current VM cycle. |
| `FE 78` | fallback | `event.reserved_opcode78_no_op(...)` | `0x8008A4F0` | `ReservedOpcode78NoOp` | returns immediately without changing state as the reserved opcode 0x78 entry. |
| `FE 79` | fallback | `event.reserved_opcode79_no_op(...)` | `0x8008A4E8` | `ReservedOpcode79NoOp` | returns immediately without changing state as the reserved opcode 0x79 entry. |
| `FE 7A` | fallback | `event.reserved_opcode7a_no_op(...)` | `0x8008A4E0` | `ReservedOpcode7ANoOp` | returns immediately without changing state as the reserved opcode 0x7A entry. |
| `FE 7B` | fallback | `event.reserved_opcode7b_no_op(...)` | `0x8008A518` | `ReservedOpcode7BNoOp` | returns immediately without changing state as the reserved opcode 0x7B entry. |
| `FE 7C` | fallback | `event.reserved_opcode7c_no_op(...)` | `0x8008A500` | `ReservedOpcode7CNoOp` | returns immediately without changing state as the reserved opcode 0x7C entry. |
| `FE 7D` | fallback | `event.reserved_opcode7d_no_op(...)` | `0x8008A508` | `ReservedOpcode7DNoOp` | returns immediately without changing state as the reserved opcode 0x7D entry. |
| `FE 7E` | fallback | `event.reserved_opcode7e_no_op(...)` | `0x8008A510` | `ReservedOpcode7ENoOp` | returns immediately without changing state as the reserved opcode 0x7E entry. |
| `FE 80` | 16 | `event.set_panorama_geometry_parameters(...)` | `0x80089FD0` | `SetPanoramaGeometryParameters` | stores eight panorama geometry and texture parameters, forces a zero third parameter to one, clears the fifth parameter, and advances fifteen bytes. |
| `FE 81` | 9 | `event.set_panorama_orientation_vector(...)` | `0x8008A08C` | `SetPanoramaOrientationVector` | evaluates and stores the panorama orientation vector's X, Z, and Y components and advances eight bytes. |
| `FE 83` | 4 | `event.set_boot_mode_when_ready(...)` | `0x80092FB4` | `SetBootModeWhenReady` | when boot-mode changes are permitted, disables random encounters for the transition, clears the readiness gate, stores the requested boot mode, and advances three bytes. |
| `FE 8E` | 6 | `event.set_screen_bounds_padding(...)` | `0x8008F348` | `SetScreenBoundsPadding` | sets horizontal and vertical padding used by actor screen-bound checks and advances five bytes. |
| `FE 9E` | 10 | `event.set_draw_clip_region(...)` | `0x8008EFE4` | `SetDrawClipRegion` | applies four supplied display clipping dimensions and advances nine bytes. |
| `FE A0` | 13 | `event.start_signed2d_presentation(...)` | `0x8008EA58` | `StartSigned2DPresentation` | starts a signed-parameter 2D presentation with fixed viewport defaults, or rewinds and yields until presentation resources are available. |
| `FE A3` | 3 | `event.toggle_vram_region_backup(...)` | `0x800881E8` | `ToggleVramRegionBackup` | captures the fixed VRAM region when the mode is zero or restores and releases it otherwise, then advances two bytes. |
| `FE B7` | 4 | `event.set_interaction_availability_override(...)` | `0x80087E5C` | `SetInteractionAvailabilityOverride` | stores an evaluated interaction-availability override and advances three bytes. |
| `FE BF` | 14 | `event.setup_battling(...)` | `0x80087848` | `SetupBattling` | waits for field resources and music readiness, then stores six battle parameters and requests battle setup. |
| `FE C0` | 4 | `event.write_battling_match_result_code(...) -> state` | `0x80087800` | `WriteBattlingMatchResultCode` | copies the Battling mode match-result code to a script variable. |
| `FE CB` | 2 | `event.request_director_mode_exit(...)` | `0x800A0EB0` | `RequestDirectorModeExit` | increments the director-mode exit signal, advances one byte, and yields the current script. |
| `FE CD` | 4 | `event.write_current_disc_number(...) -> state` | `0x800A0DFC` | `WriteCurrentDiscNumber` | queries the current disc number, writes it to the selected script variable, and advances three bytes. |
| `FE CE` | 4 | `event.set_max_mecha_count(...)` | `0x800A0DC0` | `SetMaxMechaCount` | sets the maximum mecha overlay entry count from a script operand and advances three bytes. |
| `FE D3` | 18 | `event.scale_two_ratios(...) -> state` | `0x80087420` | `ScaleTwoRatios` | scales two script ratios and writes both results. |
| `FE D6` | 6 | `event.write_game_state184e_and1852(...) -> state` | `0x800879D0` | `WriteGameState184EAnd1852` | writes two game-state fields to script memory. |
| `FE DD` | 3 or 7 | `event.vram_snapshot_command(...)` | `0x800871B0` | `VramSnapshotCommand` | allocates and captures a 256-pixel-wide VRAM region, applies indexed brightness conversion, or releases the working buffers according to subcommand. |
| `FE DF` | 4 | `event.configure_display_mode(...)` | `0x80086E1C` | `ConfigureDisplayMode` | configures display/draw environments. |
| `FE E0` | 3 | `event.set_pause_disabled(...)` | `0x80086DE0` | `SetPauseDisabled` | sets the Field pause-disable state. |
| `FE E2` | 2 | `event.soft_reset(...)` | `0x80086D4C` | `SoftReset` | performs a software reset, requests script suspension, and advances the instruction pointer. |

## Regeneration

```bash
python3 tools/build_dsl_documentation.py
```

When the Field source documentation changes, regenerate the opcode table first:

```bash
python3 tools/build_opcode_table.py
python3 tools/build_dsl_documentation.py
```
