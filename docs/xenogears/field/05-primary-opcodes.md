# Primary Opcodes

The byte at the active PC directly selects one of these 256 entries.

`D1` and `E4` deliberately leave the PC unchanged. `13`, `FD`, and `FF` are aliases of the same advancing NOP. `FE` enters the extended table.

| Op | Bytes | Handler | Name | Behavior |
|---:|---:|---:|---|---|
| `00` | 1 | `0x800A1B70` | `Stop` | stops or yields the current Field script. |
| `01` | 3 | `0x800A1E74` | `Jmp` | unconditional VM jump handler. |
| `02` | 8 | `0x800A1BD0` | `ConditionalJmp` | conditional VM jump handler. |
| `03` | 4 | `0x8009C104` | `OpenDialogueMode2` | opens the selected dialogue block through the common creation path in fixed-size mode 2 using the trailing control byte. |
| `04` | 1 | `0x800A1A8C` | `StopAndRedirectWaiters` | redirects waiters before stopping. |
| `05` | 3 | `0x800A17F4` | `CallRelative3` | pushes IP+3 and calls a relative script routine. |
| `06` | 5 | `0x800A1730` | `CallRelative5` | pushes IP+5 and calls a relative script routine. |
| `07` | 3 | `0x8009EB78` | `StartActorScript` | starts a target actor script without waiting. |
| `08` | 3 | `0x8009ED68` | `StartActorScriptAndWait` | starts and waits for actor script completion. |
| `09` | 3 | `0x8009F0A0` | `StartActorScriptAndWaitExtended` | extended start/wait state machine. |
| `0A` | 4 | `0x8009533C` | `CallTriggerZone2D` | conditionally calls the encoded target when the physically controlled actor lies inside the indexed trigger's inclusive XZ quadrilateral, otherwise advances four bytes. |
| `0B` | 3 | `0x800A1624` | `InitializeNpcActor` | initializes the current NPC from a selected field graphic, synchronizes placement, enables updates and visibility, and advances three bytes. |
| `0C` | 1 | `0x8009F5A8` | `UpdatePlayerCharacterPreserveIP` | runs the player-control update and restores the caller's VM instruction pointer afterward. |
| `0D` | 1 | `0x800A18B8` | `Return` | pops the Field script call stack. |
| `0E` | 1 | `0x80092404` | `Opcode0EAdvance` | advances one byte without changing other state. |
| `0F` | 1 | `0x800923E4` | `Opcode0FAdvance` | advances one byte without changing other state. |
| `10` | 2 or 9 | `0x80098C00` | `MoveActorToPosition` | initializes unrestricted interpolated actor movement. |
| `11` | 13 | `0x80098C3C` | `MoveActorToPositionWithLimit` | initializes bounded interpolated actor movement. |
| `12` | 9 | `0x80093200` | `StartCustomFieldTransition` | waits for coordinator readiness, stages the destination field and entry parameter, records the transition mode and fade length, and yields for in-place map replacement. |
| `13` | 1 | `0x800A2FC0` | `Nop` | Field script no-op handler. |
| `14` | 1 | `0x80093C48` | `DisableRandomEncounters` | disables encounters. |
| `15` | 1 | `0x80093C6C` | `EnableRandomEncounters` | enables random encounters. |
| `16` | 3 | `0x800A08B8` | `InitializePlayableActor` | initializes a playable-character actor, updates player and party mappings, selects party or replacement graphics, applies entry placement, and handles missing-party fallback. |
| `17` | 18 | `0x8009E91C` | `SetupMovementBoundingZone` | allocates and loads four movement-boundary vertices. |
| `18` | 5 | `0x8009E83C` | `SetActorCollisionDimensions` | sets actor collision bounds. |
| `19` | 6 | `0x8009E4BC` | `SetCoordinatesAndClearMovementState` | applies coordinates and clears movement state. |
| `1A` | 2 | `0x8009E428` | `SetWalkmeshAtCurrentPosition` | changes walkmesh at the actor's current position. |
| `1B` | 7 | `0x8009E35C` | `SetWalkmeshAndCoordinates` | assigns walkmesh and evaluated actor coordinates. |
| `1C` | 4 | `0x8009E2C8` | `ApplySingleCoordinate` | applies one evaluated actor coordinate. |
| `1D` | 7 | `0x8009E248` | `ApplyThreeImmediateCoordinates` | applies three immediate actor coordinates. |
| `1E` | 1 | `0x8009E208` | `ResetActorElevationTracking` | clears the actor elevation offset, anchors elevation to the current Y position, and enables elevation updates. |
| `1F` | 2 | `0x8009E1A0` | `SetActorLowFlagsFromPackedByte` | replaces low actor flags from a packed byte. |
| `20` | 3 | `0x8009E10C` | `SetCurrentActorFlags` | decodes a script mask into current-actor flags. |
| `21` | 3 | `0x8009E094` | `SetActorMovementSpeed` | writes the evaluated speed to ActorData+0x76, propagates it to the actor's bound field entity, and advances three bytecode bytes. |
| `22` | 1 | `0x8009DF10` | `ShowActor` | shows the current actor. |
| `23` | 1 | `0x8009E040` | `HideActor` | hides the current actor. |
| `24` | 2 | `0x8009DDEC` | `ShowActorById` | shows an actor by ID. |
| `25` | 2 | `0x8009DE94` | `HideActorById` | hides an actor by ID. |
| `26` | 3 | `0x8009DD34` | `Sleep` | initializes the current slot's byte timer from the evaluated operand, yields on every dispatch, and advances after N+1 scheduler selections for timer value N. |
| `27` | 2 | `0x8009DC4C` | `StopAndDisableActorVM` | stops movement and disables actor VM state. |
| `28` | 2 | `0x8009DBC8` | `EnableActorVM` | enables an actor VM. |
| `29` | 2 | `0x8009DAC4` | `DisableAndHideActorVM` | disables, hides, and closes actor dialogue. |
| `2A` | 1 | `0x8009DA1C` | `DisableDialogActivation` | disables dialog. |
| `2B` | 1 | `0x8009DA44` | `EnableDialogActivation` | enables dialog. |
| `2C` | 2 | `0x8009A130` | `PlayAnimation` | starts an actor animation. |
| `2D` | 8 | `0x8009A024` | `GetActorPosition` | gets actor position. |
| `2E` | 3 | `0x80099FC4` | `GetActorDirection` | gets actor direction. |
| `2F` | 3 | `0x80099EF8` | `WriteCurCharacterID` | writes current character ID. |
| `30` | 3 | `0x80099F48` | `WritePartyLeaderCharacterID` | writes leader ID. |
| `31` | 5 | `0x800961A0` | `CheckCurrentInputMask` | advances five bytes when the evaluated mask intersects currently held controller input, otherwise branches to the encoded destination. |
| `32` | 5 | `0x800961C8` | `CheckAccumulatedInputMask` | advances five bytes when the evaluated mask intersects accumulated controller input, otherwise branches to the encoded destination. |
| `33` | 1 | `0x800961F0` | `ResetAccumulatedInput` | clears the accumulated controller-input mask and advances one byte. |
| `34` | 5 | `0x80096214` | `WriteInventoryObjectQuantity` | writes the encoded object's quantity to the requested script variable, using zero when the object is absent, and advances five bytes. |
| `35` | 6 | `0x8009D9A4` | `VariableAssign` | assigns a variable. |
| `36` | 3 | `0x8009D960` | `VariableSetTrue` | sets a variable true. |
| `37` | 3 | `0x8009D91C` | `VariableSetFalse` | clears a variable. |
| `38` | 6 | `0x8009D890` | `VariableAdd` | adds to a variable. |
| `39` | 6 | `0x8009D804` | `VariableSub` | subtracts from a variable. |
| `3A` | 6 | `0x8009D644` | `VariableSetBit` | evaluates a zero-based bit index, masks it to five bits, and sets that bit in the selected script variable. |
| `3B` | 6 | `0x8009D408` | `VariableUnsetBit` | evaluates a zero-based bit index, masks it to five bits, and clears that bit in the selected script variable. |
| `3C` | 3 | `0x8009D340` | `IncVariable` | increments a variable. |
| `3D` | 3 | `0x8009D3A4` | `DecVariable` | decrements a variable. |
| `3E` | 6 | `0x8009D5B8` | `VariableAND` | applies variable AND. |
| `3F` | 6 | `0x8009D52C` | `VariableOR` | applies variable OR. |
| `40` | 6 | `0x8009D4A0` | `VariableXOR` | applies variable XOR. |
| `41` | 5 | `0x8009D2D0` | `LShiftVariable` | shifts a variable left. |
| `42` | 5 | `0x8009D260` | `RShiftVariable` | shifts a variable right. |
| `43` | 3 | `0x8009D198` | `RandVariable` | writes a random variable. |
| `44` | 5 | `0x80098184` | `MoveActorAlongAngleWithStepLimit` | continues planar walking along a scripted angle until the step limit expires or the generated target is reached. |
| `45` | 8 | `0x80097864` | `MoveActorAlongAngle3DWithStepLimit` | continues three-axis walking toward a point 32 units along a scripted angle with a vertical offset until the destination or step limit is reached. |
| `46` | 1 | `0x80092808` | `StartYawDirectedMovement` | derives current-actor X and Z movement components from the field entity's Y rotation, marks scripted movement active, and advances one byte. |
| `47` | 6 | `0x80092EA0` | `WalkPlayerAndChangeField` | waits for transition systems, enables scripted player control, and walks the player toward the current actor's offset exit point while committing the pending field transition. |
| `48` | 7 | `0x80093CD0` | `WriteScriptU8ToVariable` | writes a script byte to a variable. |
| `49` | 8 | `0x80093D48` | `WriteScriptS16ToVariable` | writes a script halfword to a variable. |
| `4A` | 6 | `0x80099980` | `MoveActorToPositionUnlimited` | starts planar movement toward evaluated absolute coordinates with no step limit and waits for arrival. |
| `4B` | 8 | `0x80098430` | `MoveActorToPositionWithStepLimit` | continues planar walking toward scripted absolute coordinates until the destination or step limit is reached. |
| `4C` | 8 | `0x800979F0` | `MoveActorToPosition3D` | continues three-axis walking toward scripted absolute coordinates until the destination is reached. |
| `4D` | 10 | `0x80097954` | `MoveActorToPosition3DWithStepLimit` | continues three-axis walking toward scripted absolute coordinates until the destination or step limit is reached. |
| `4E` | 6 | `0x80098370` | `MoveActorByOffset` | continues planar walking toward coordinates relative to the starting position until the destination is reached. |
| `4F` | 8 | `0x80098274` | `MoveActorByOffsetWithStepLimit` | continues planar walking toward coordinates relative to the starting position until the destination or step limit is reached. |
| `50` | 8 | `0x800977A4` | `MoveActorByOffset3D` | continues three-axis walking toward a position relative to the starting coordinates until the destination is reached. |
| `51` | 10 | `0x800976A8` | `MoveActorByOffset3DWithStepLimit` | continues three-axis walking toward a position relative to the starting coordinates until the destination or scripted step limit is reached. |
| `52` | 2 | `0x800980FC` | `FollowActor` | continues planar walking toward a selected actor until contact range is reached. |
| `53` | 4 | `0x80098038` | `FollowActorWithStepLimit` | continues planar walking toward a selected actor until contact range or the scripted step limit is reached. |
| `54` | 5 | `0x800975C0` | `MoveTowardActorUnlimited` | copies a selected actor's position into the movement target, uses an unlimited step count, moves toward the target, and advances five bytes upon completion. |
| `55` | 7 | `0x8009749C` | `MoveTowardActorWithStepLimit` | copies a selected actor's position into the movement target, initializes the current slot's step limit, moves toward the target, and advances seven bytes upon completion. |
| `56` | 10 | `0x80093014` | `StartWorldMapTransition` | waits for loading and audio readiness, persists current Field state, disables encounters, stores the return field, initial position, camera yaw, and World Map mode, requests the World Map handoff, and advances ten bytes. |
| `57` | 2 or 11 | `0x80099214` | `ContinueBallisticActorMove` | initializes or continues a timed parabolic move to scripted coordinates, optionally derives elevation from the walkmesh, updates facing and position each frame, and supports walkmesh-only refresh mode. |
| `58` | 4 | `0x80094918` | `SetCurrentActorAxisRotation` | assigns one current-actor axis. |
| `59` | 1 | `0x8009F4CC` | `RandomTurnWithSpecialDirection` | handles signed random turns. |
| `5A` | 1 | `0x8009524C` | `ResetActorMovementState` | clears current movement state, marks movement inactive, yields, and advances one byte. |
| `5B` | 1 | `0x80095284` | `ParkActorMovementUpdate` | clears movement vectors and render offsets, marks movement and rotation inactive, and yields without advancing, so repeated selection parks the invocation on this opcode without releasing its slot. |
| `5C` | 3 | `0x800A0228` | `InitializePartySlotActor` | binds the current actor to a selected party slot, restores its saved map placement when applicable, initializes its sprite, and hides unavailable or off-map members. |
| `5D` | 2 | `0x8009A174` | `PlayAnimationAndClearCompletion` | starts the immediate actor animation and clears its completion latch. |
| `5E` | 1 | `0x8009A1AC` | `WaitForAnimationCompletion` | stalls until the actor animation completion latch is set, then removes the forced animation and advances. |
| `5F` | 2 | `0x8009AD6C` | `SetImmediateActorCardinalDirection` | rotates the current actor to the immediate world-relative cardinal direction. |
| `60` | 1 | `0x8008FDD0` | `ResetCameraTargetMovement` | resets camera-target movement. |
| `61` | 8 | `0x8008FE2C` | `SetCameraTargetMovementFrom` | sets camera-target origin. |
| `62` | 2 | `0x8008FF04` | `SetCameraTargetMovementDestToActor` | targets an actor. |
| `63` | 8 | `0x8008FF90` | `SetCameraTargetMovementDest` | sets camera-target destination. |
| `64` | 1 | `0x80090068` | `ResetCameraPosMovement` | resets camera-position movement. |
| `65` | 8 | `0x800900C4` | `SetCameraPosMovementFrom` | sets camera-position origin. |
| `66` | 2 | `0x8009019C` | `SetCameraPosMovementDestToActor` | moves camera to an actor. |
| `67` | 4 | `0x8009ABFC` | `SetActorDirection` | sets actor direction. |
| `68` | 4 | `0x8009AC34` | `SetTargetActorCameraRelativeDirection` | rotates a selected actor to a scripted cardinal direction relative to the camera. |
| `69` | 3 | `0x8009AC7C` | `SetCurActorRotation` | sets current rotation. |
| `6A` | 3 | `0x8009ACB4` | `SetCameraRelativeActorDirection` | maps a direction to camera-relative actor rotation. |
| `6B` | 3 | `0x8009AB5C` | `RotateActorClockwise` | rotates actor clockwise. |
| `6C` | 3 | `0x8009ABAC` | `RotateActorCounterClockwise` | rotates actor counter-clockwise. |
| `6D` | 8 | `0x8009A6AC` | `Cos` | Field script cosine helper. |
| `6E` | 8 | `0x8009A768` | `Sin` | Field script sine helper. |
| `6F` | 2 | `0x8009A2A8` | `FaceActor` | turns the current actor toward the selected actor when that actor is valid. |
| `70` | 2 | `0x8009A1E4` | `FacePartyMember` | turns the current actor toward the mapped party member when that member is present. |
| `71` | 3 | `0x80093568` | `StartBattle` | waits for coordinator readiness, stores the requested battle configuration, marks the battle handoff pending, yields, and advances three bytes. |
| `72` | 3 | `0x8008F724` | `PlayMusicModeZero` | waits for music authorization then selects request mode zero and processes the requested music ID. |
| `73` | 2 or 8 | `0x80086C34` | `InitializeParticleSystemCommand` | skips the disabled subcommand or initializes default particle banks and global particle parameters from three script arguments. |
| `74` | 3 | `0x8008F668` | `PlaySoundEffect` | starts or stops the requested sound effect on channel 3 and advances three bytes. |
| `75` | 3 | `0x8008F76C` | `PlayMusicModeMinusOne` | waits for music authorization then selects request mode minus one and processes the requested music ID. |
| `76` | 1 | `0x80093A68` | `ClearSceneFlag8000` | clears scene flag 0x8000. |
| `77` | 1 | `0x80093A98` | `SetSceneFlag8000` | sets scene flag 0x8000. |
| `78` | 4 | `0x800973A4` | `WaitForArchiveFile` | requests or waits for the selected archive file. |
| `79` | 1 | `0x80097264` | `RestoreHp` | restores HP. |
| `7A` | 1 | `0x800972AC` | `RestoreMp` | restores MP. |
| `7B` | 4 | `0x800969FC` | `DecreasePartyHp` | VM party-HP decrease handler. |
| `7C` | 4 | `0x80096F18` | `IncreasePartyMp` | increases party MP. |
| `7D` | 4 | `0x80097010` | `DecreasePartyMp` | decreases party MP. |
| `7E` | 4 | `0x80097108` | `IncreasePartyHp` | increases HP for each valid party member selected by the script mask, caps each result at maximum HP, and advances four bytes. |
| `7F` | 3 | `0x80095300` | `SetWalkmeshMaterialMotionAngleOffset` | sets the global angular offset added to the current walkmesh material's motion direction before its XZ vector is accumulated. |
| `80` | 5 | `0x80092664` | `SetWalkmeshMaterialByte` | writes a resolved value to the selected byte lane of a walkmesh material flag and advances five bytes. |
| `81` | 5 | `0x800926C8` | `OrWalkmeshMaterialByte` | applies a bitwise OR with a resolved mask to the selected byte lane of a walkmesh material flag and advances five bytes. |
| `82` | 5 | `0x80093664` | `WriteWalkmeshNormalByte` | reads a selected byte from a packed walkmesh-normal entry, writes it to the requested script variable, and advances five bytes. |
| `83` | 5 | `0x80092768` | `AndWalkmeshMaterialByte` | applies a bitwise AND with a resolved mask to the selected byte lane of a walkmesh material flag and advances five bytes. |
| `84` | 5 | `0x80096644` | `CheckScenarioFlagsLessThan` | compares scenario flags. |
| `85` | 5 | `0x800966B4` | `CheckScenarioFlagsGreaterThan` | compares scenario flags. |
| `86` | 5 | `0x80096724` | `CheckScenarioFlagsEqual` | compares scenario flags. |
| `87` | 3 | `0x80096790` | `SetScenarioFlags` | writes scenario flags. |
| `88` | 3 | `0x800967E8` | `GetScenarioFlags` | reads scenario flags. |
| `89` | 6 | `0x80095E48` | `CheckActorDistance` | tests actor distance. |
| `8A` | 4 | `0x80095C00` | `CheckActorOnScreen` | tests actor screen visibility. |
| `8B` | 5 | `0x800962C0` | `CheckInventoryObject` | advances five bytes when the encoded object is present, otherwise branches to the encoded destination. |
| `8C` | 3 | `0x8009631C` | `AddInventoryObject` | adds one unit of the encoded object and advances three bytes. |
| `8D` | 3 | `0x8009640C` | `RemoveInventoryObject` | decrements an encoded object's quantity, clears its identifier when the quantity reaches zero, and advances three bytes. |
| `8E` | 7 | `0x80095F24` | `CheckGoldAmount` | tests the current gold amount. |
| `8F` | 3 | `0x80095FB8` | `IncreaseGold` | increases party gold. |
| `90` | 3 | `0x8009601C` | `DecreaseGold` | decreases party gold. |
| `91` | 4 | `0x800964B0` | `CheckPartyMember` | tests party membership. |
| `92` | 1 | `0x800A19B0` | `InitializeActorScripts` | resets all actor script slots. |
| `93` | 3 | `0x800A1364` | `AddCurrentActorToMechaList` | initializes the current actor's base graphic, synchronizes its transform, assigns a mecha-list slot and model identifier, and increments the mecha count. |
| `94` | 5 | `0x800945D4` | `SetAndPauseEventTimer` | packs two evaluated bytes into VM variable 0x0A, pauses the event timer, and resets its update divider. |
| `95` | 2 | `0x80094650` | `ConfigureEventTimer` | configures event-timer pause and count direction from the supplied control byte. |
| `96` | 1 | `0x8009468C` | `PauseAndResetEventTimerDivider` | pauses the event timer and resets its update divider. |
| `97` | 3 | `0x8009A634` | `SetAutomaticCameraRotationSectorMask` | sets the eight-sector mask that can trigger automatic camera orbit correction. |
| `98` | 5 | `0x800932D0` | `ChangeFieldWhenReady` | waits for coordinator readiness, optionally saves the current field and directions, stages the destination field and entry parameter, marks replacement pending, then yields and advances five bytes. |
| `99` | 1 | `0x8008FB98` | `EnterScriptCameraMode` | enters script-controlled camera mode, snapshots yaw, dip, and scaled depth, resets scale, initializes target and eye smoothing to twelve, and locks manual orbit. |
| `9A` | 3 | `0x8008FC4C` | `LeaveOrReacquireFollowCamera` | in script mode, zero duration returns immediately to normal follow and advances six bytes, while nonzero duration starts asynchronous mode-2 reacquisition and advances three; an invocation already in mode 2 leaves the PC unchanged. |
| `9B` | 5 | `0x8008FD40` | `SetCameraSmoothingDivisors` | sets independent target and eye smoothing divisors, normalizing either zero operand to one. |
| `9C` | 1 | `0x8009BB0C` | `WaitForOwnedTextBox` | waits for the actor's owned text box, conditionally requests its closure, and copies the confirmed choice line to VM variable 0x14 after the slot is released. |
| `9D` | 4 | `0x8009A34C` | `InterpolateSceneScale` | starts interpolation from the current scene scale to the scripted scale over the encoded duration. |
| `9E` | 1 | `0x8009B9A0` | `SnapshotCameraGeometry` | waits for camera rotation to finish, then saves the current camera direction, projection distance, and depth-cue parameter and advances one byte. |
| `9F` | 1 | `0x8009BA0C` | `RestoreCameraGeometry` | waits for camera rotation to finish, then interpolates camera direction, projection distance, and depth-cue parameter back to their saved values over 32 frames. |
| `A0` | 7 | `0x8009BA7C` | `SetScreenGeometry` | sets depth cue, camera direction, and projection distance from three script operands, updates the geometry screen distance, and advances seven bytes. |
| `A1` | 3 | `0x8009A670` | `SetBlockedCameraSectorMask` | sets the eight-sector mask used to reject or redirect manual camera orbit. |
| `A2` | 2 | `0x8009A58C` | `WaitForCameraAnimationMask` | advances only after every camera animation flag selected by the immediate mask has cleared. |
| `A3` | 8 | `0x80090228` | `SetCameraPosMovementDest` | sets camera-position destination. |
| `A4` | 4 | `0x8009A490` | `InterpolateCameraDip` | starts interpolation from the current camera dip to the scripted dip over the encoded duration. |
| `A5` | 3 | `0x8009A534` | `WriteCameraDirection` | writes camera direction. |
| `A6` | 3 | `0x80097410` | `SkipScriptTriplets` | advances by three plus three times the evaluated signed count. |
| `A7` | 1 | `0x8009F5F4` | `UpdatePlayerCharacter` | processes player movement eligibility, idle detection, directional input, movement triggers, and facing before advancing one byte. |
| `A8` | 5 | `0x8009D1F0` | `MulVariableWithRand` | multiplies by random. |
| `A9` | 2 | `0x8009BC98` | `SetupMultichoice` | waits for the current dialogue to accept choices, configures the packed first and last choice indices, initializes cursor state, and advances two bytes. |
| `AA` | 2 | `0x8009ACEC` | `SetImmediateCameraRelativeActorDirection` | rotates the current actor to an immediate cardinal direction relative to the camera. |
| `AB` | 1 | `0x80090300` | `ResetCameraMovements` | resets Field camera movements. |
| `AC` | 4 | `0x800903BC` | `StartCameraMovement` | starts Field camera movement. |
| `AD` | 7 | `0x80090B18` | `WriteCameraTweenTarget` | writes tween camera target. |
| `AE` | 7 | `0x80090B9C` | `WriteCameraTweenPosition` | writes tween camera position. |
| `AF` | 4 | `0x80090C20` | `ReadOrWriteCameraYaw` | writes the captured camera yaw to a variable when the control byte is zero or replaces it from the operand otherwise, then advances four bytes. |
| `B0` | 4 | `0x80090CB8` | `ReadOrWriteCameraDip` | writes the captured projection dip to a variable when the control byte is zero or replaces it from the operand otherwise, then advances four bytes. |
| `B1` | 4 | `0x80090D50` | `ReadOrWriteCameraDepth` | writes the captured scaled projection depth to a variable when the control byte is zero or replaces it with the unsigned operand otherwise, then advances four bytes. |
| `B2` | 2 | `0x8009A5E0` | `YieldUntilCameraAnimationMaskClears` | yields script execution while any camera animation flag selected by the immediate mask remains set. |
| `B3` | 3 | `0x8009731C` | `FadeOut` | starts a script fade-out. |
| `B4` | 3 | `0x80097364` | `FadeIn` | starts a script fade-in. |
| `B5` | 5 | `0x8009B8E4` | `SetCameraDirectionTimed` | snaps the camera direction when interpolation is disabled, otherwise waits for an active rotation or starts a timed direction change, then advances five bytes and yields. |
| `B6` | 5 | `0x8009B6AC` | `InterpolateProjectionDepth` | applies or interpolates projection depth to the scripted value over the scripted duration. |
| `B7` | 1 | `0x8009ADDC` | `DisableCameraHeightCheck` | disables automatic camera height checking and advances the script. |
| `B8` | 1 | `0x8009AE0C` | `EnableCameraHeightCheck` | enables automatic camera height checking and advances the script. |
| `B9` | 4 | `0x80096534` | `CheckAvailablePartyMember` | advances four bytes when the requested character is available, otherwise branches to the encoded destination. |
| `BA` | 2 | `0x800965A8` | `AddAvailablePartyMember` | sets the requested character's availability bit and advances two bytes. |
| `BB` | 2 | `0x800965F4` | `RemoveAvailablePartyMember` | clears the requested character's availability bit and advances two bytes. |
| `BC` | 1 | `0x800A0D3C` | `InitializeActorSprite` | creates and initializes actor sprite state. |
| `BD` | 3 | `0x80094A5C` | `IncreaseCurrentActorRotationX` | increments actor rotation X. |
| `BE` | 3 | `0x80094ACC` | `DecreaseCurrentActorRotationX` | decrements actor rotation X. |
| `BF` | 3 | `0x80094B3C` | `IncreaseCurrentActorRotationY` | increments actor rotation Y. |
| `C0` | 3 | `0x80094BAC` | `DecreaseCurrentActorRotationY` | decrements actor rotation Y. |
| `C1` | 3 | `0x80094C1C` | `IncreaseCurrentActorRotationZ` | increments actor rotation Z. |
| `C2` | 3 | `0x80094C8C` | `DecreaseCurrentActorRotationZ` | decrements actor rotation Z. |
| `C3` | 1 | `0x800972F4` | `YieldCurrentCycle` | requests a VM yield and advances one byte. |
| `C4` | 2 | `0x80093E30` | `RotateActorAndSetStateFlag` | rotates an actor over 30 steps and sets its state flag. |
| `C5` | 2 | `0x80093FC0` | `RotateActorAndClearStateFlag` | applies the inverse rotation and clears its state flag. |
| `C6` | 1 | `0x800A1E9C` | `Yield32` | adds 32 instructions to the VM budget and yields. |
| `C7` | 3 | `0x8009B824` | `StartCameraDollyMode0` | starts a positive one-eighth-turn camera dolly rotation over the requested duration when no rotation is active, then advances three bytes and yields. |
| `C8` | 3 | `0x8009B884` | `StartCameraDollyMode0Alias` | starts a positive one-eighth-turn camera dolly rotation over the requested duration when no rotation is active, then advances three bytes and yields. |
| `C9` | 4 | `0x80095734` | `BranchUnlessInsideTriggerZone2D` | advances four bytes when the physically controlled actor lies inside the indexed trigger's inclusive XZ quadrilateral, otherwise branches to the encoded target. |
| `CA` | 8 | `0x8009A824` | `Atan2` | Field script atan2 helper. |
| `CB` | 4 | `0x800958C0` | `BranchUnlessInsideTriggerZone3D` | advances four bytes when the physically controlled actor straddles the indexed trigger's Y plane and lies inside its inclusive XZ quadrilateral, otherwise branches to the encoded target. |
| `CC` | 4 | `0x80095520` | `CallTriggerZone3D` | conditionally calls the encoded target when the physically controlled actor straddles the indexed trigger's Y plane and lies inside its inclusive XZ quadrilateral, otherwise advances four bytes. |
| `CD` | 1 | `0x8009DA70` | `DisableAutomaticContactScript` | suppresses automatic contact routine 3 while leaving explicit interaction routine 2 available. |
| `CE` | 1 | `0x8009DA98` | `EnableAutomaticContactScript` | re-enables automatic contact routine 3. |
| `CF` | 5 | `0x8009CE48` | `SetDialogueWindowImmediate` | stores immediate forced X and Y positions, width in three-pixel character units, and height, then advances five bytes. |
| `D0` | 11 | `0x8009CEE0` | `SetDialogueWindowEvaluated` | evaluates and stores forced X and Y positions, width in three-pixel character units, height, and window flags, then advances eleven bytes. |
| `D1` | stall | `0x8009CF70` | `ImmediateNoop` | returns immediately without changing script or field state. |
| `D2` | 4 | `0x8009C0B4` | `OpenActorDialogueMode0` | opens the selected dialogue block in actor-anchored mode 0 using the trailing control byte. |
| `D3` | 4 | `0x8009C0DC` | `OpenDialogueMode1` | opens the selected dialogue block in window mode 1 using the trailing control byte. |
| `D4` | 5 | `0x8009C01C` | `OpenDialogueAtActorMode0` | resolves the selected actor as the placement anchor for the current actor's mode-zero dialogue, skips six bytes for an invalid actor, or rewinds one byte to retry deferred creation. |
| `D5` | 3 | `0x80092628` | `SetControllerBtnMask` | sets controller mask. |
| `D6` | 3 | `0x800925A0` | `SetDialogAnimationSpeed` | stores the dialog animation mode and selects eight, six, or four opening frames for modes zero, one, or two before advancing three bytes. |
| `D7` | 3 | `0x800946BC` | `SetObjectSwivelXAxis` | selects object swivel around X and stores the evaluated angle. |
| `D8` | 3 | `0x80094710` | `SetObjectSwivelYAxis` | selects object swivel around Y and stores the evaluated angle. |
| `D9` | 3 | `0x80094764` | `SetObjectSwivelZAxis` | selects object swivel around Z and stores the evaluated angle. |
| `DA` | 17 | `0x800921E8` | `CreateLineScrollEffect` | allocates and fills a line-scroll byte buffer, allocates and initializes its descriptor from six geometry parameters, registers up to 32 effects, and advances seventeen bytes. |
| `DB` | 5 | `0x80091F84` | `SetDeformationStrength` | caps a deformation strength at 0xFFF, stores it in the selected deformation slot when the current model supports deformation, and advances five bytes. |
| `DC` | 5 | `0x80092044` | `SwapVariables` | exchanges the values of two script variables and advances five bytes. |
| `DD` | 6 | `0x80091E00` | `SetCurrentActorBlendParameters` | replaces rendering blend bits 5 through 6 from the low two operand bits, stores the accompanying rendering value, and advances six bytes. |
| `DE` | 6 | `0x8009D6D8` | `VariableMul` | multiplies a variable. |
| `DF` | 6 | `0x8009D768` | `VariableDiv` | divides a variable. |
| `E0` | 7 | `0x80091E98` | `SetSelectedActorBlendParameters` | resolves an actor and, when valid, replaces its rendering blend bits and accompanying rendering value before advancing seven bytes. |
| `E1` | 14 | `0x80091BBC` | `MoveOrClearVramRectangle` | clears the rectangle defined by the final four operands when the first coordinate pair is zero, otherwise moves the rectangle defined by the first four operands to the final coordinate pair, then advances fourteen bytes. |
| `E2` | 5 | `0x80096150` | `CheckCurrentInputExact` | conditionally branches on exact current input. |
| `E3` | 5 | `0x80096178` | `CheckAccumulatedInputExact` | conditionally branches on exact accumulated input. |
| `E4` | stall | `0x80091AD4` | `OpcodeE4NoOp` | returns immediately without changing state or advancing script execution. |
| `E5` | 17 | `0x80091944` | `ConfigureFog` | stores near and far RGB colors plus near and far fog distances, enables fog, applies the new configuration, and advances seventeen bytes. |
| `E6` | 9 | `0x80091A08` | `SetCameraLimits` | stores the signed camera-limit origin and extents while negating the final extent and advances nine bytes. |
| `E7` | 7 | `0x80091A78` | `SetExtendedBackgroundClearColor` | stores the red, green, and blue background clear components and advances seven bytes. |
| `E8` | 7 | `0x80094158` | `MoveActorAndSetStateFlag` | moves an actor for a scripted step count and sets its state flag. |
| `E9` | 7 | `0x800943AC` | `MoveActorAndClearStateFlag` | applies complementary movement and clears its state flag. |
| `EA` | 6 | `0x80092DFC` | `WalkPlayerToAlignedExit` | waits for transition systems, enables scripted player control, and walks the player toward the current actor's aligned exit point while committing any pending field transition. |
| `EB` | 20 | `0x800910C0` | `ComputeOrbitPointFromCoordinates` | builds a camera-scale-adjusted orbit point around explicit coordinates from yaw, pitch, and magnitude inputs, writes its X, Z, and Y coordinates, and advances twenty bytes. |
| `EC` | 15 | `0x80091318` | `ComputeCameraOrbitPoint` | computes a rotated camera endpoint. |
| `ED` | 8 | `0x800915C4` | `WriteCameraMovementParameter` | writes a camera parameter. |
| `EE` | 3 | `0x80091720` | `SetCameraMovementParameter` | sets a camera parameter. |
| `EF` | 3 | `0x8008FA38` | `WaitForCameraMovement` | waits for camera movement. |
| `F0` | 7 | `0x80090DEC` | `WriteCameraProjectionParameters` | writes captured camera yaw, projection dip, and scaled projection depth to three variables and advances seven bytes. |
| `F1` | 11 | `0x8008B248` | `SetupRgbCalculationMode1` | configures RGB calculation mode 1 from five script parameters. |
| `F2` | 9 | `0x8008F90C` | `SetupCameraShake` | configures target X, Z, and Y shake offsets and per-frame deltas for a nonzero duration, with an extended return-to-zero phase when every target is zero. |
| `F3` | 7 | `0x80090E70` | `WriteCameraOrbitParameters` | writes yaw, pitch, and half-distance. |
| `F4` | 2 | `0x8009BE9C` | `CloseDialogueOrResetWindowConfig` | control value zero requests closure of the actor's owned dialogue, while a nonzero value clears configured width, height, forced position, and window flags. |
| `F5` | 4 | `0x8009C12C` | `OpenCenteredDialogueMode3` | opens the selected dialogue block in centered mode 3 using the trailing control byte. |
| `F6` | 2 | `0x8008E8C8` | `ConfigureActorRotationLockMode` | clears rotation locking and residual motion for mode zero, locks and snapshots rotation for mode one, enables deferred motion cleanup for mode two, and advances two bytes. |
| `F7` | 5 | `0x8008E85C` | `ConfigureRandomEncounterTimers` | evaluates the encounter timer range and active-timer count, caps the count at 32, and regenerates that many unique randomized countdowns. |
| `F8` | 4 | `0x8008E59C` | `UpdateCurrentActorFlagQuarter` | sets or clears the selected lower or upper 16-bit quarter of either current actor flag group and advances four bytes. |
| `F9` | 2 | `0x8008DE64` | `SetParentActor` | assigns an actor parent. |
| `FA` | 5 | `0x800947B0` | `AdjustActorAxisRotation` | adjusts a selected actor rotation axis. |
| `FB` | 5 | `0x8008D780` | `JumpIfIndexedBitClear` | conditionally branches on an indexed bit. |
| `FC` | 5 | `0x8009BF8C` | `OpenDialogueAtActorWithCopiedPortrait` | resolves the selected actor, copies its portrait ID into the current actor, and opens the current actor's mode-zero dialogue anchored to the selected actor; an invalid selector skips six bytes and deferred creation retries. |
| `FD` | 1 | `0x800A2FC0` | `Nop` | Field script no-op handler. |
| `FE` | prefix | `0x800869B8` | `ExtendedDispatch` | executes the secondary Field script VM. |
| `FF` | 1 | `0x800A2FC0` | `Nop` | Field script no-op handler. |

Mode-dependent lengths describe one physical dispatch. Waiting handlers may preserve or rewind the PC until completion; the table reports the completed encoding, not a temporary update-time PC delta.
