# Extended FE Opcodes

Each instruction is encoded as `FE xx`; byte counts include the `FE` prefix. Only `FE 00..FE E2` have dispatch entries.

`FE 00` and `FE 78..FE 7E` consume only the prefix and reinterpret the second byte as a primary opcode. `FE E3..FE FF` are outside the table.

| Op | Bytes | Handler | Name | Behavior |
|---:|---:|---:|---|---|
| `FE 00` | fallback | `0x8008D2D8` | `ExtendedOpcodeZeroNoOp` | returns immediately without changing runtime state. |
| `FE 01` | 2 | `0x8009F424` | `RandomTurn` | periodically selects a random turn. |
| `FE 02` | 5 | `0x80095B3C` | `FieldActorInnerProximityPredicate` | checks the near-screen actor range. |
| `FE 03` | 4 | `0x8008D0F4` | `SetCurrentActorUniformScale` | sets uniform actor scale, applies a three-quarter sprite scale, refreshes rotation, and advances three bytes. |
| `FE 04` | 4 | `0x8008D26C` | `SetCurrentActorSpriteGeometryScale` | doubles the supplied value into the current actor's sprite geometry scale and advances three bytes. |
| `FE 05` | 7 | `0x80095CC4` | `CheckActorWalkmeshId` | advances six bytes when the selected actor uses the requested walkmesh, otherwise branches to the encoded destination. |
| `FE 06` | 7 | `0x80095D6C` | `CheckActorWalkmeshMaterial` | advances six bytes when the selected actor's current triangle has the requested material byte, otherwise branches to the encoded destination. |
| `FE 07` | 3 | `0x8008D604` | `SetCurrentActorFlag400FromMode` | clears actor flag 0x400 for mode zero, sets it for mode one, and advances two bytes. |
| `FE 08` | 8 | `0x8008D180` | `SetCurrentActorAxisScales` | sets independent actor X, Y, and Z scales, restores the sprite scale to 0xC00, refreshes rotation, and advances seven bytes. |
| `FE 09` | 4 | `0x8008D078` | `ToggleActorMechaSuppression` | clears actor flag 0x800 for a zero operand and sets it otherwise, controlling mecha and shadow suppression. |
| `FE 0A` | 4 | `0x8008D684` | `VariableSetIndexedBit` | sets an indexed script-variable bit. |
| `FE 0B` | 4 | `0x8008D700` | `VariableClearIndexedBit` | clears an indexed script-variable bit. |
| `FE 0C` | 14 | `0x8008CFEC` | `SetSharedGeometryParameters` | sets six signed shared geometry parameters from script operands. |
| `FE 0D` | 4 | `0x8008CF9C` | `SetDialogPortraitCharacter` | resolves a character selector and stores it as the current actor's dialog portrait identifier. |
| `FE 0E` | 6 | `0x8008C84C` | `FadeMusicVolume` | transitions music volume to the requested target over the requested duration and retries when music startup is incomplete. |
| `FE 0F` | 7 | `0x8008C938` | `FadeMusicPitch` | transitions music pitch to a signed target over the requested duration and retries when music startup is incomplete. |
| `FE 10` | 6 | `0x8008CA60` | `FadeMusicTempo` | transitions music tempo to the requested target over the requested duration and retries when music startup is incomplete. |
| `FE 11` | 7 | `0x8008CB4C` | `FadeMusicPan` | transitions music pan to a signed target over the requested duration and retries when music startup is incomplete. |
| `FE 12` | 4 | `0x8008CC74` | `SetMusicChannelMask` | applies a channel-enable mask to the active music instance and updates affected voices. |
| `FE 13` | 6 | `0x8008CD48` | `ConfigureActorPositionalSound` | assigns the current actor's positional sound identifier and volume in mode 0, stops its previous channel, and marks zero identifiers inactive. |
| `FE 14` | 6 | `0x8008CDD4` | `ConfigureActorPositionalSoundMode80` | assigns the current actor's positional sound identifier and volume with mode 0x80, stops its previous channel, and marks zero identifiers inactive. |
| `FE 15` | 6 | `0x800A14F0` | `InitializeActorGraphicVariant` | initializes the current actor from a selected field graphic and variant, synchronizes placement, enables updates and visibility, and advances five bytes. |
| `FE 16` | 2 | `0x8008C7D8` | `FreeMovementBoundingZone` | releases the current actor's allocated movement-boundary vertices and clears their ownership flag. |
| `FE 17` | 4 | `0x8009AA00` | `FaceActorTowardActor` | turns the first selected actor toward the second selected actor when both are valid. |
| `FE 18` | 5 | `0x8008BDD8` | `AddImmediatePartyCharacter` | reserves a staging slot and begins loading an immediate party character, or marks an already staged character as present. |
| `FE 19` | 3 | `0x8008C334` | `RemovePartyCharacter` | removes a resolved party member, compacts party resources and slot metadata, and reactivates shifted actors. |
| `FE 1A` | 2 | `0x8008B894` | `FinalizePartyCharacterLoad` | waits for staged loading, decompresses the character resource, releases compressed data, and initializes the first entry actor targeting that character. |
| `FE 1B` | 6 | `0x8008B5D4` | `AdjustCurrentModelRedGreen` | adds signed red and green deltas to every current-model vertex and mirrors the colors into the alternate render buffer. |
| `FE 1C` | 9 | `0x80098A7C` | `SetActorPosition3DImmediate` | places the current actor at scripted XYZ coordinates and synchronizes its rendered and physical positions. |
| `FE 1D` | 9 | `0x800984EC` | `SetGlobalModelTranslationStep` | sets the global XYZ model translation increments and enables their application. |
| `FE 1E` | 3 | `0x8009FB98` | `SwitchMapToGears` | synchronizes assets and marks the Field map for Gear mode. |
| `FE 1F` | 2 | `0x8009FDD4` | `MountCurrentPartyActor` | maps the current actor to a party slot, mounts it when not already riding a gear, and advances one byte. |
| `FE 20` | 3 | `0x8009FE4C` | `DismountPartyCharacter` | resolves the selected character to a party slot, dismounts it when currently riding a gear, and advances two bytes. |
| `FE 21` | 4 | `0x800A06E8` | `InitializePartyCharacterActor` | resolves a character into the active party, binds its party graphics and synchronized transform, or creates a disabled placeholder when absent. |
| `FE 22` | 4 | `0x8009B664` | `WriteProjectionDepth` | writes the current projection depth to script memory. |
| `FE 23` | 21 | `0x8009B398` | `MovePartyToFormation` | moves the three party slots toward separate scripted positions and facings until all arrive, while the sentinel mode immediately normalizes their rotations. |
| `FE 24` | 2 | `0x8009B210` | `GatherPartyAtLeader` | moves all three party slots to the leader, stalls until every valid member arrives, then resets follow history. |
| `FE 25` | 3 | `0x8008D5C8` | `SetCameraFollowHeightMode` | loads the camera follow-height mode from the next byte and advances two bytes. |
| `FE 26` | 16 | `0x8008B2F0` | `SetupScreenDistortion` | initializes screen distortion mode 0 from seven script parameters. |
| `FE 27` | 3 or 5 | `0x8008B328` | `ControlScreenDistortion` | starts distortion fade-out, waits for completion, stops updates, or releases distortion resources according to its subcommand. |
| `FE 28` | 4 | `0x8008E4EC` | `WriteCurrentActorFlags1` | writes current actor flag word 1 to the indexed script variable and advances three bytes. |
| `FE 29` | 4 | `0x8008E518` | `WriteCurrentActorFlags2` | writes current actor flag word 2 to the indexed script variable and advances three bytes. |
| `FE 2A` | 4 | `0x8008E544` | `WriteCurrentActorFlags3` | writes current actor flag word 3 to the indexed script variable and advances three bytes. |
| `FE 2B` | 4 | `0x8008E570` | `WriteCurrentActorFlags4` | writes current actor flag word 4 to the indexed script variable and advances three bytes. |
| `FE 2C` | 4 | `0x8008DEBC` | `WriteActorFlags1` | writes actor flag group 1. |
| `FE 2D` | 4 | `0x8008DF44` | `WriteActorFlags2` | writes actor flag group 2. |
| `FE 2E` | 4 | `0x8008DFCC` | `WriteActorFlags3` | writes actor flag group 3. |
| `FE 2F` | 4 | `0x8008E054` | `WriteActorFlags4` | writes actor flag group 4. |
| `FE 30` | 6 | `0x8008E3E8` | `JumpUnlessCurrentActorFlags1Set` | branches to the encoded destination when the requested mask does not intersect current actor flag word 1, otherwise advances five bytes. |
| `FE 31` | 6 | `0x8008E414` | `JumpUnlessCurrentActorFlags2Set` | branches to the encoded destination when the requested mask does not intersect current actor flag word 2, otherwise advances five bytes. |
| `FE 32` | 6 | `0x8008E440` | `JumpUnlessCurrentActorFlags3Set` | branches to the encoded destination when the requested mask does not intersect current actor flag word 3, otherwise advances five bytes. |
| `FE 33` | 6 | `0x8008E46C` | `JumpUnlessCurrentActorFlags4Set` | branches to the encoded destination when the requested mask does not intersect current actor flag word 4, otherwise advances five bytes. |
| `FE 34` | 7 | `0x8008E298` | `JumpUnlessTargetActorFlags1Set` | branches to the encoded destination when the requested mask does not intersect target actor flag word 1, otherwise advances six bytes. |
| `FE 35` | 7 | `0x8008E2EC` | `JumpUnlessTargetActorFlags2Set` | branches to the encoded destination when the requested mask does not intersect target actor flag word 2, otherwise advances six bytes. |
| `FE 36` | 7 | `0x8008E340` | `JumpUnlessTargetActorFlags3Set` | branches to the encoded destination when the requested mask does not intersect target actor flag word 3, otherwise advances six bytes. |
| `FE 37` | 7 | `0x8008E394` | `JumpUnlessTargetActorFlags4Set` | branches to the encoded destination when the requested mask does not intersect target actor flag word 4, otherwise advances six bytes. |
| `FE 38` | 6 | `0x8008E1B4` | `WriteActorDistance` | resolves two actor selectors, computes their planar distance from fixed-point X/Z positions, writes zero if either actor is absent, and stores the result in the selected script variable. |
| `FE 39` | 4 | `0x8008D230` | `SetActorAnimationOffsetScale` | sets the global multiplier used to convert animation displacement samples into planar actor offsets and advances three bytes. |
| `FE 3A` | 4 | `0x8008CED0` | `SetPartyFrameMask` | resolves a character selector and sets that character's bit in the party frame mask. |
| `FE 3B` | 4 | `0x8008CE64` | `ClearPartyFrameMask` | resolves a character selector and clears that character's bit in the party frame mask. |
| `FE 3C` | 6 | `0x8008B180` | `PlayMechaAnimation` | starts the selected animation on a mecha slot and records its current animation identifier. |
| `FE 3D` | 11 | `0x8008AEC8` | `SetPrimaryMechaMatrixRow` | writes three signed values into a selected row of the primary mecha field matrix. |
| `FE 3E` | 11 | `0x8008AFD8` | `SetSecondaryMechaMatrixRow` | writes three signed values into a selected row of the secondary mecha field matrix. |
| `FE 3F` | 8 | `0x8008B0E8` | `SetBackgroundClearColor` | sets the background clear red, green, and blue components. |
| `FE 40` | 8 | `0x80092148` | `WriteLineScrollByte` | writes one byte at the requested offset of a line-scroll buffer when the offset is below its configured length and advances seven bytes. |
| `FE 41` | 4 | `0x8009FC48` | `PartyMemberRideGear` | mounts a party member in Gear. |
| `FE 42` | 4 | `0x8009FCAC` | `PartyMemberDisembarkGear` | disembarks a party member. |
| `FE 43` | 2 | `0x8009B15C` | `DisablePartyFollow` | disables party members following the leader. |
| `FE 44` | 2 | `0x8009B184` | `EnablePartyFollow` | enables party following, clears follow state, and seeds the complete movement history with the leader's current state. |
| `FE 45` | 3 | `0x8009A0FC` | `SetAnimationOverride` | replaces the current actor animation override with the immediate animation identifier. |
| `FE 46` | 3 | `0x8008AE5C` | `SetMechaRotationAuthority` | selects whether the current actor follows its mecha root rotation or drives that rotation. |
| `FE 47` | 4 | `0x8008B144` | `SetDefaultModelTurnRate` | sets the default angular step used when rotating field models toward their targets. |
| `FE 48` | 9 | `0x8008B518` | `SetCameraProjectionAngles` | sets the three signed camera projection angles. |
| `FE 49` | 2 | `0x8008DAFC` | `ClearCurrentActorParent` | sets the current actor's parent identifier to 0xFF and advances one byte. |
| `FE 4A` | 4 | `0x8008ACE8` | `BeginSpecialAnimationLoad` | replaces the current actor's special-animation allocation and begins loading the selected animation resource. |
| `FE 4B` | 2 | `0x8008A9AC` | `FinalizeSpecialAnimationLoad` | waits for loading to finish, attaches the special animation to the current sprite, and retries while busy. |
| `FE 4C` | 3 | `0x8008A974` | `SetForcedAnimationAndReleaseOverride` | sets the complemented forced-animation identifier and clears actor flag 0x10000. |
| `FE 4D` | 3 | `0x8008A93C` | `SetForcedAnimationComplement` | stores the bitwise complement of an immediate animation identifier as the current actor's forced animation. |
| `FE 4E` | 2 | `0x8008AA60` | `FreeSpecialAnimation` | releases the current actor's special-animation allocation and resets its resource identifier. |
| `FE 4F` | 2 | `0x80093BB0` | `EnableEncounterIndicator` | enables the encounter indicator. |
| `FE 50` | 2 | `0x80093BD4` | `DisableEncounterIndicator` | disables the encounter indicator. |
| `FE 51` | 2 | `0x80093BFC` | `EnableCompass` | enables the Field compass. |
| `FE 52` | 2 | `0x80093C20` | `DisableCompass` | disables the Field compass. |
| `FE 53` | 2 | `0x80093AC8` | `DisableEncountersAndCompass` | disables encounter and compass state. |
| `FE 54` | 2 | `0x80093B10` | `EnableEncountersAndCompass` | restores encounter and compass state. |
| `FE 55` | 2 | `0x80093740` | `OpenNormalMenu` | queues menu mode 0 with the configured menu argument, yields, increments the open-menu count, and advances one byte. |
| `FE 56` | 4 | `0x80093930` | `OpenMenuMode1WithSelection` | copies the evaluated selection into script variable 1 and persistent menu state, queues menu mode 1, yields, and advances three bytes. |
| `FE 57` | 2 | `0x800937E0` | `OpenLoadGameMenu` | queues menu mode 2, yields, increments the open-menu count, and advances one byte. |
| `FE 58` | 4 | `0x80093824` | `OpenEnterNameMenu` | queues the Enter Name menu for the selected character or name record and yields the current script. |
| `FE 59` | 4 | `0x800939A0` | `OpenShopMenu` | queues the selected shop inventory and yields the current script. |
| `FE 5A` | 4 | `0x80093A04` | `OpenGearShopMenu` | queues the selected Gear shop inventory and yields the current script. |
| `FE 5B` | 4 | `0x8008B210` | `SetActorModelTurnRate` | sets the current actor's model-specific angular rotation step. |
| `FE 5C` | 3 or 5 | `0x800A0FD8` | `LoadCurrentActorMecha` | waits for I/O, hides or frees the indexed mecha, asynchronously loads replacement files, then constructs and binds the replacement with the actor's scale and position. |
| `FE 5D` | 8 | `0x8008F6AC` | `PlaySoundEffectWithParameters` | plays the requested sound effect on channel 3 with resolved volume and pan values and advances seven bytes. |
| `FE 5E` | 4 | `0x8008F2D8` | `SetCurrentActorTransparencyMode` | applies the selected transparency mode to the current actor and advances three bytes. |
| `FE 5F` | 9 | `0x8008F1C8` | `SetCurrentActorDualLightingColors` | conditionally assigns either or both RGB lighting triplets to the current actor and advances eight bytes. |
| `FE 60` | 10 | `0x8008EC30` | `StartPreset2DPresentation` | starts a 2D presentation using resource, offset, limit, and mode arguments with mode-specific viewport defaults, or rewinds until resources are available. |
| `FE 61` | 2 | `0x8008E9F8` | `WaitFor2DPresentationReady` | rewinds while presentation readiness is clear, otherwise clears readiness and advances one byte, yielding after every check. |
| `FE 62` | 6 | `0x8008F444` | `SetSoundChannelVolume` | immediately sets the selected sound channel volume and advances five bytes. |
| `FE 63` | 6 | `0x8008F4A0` | `SetSoundChannelPan` | immediately sets the selected sound channel pan and advances five bytes. |
| `FE 64` | 4 | `0x8008F5E4` | `WaitForSoundChannelMaskClear` | rewinds while any selected sound channel remains active, otherwise advances three bytes, yielding after every check. |
| `FE 65` | 6 | `0x8008F4FC` | `PlayOrStopSoundEffectChannel` | stops the selected channel when the effect identifier is zero, otherwise starts that effect with default volume and pan, then advances five bytes. |
| `FE 66` | 10 | `0x8008F558` | `PlaySoundEffectChannelCustomized` | restarts the selected channel with the supplied effect identifier, volume, and pan, then advances nine bytes. |
| `FE 67` | 20 | `0x8008EE14` | `StartExplicit2DPresentation` | starts a 2D presentation with explicit resource, offsets, mode, dimensions, and scale values, then advances 19 bytes. |
| `FE 68` | 7 | `0x80092C20` | `WalkPlayerToPositionAndWait` | waits for transition systems to become ready, walks the player toward resolved X and Z coordinates, preserves the player's preexisting control flag, and completes after reaching or failing to reach the destination. |
| `FE 69` | 6 | `0x8008A6E0` | `GetPartyProgressTotal` | writes a selected character's base-plus-remainder progression to a script variable, or zero when no character resolves. |
| `FE 6A` | 4 | `0x8008A604` | `SetLinkOrderingTableIndex` | sets the ordering-table link index from an immediate or variable operand. |
| `FE 6B` | 6 | `0x8008A640` | `SetPartyProgressRemainder` | sets a selected character's progression remainder to the nonnegative difference between a requested total and its base progression. |
| `FE 6C` | 2 | `0x8008A5A0` | `ClearControllerEnableFlag` | clears the controller enable byte when the operand is zero, then advances one byte. |
| `FE 6D` | 2 | `0x8008FB28` | `SnapshotCameraProjectionBaseline` | snapshots the current scaled projection depth, projection dip, and camera yaw as script baselines, resets camera scale to 0x1000, and advances one byte. |
| `FE 6E` | 5 | `0x8008FABC` | `SetSceneAngleY` | assigns both scene Y-angle fields. |
| `FE 6F` | 9 | `0x8008B45C` | `SetGlobalModelRotation` | sets the three signed global model-rotation angles used to rebuild the rendering matrix. |
| `FE 70` | 4 | `0x80089F54` | `SetBackgroundModelRenderMode` | stores the script byte minus 0x80 as the mode controlling camera-offset application and forced rendering for background models, then advances three bytes. |
| `FE 71` | 4 | `0x8009899C` | `WriteCurrentActorRotationAngle` | writes the current actor rotation modulo one revolution to script memory. |
| `FE 72` | 11 | `0x800988B8` | `WriteInterpolatedAngle` | interpolates between two scripted angles by a scripted step and writes the resulting angle to script memory. |
| `FE 73` | 13 | `0x8009861C` | `WriteDistanceBetween2DPoints` | computes the planar distance between two scripted points and writes the result to script memory. |
| `FE 74` | 4 | `0x800985BC` | `DebugPrintVariableHexAndDecimal` | evaluates the encoded operand and prints it in hexadecimal and decimal when Field debug output is enabled. |
| `FE 75` | 5 | `0x800989F0` | `WriteActorRotationAngle` | writes a selected actor rotation modulo one revolution to script memory when that actor is valid. |
| `FE 76` | 17 | `0x80098738` | `WriteDistanceBetween3DPoints` | computes the spatial distance between two scripted points and writes the result to script memory. |
| `FE 77` | 3 or 12 | `0x8008A2E8` | `ManageOverlayImageAsset` | waits for archive I/O, then loads an indexed image into memory, uploads it with optional VRAM backup, or releases it according to the mode, and stops the current VM cycle. |
| `FE 78` | fallback | `0x8008A4F0` | `ReservedOpcode78NoOp` | returns immediately without changing state as the reserved opcode 0x78 entry. |
| `FE 79` | fallback | `0x8008A4E8` | `ReservedOpcode79NoOp` | returns immediately without changing state as the reserved opcode 0x79 entry. |
| `FE 7A` | fallback | `0x8008A4E0` | `ReservedOpcode7ANoOp` | returns immediately without changing state as the reserved opcode 0x7A entry. |
| `FE 7B` | fallback | `0x8008A518` | `ReservedOpcode7BNoOp` | returns immediately without changing state as the reserved opcode 0x7B entry. |
| `FE 7C` | fallback | `0x8008A500` | `ReservedOpcode7CNoOp` | returns immediately without changing state as the reserved opcode 0x7C entry. |
| `FE 7D` | fallback | `0x8008A508` | `ReservedOpcode7DNoOp` | returns immediately without changing state as the reserved opcode 0x7D entry. |
| `FE 7E` | fallback | `0x8008A510` | `ReservedOpcode7ENoOp` | returns immediately without changing state as the reserved opcode 0x7E entry. |
| `FE 7F` | 2 | `0x8008A244` | `WaitForVideoPlayback` | yields and repeats while video playback remains active, advances when playback finishes, and stops the current VM cycle. |
| `FE 80` | 16 | `0x80089FD0` | `SetPanoramaGeometryParameters` | stores eight panorama geometry and texture parameters, forces a zero third parameter to one, clears the fifth parameter, and advances fifteen bytes. |
| `FE 81` | 9 | `0x8008A08C` | `SetPanoramaOrientationVector` | evaluates and stores the panorama orientation vector's X, Z, and Y components and advances eight bytes. |
| `FE 82` | 26 | `0x8008A148` | `SetPanoramaColorParameters` | stores three RGB triplets and three additional panorama values, enables panorama rendering, and advances twenty-five bytes. |
| `FE 83` | 4 | `0x80092FB4` | `SetBootModeWhenReady` | when boot-mode changes are permitted, disables random encounters for the transition, clears the readiness gate, stores the requested boot mode, and advances three bytes. |
| `FE 84` | 10 | `0x800933F8` | `StartBattleWithReturnField` | waits for coordinator readiness, stores the requested battle and optional post-battle Field destination and variable 2 value, marks the battle handoff pending, yields, and advances nine bytes. |
| `FE 85` | 4 | `0x8008A2A0` | `WriteVideoPlaybackFrame` | writes the current video playback frame to a script variable and advances three bytes. |
| `FE 86` | 3 | `0x80089F94` | `SetVideoTransitionMode` | stores the raw video-transition mode byte and advances two bytes. |
| `FE 87` | 2 | `0x800936E4` | `WaitForMenuClose` | yields while a menu remains open by rewinding one byte, otherwise advances one byte. |
| `FE 88` | 19 | `0x80089BF0` | `ConfigureProximityLightGradient` | stores indexed near and far RGB triplets plus the interpolation range used for distance-dependent model lighting, then advances eighteen bytes. |
| `FE 89` | 12 | `0x80089DCC` | `SetProximityLightAnchor` | stores an indexed XYZ lighting anchor and its resolved actor association, using -1 when resolution fails, then advances eleven bytes. |
| `FE 8A` | 4 | `0x80089F18` | `SetSpatialAudioListenerSource` | selects the controlled actor, camera eye, or camera target as the spatial-audio listener source and advances three bytes. |
| `FE 8B` | 4 | `0x80089B54` | `WriteCurrentActorPartySlot` | writes the current actor's party slot from the three active mappings or 0xFF when absent, then advances three bytes. |
| `FE 8C` | 8 | `0x8008F3D0` | `RampSoundChannelVolume` | ramps the selected sound channel toward a supplied volume over a supplied duration and advances seven bytes. |
| `FE 8D` | 4 | `0x8008F394` | `SetSoundChannelPreserveMask` | sets the mask that exempts selected sound channels from the next field audio reset and advances three bytes. |
| `FE 8E` | 6 | `0x8008F348` | `SetScreenBoundsPadding` | sets horizontal and vertical padding used by actor screen-bound checks and advances five bytes. |
| `FE 8F` | 9 | `0x80088790` | `ResetParticleConfigResolvedActor` | resolves an actor with zero fallback, stores three particle modifiers, resets its default particle banks, normalizes mode values 1 through 3 to 0x10 through 0x30, adds four VM cycles, and advances eight bytes. |
| `FE 90` | 10 | `0x80089004` | `InitializeParticleBank` | initializes a script particle bank. |
| `FE 91` | 15 | `0x80089174` | `SetParticleBankPosition` | sets particle-bank position. |
| `FE 92` | 15 | `0x80089374` | `SetParticleBankPhysics` | sets particle-bank physics. |
| `FE 93` | 12 | `0x80089574` | `SetParticleBankParameters` | sets particle-bank parameters. |
| `FE 94` | 11 | `0x800896D4` | `SetParticleBankScale` | sets particle-bank scale. |
| `FE 95` | 15 | `0x80089880` | `SetParticleBankColor` | sets particle-bank color. |
| `FE 96` | 2 | `0x80089A80` | `ParticlesInitialize` | initializes script particles. |
| `FE 97` | 3 | `0x80089AE4` | `StopParticleActor` | stops an actor particle effect. |
| `FE 98` | 4 | `0x800884CC` | `SetSpatialAudioFalloffDistance` | stores the evaluated maximum distance used for spatial-audio attenuation and advances three bytes. |
| `FE 99` | 3 | `0x8008848C` | `SetMenuOpenArgument` | stores the inverse of the supplied one-bit menu argument and advances two bytes. |
| `FE 9A` | 10 | `0x8008F0B4` | `SetTargetActorDualLightingColors` | conditionally assigns either or both RGB lighting triplets to a resolved actor and advances nine bytes. |
| `FE 9B` | 4 | `0x8008EF5C` | `StartTransitionEffectMode1` | selects transition effect mode 1 with the supplied effect parameter and advances three bytes. |
| `FE 9C` | 4 | `0x8008EFA0` | `StartTransitionEffectMode2` | selects transition effect mode 2 with the supplied effect parameter and advances three bytes. |
| `FE 9D` | 4 | `0x8008F070` | `StartTransitionEffectMode3` | selects transition effect mode 3 with the supplied effect parameter and advances three bytes. |
| `FE 9E` | 10 | `0x8008EFE4` | `SetDrawClipRegion` | applies four supplied display clipping dimensions and advances nine bytes. |
| `FE 9F` | 5 | `0x800883D4` | `SetPartyFrameLock` | resolves a character and sets or clears that character's party-frame-lock bit according to the mode byte, then advances four bytes. |
| `FE A0` | 13 | `0x8008EA58` | `StartSigned2DPresentation` | starts a signed-parameter 2D presentation with fixed viewport defaults, or rewinds and yields until presentation resources are available. |
| `FE A1` | 6 | `0x80088360` | `SetCharacterGear` | assigns a character Gear. |
| `FE A2` | 2 | `0x8008825C` | `WaitForMusicLoad` | yields and repeats while music loading is pending, advances when loading finishes, and stops the current VM cycle. |
| `FE A3` | 3 | `0x800881E8` | `ToggleVramRegionBackup` | captures the fixed VRAM region when the mode is zero or restores and releases it otherwise, then advances two bytes. |
| `FE A4` | 2 | `0x80088198` | `RestoreAllGearFuelAndEther` | restores fuel and ether to their maxima for all twenty Gears and advances one byte. |
| `FE A5` | 8 | `0x80088C1C` | `SetParticleRotationAngle` | stores the active particle slot value, merges evaluated flags into its high byte, stores its rotation angle, adds four VM cycles, and advances seven bytes. |
| `FE A6` | 6 | `0x800888A4` | `ConfigureSingleKeySpriteMotion` | initializes current-actor sprite motion mode 2, stores one nibble selector and one packed 9-bit key value, marks the motion active, and advances five bytes. |
| `FE A7` | 10 | `0x800889BC` | `ConfigureDualKeySpriteMotion` | initializes current-actor sprite motion mode 3, stores two nibble selectors and two packed 9-bit key values, marks the motion active, and advances nine bytes. |
| `FE A8` | 8 | `0x80090A10` | `WriteCurCameraTarget` | writes current camera target. |
| `FE A9` | 8 | `0x80090A94` | `WriteCurCameraPosition` | writes current camera position. |
| `FE AA` | 3 | `0x8008DB2C` | `SetCameraTrackedActor` | resolves the encoded actor and stores it as the camera tracking subject, then advances two bytes. |
| `FE AB` | 5 | `0x8008DC74` | `IncreasePartyGearHp` | VM Gear-HP increase handler. |
| `FE AC` | 5 | `0x8008DD6C` | `DecreasePartyGearHp` | VM Gear-HP decrease handler. |
| `FE AD` | 5 | `0x80096B58` | `WritePartyMemberHp` | writes party-member HP. |
| `FE AE` | 8 | `0x80096AF4` | `ConfigureSpecialMovementAnimation` | sets the special-movement enable value, animation identifier, and countdown reload, clears the active countdown, and advances seven bytes. |
| `FE AF` | 19 | `0x8008800C` | `TransformActorJointOffset` | resolves an actor joint transform, applies it to a script-supplied vector, writes the transformed XYZ coordinates to three script variables, and advances eighteen bytes. |
| `FE B0` | 3 or 7 | `0x8008AACC` | `LoadWdsSoundBankSlot` | replaces a selected WDS sound-bank slot through asynchronous loading and finalizes the bank on re-entry. |
| `FE B1` | 2 | `0x80087FD4` | `InitializeCameraDirectionGauge` | loads the direction-gauge texture, allocates double-buffered packets, initializes 109 textured quads, and advances one byte. |
| `FE B2` | 5 | `0x80096D28` | `SetPartyMemberHp` | sets party-member HP. |
| `FE B3` | 5 | `0x80096E20` | `SetPartyMemberMp` | sets party-member MP. |
| `FE B4` | 5 | `0x80096C40` | `WritePartyMemberMp` | writes party-member MP. |
| `FE B5` | 2 | `0x80087FA4` | `IncrementPartyConvergenceOverride` | increments the party-convergence override counter and advances one byte. |
| `FE B6` | 3 | `0x80087E98` | `SetControlledAndTrackedActor` | resolves the requested actor, assigns it as both the physically controlled and camera-tracked actor, records whether control differs from the normal party leader, clears control flags from every actor, assigns control to the selected actor, and advances two bytes. |
| `FE B7` | 4 | `0x80087E5C` | `SetInteractionAvailabilityOverride` | stores an evaluated interaction-availability override and advances three bytes. |
| `FE B8` | 5 | `0x80087DE0` | `SetTransitionMusicId` | writes an evaluated music ID to the field-transition slot when the selector is zero or the battle-transition slot otherwise, then advances four bytes. |
| `FE B9` | 10 | `0x80087B5C` | `WriteCompleteWorldMapPosition` | writes all four saved world-map position components to script variables and advances nine bytes. |
| `FE BA` | 11 | `0x80087C34` | `SetWorldMapPosition` | evaluates and stores all four saved world-map position components and advances ten bytes. |
| `FE BB` | 4 | `0x80087D30` | `WriteWorldMapVehicleState` | writes the saved world-map vehicle state to a script variable and advances three bytes. |
| `FE BC` | 5 | `0x80087D80` | `SetWorldMapVehicleState` | evaluates and stores the saved world-map vehicle state and advances four bytes. |
| `FE BD` | 8 | `0x80088B68` | `SetParticleAttachmentMode` | sets particle flag 0x80 for mode 1 or 0x40 for mode 2, adds four VM cycles, and advances seven bytes. |
| `FE BE` | 2 | `0x80087C0C` | `EnableVideoFrameTileAnimation` | enables animated video-frame tile cycling and advances one byte. |
| `FE BF` | 14 | `0x80087848` | `SetupBattling` | waits for field resources and music readiness, then stores six battle parameters and requests battle setup. |
| `FE C0` | 4 | `0x80087800` | `WriteBattlingMatchResultCode` | copies the Battling mode match-result code to a script variable. |
| `FE C1` | 8 | `0x80088508` | `QueryPartySpriteAnimationStatus` | resolves a party actor, writes its sprite-animation status and actor index to script variables, clears nonterminal status values, adds four VM cycles, and advances seven bytes. |
| `FE C2` | 5 | `0x80088674` | `ResetParticleConfigImmediateActor` | selects an immediate actor with zero fallback, stores three particle modifiers, resets its default particle banks, normalizes mode values 1 through 3 to 0x10 through 0x30, adds four VM cycles, and advances nine bytes. |
| `FE C3` | 2 | `0x8009E014` | `SetCurrentActorFlags02000800` | sets flags 0x02000000 and 0x800 on the current actor. |
| `FE C4` | 3 | `0x8009DF78` | `SetActorFlags02000800ById` | sets actor flags 0x02000000 and 0x800 by ID. |
| `FE C5` | 6 | `0x80086F7C` | `SetModelAnimation` | assigns a model animation. |
| `FE C6` | 4 | `0x8008BC80` | `QueueVariablePartyCharacterLoad` | resolves an immediate-or-variable character, reserves a staging slot, and begins loading when no duplicate or conflicting load exists. |
| `FE C7` | 6 | `0x800882B8` | `WriteActorCharacterGearId` | resolves an actor selector, writes its character's Gear ID or 0xFF to a script variable, and advances five bytes. |
| `FE C8` | 19 | `0x80088CF8` | `SetParticleDirections0To3` | sets particle directions 0 through 3. |
| `FE C9` | 19 | `0x80088D18` | `SetParticleDirections4To7` | sets particle directions 4 through 7. |
| `FE CA` | 3 | `0x800A0EE8` | `ReleaseCurrentActorMecha` | clears the actor's active-mecha flag, then either hides its indexed mecha or frees it and decrements the loaded-mecha count before yielding. |
| `FE CB` | 2 | `0x800A0EB0` | `RequestDirectorModeExit` | increments the director-mode exit signal, advances one byte, and yields the current script. |
| `FE CC` | 2 | `0x800A0E54` | `WaitForDirectorModeEnd` | advances when director-only mode is inactive, otherwise rewinds one byte and yields so the instruction is retried. |
| `FE CD` | 4 | `0x800A0DFC` | `WriteCurrentDiscNumber` | queries the current disc number, writes it to the selected script variable, and advances three bytes. |
| `FE CE` | 4 | `0x800A0DC0` | `SetMaxMechaCount` | sets the maximum mecha overlay entry count from a script operand and advances three bytes. |
| `FE CF` | 6 | `0x80093888` | `OpenMenuMode1WithFieldContext` | disables field controls, saves field and direction state, installs the requested field and variable 2 value, queues menu mode 1, yields, and advances five bytes. |
| `FE D0` | 6 | `0x8008764C` | `CloneGearAndAbilityState` | copies one Gear's persistent state and associated ability blocks to another slot and marks special destination slots present. |
| `FE D1` | 2 | `0x8008754C` | `SetGameStateFlag4000` | sets game-state flag 0x4000. |
| `FE D2` | 4 | `0x8008752C` | `SkipThreeByteInstruction` | advances past a three-byte no-op instruction. |
| `FE D3` | 18 | `0x80087420` | `ScaleTwoRatios` | scales two script ratios and writes both results. |
| `FE D4` | 3 or 11 | `0x80086FD0` | `ManageSpriteOverlayList` | mode 0 allocates and initializes a 33-entry sprite-overlay list, mode 1 links an indexed entry at evaluated screen coordinates, mode 2 frees the list, and mode 3 sets an indexed entry's RGB color. |
| `FE D5` | 6 | `0x80087960` | `WriteWorldMapPosition` | writes both persistent world-map position values to script variables. |
| `FE D6` | 6 | `0x800879D0` | `WriteGameState184EAnd1852` | writes two game-state fields to script memory. |
| `FE D7` | 7 | `0x80087AB8` | `SetWorldMapMarkerPositionXZ` | writes evaluated X and Z coordinates into the saved world-map marker position, clears Y and padding, marks the position valid, and advances six bytes. |
| `FE D8` | 3 | `0x80087A40` | `SetSpriteLightingBypass` | stores a script byte that enables or bypasses dynamic sprite color lighting. |
| `FE D9` | 3 | `0x80087A7C` | `SetRandomTurnDirectionTable` | stores a script byte selecting the direction table used for random actor turns. |
| `FE DA` | 2 | `0x80093790` | `OpenMenuMode6` | queues menu mode 6 with argument 1, yields, increments the open-menu count, and advances one byte. |
| `FE DB` | 4 | `0x80097200` | `RestoreCharacterHpAndMp` | restores HP and MP. |
| `FE DC` | 6 | `0x800873C4` | `SetPartySpriteColumnOffset` | writes an indexed byte controlling a party sprite's horizontal render column. |
| `FE DD` | 3 or 7 | `0x800871B0` | `VramSnapshotCommand` | allocates and captures a 256-pixel-wide VRAM region, applies indexed brightness conversion, or releases the working buffers according to subcommand. |
| `FE DE` | 6 | `0x80087148` | `OrCharacterRecordFlags` | applies bitwise OR with a script-supplied mask to the selected character's persistent flag field. |
| `FE DF` | 4 | `0x80086E1C` | `ConfigureDisplayMode` | configures display/draw environments. |
| `FE E0` | 3 | `0x80086DE0` | `SetPauseDisabled` | sets the Field pause-disable state. |
| `FE E1` | 6 | `0x80087580` | `CopyGear` | copies Gear state from a Field script. |
| `FE E2` | 2 | `0x80086D4C` | `SoftReset` | performs a software reset, requests script suspension, and advances the instruction pointer. |

Mode-dependent lengths describe one physical dispatch. Waiting handlers may preserve or rewind the PC until completion; the table reports the completed encoding, not a temporary update-time PC delta.
