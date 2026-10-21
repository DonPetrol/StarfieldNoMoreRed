# No More Red - Starfield 1.16.244 outpost placement RE notes

> These notes were written by AI while creating this mod, and are kept up to date as development continues.

Addresses are RVAs + 0x140000000 (Steam build 1.16.244, SteamStub-wrapped, .text not encrypted).

## Status byte
`RootWorkshopStruct + 0x193` = current `Workshop::WorkshopCodes` (u8). `2` = VALID.
The per-item result is written at `root+0xD0+0xC3` (same byte) inside EvaluatePlacement.
Commit states (`StateUpdateAutoFoundation`, `StateBulldoze`, … vtable slot 23) check `== 2`.

## WorkshopCodes (from code->message switch @ 0x140F7BA90)
0 none · 1 NO_ITEM · 2 VALID · 3 FLOATING · 4 INTERSECTING · 5 TERRAIN_ONLY · 6 IN_WATER ·
7 NOT_IN_WATER · 8 NAVMESH_ONLY(turrets) · 9 SPLINE_TOO_LONG · 10 OUTSIDE_BUILD_AREA ·
11 REDUNDANT_SPLINE · 12 ATTACHED_SPLINE_INVALID · 13 UNSUPPORTED · 14 UNSUPPORTED_FORWARD_CAST(walls) ·
15 UNSUPPORTED_CLAMP_UP(ceilings) · 16 NO_SNAPPING · 17 NO_SNAPPING_FLOOR · 18 RADIUS_OVERLAP · 19 TIMER ·
20 OUTSIDE_PLAYABLE_AREA · 21 OVERLAPS_WORKSHOP · 22 RESTRICTED_AREA · 23 MISSING_RESOURCES (= harvester not on a resource vein; the data shows "Must be placed on a resource vein", tested) ·
24 MISSING_COMPONENTS · 25 TOO_CLOSE_TO_PLAYER · 26 FOUNDATION_TOO_TALL · 27 ON_ANIMATED_DOOR ·
28 INVALID_SURFACE · 29 PERK/ATTRIBUTES · 30 EXCEEDED_BUDGET · 31 EXCEEDED_MAX_COUNT · 32–40 blueprint/transfer-link errors

## Functions
| Address | Name (mine) | Notes |
|---|---|---|
| 0x140F3F9A0 | EvaluatePlacement(root, ?, tempItem**, ?) -> bool | Core validator. Calls intersection test + ValidatePlacement, sets most codes. Callers: 0x1421B51BA (StateUpdateSnapped), 0x1421D5C9C |
| 0x140F391C0 | ValidatePlacement(root, tempItem) | Build area (10), playable area (20), other workshop (21), restricted (22 via CheckRestrictedArea), terrain-only (5), water (6/7) |
| 0x140F3E2E0 | IntersectTest (hknpWorkshopIntersectCollector) | Result cached at root+0x18C |
| 0x140F38CB0 | CheckRestrictedArea(ref, bool) -> code | returns 2 or 0x16 |
| 0x140F4AA50 | IsInBuildArea(workshop, item, ...) | also used by 6 other callers |
| 0x1421D74B0 | UpdatePlacementStatus(root) | Resets code to 1, re-evaluates, may set 4/0x1A. Callers: 0x1421B4247 (StateUpdatePlacement), 0x1421BA52D (StateBulldoze) |
| 0x1421D8640 | UpdateTempItem(root, bool) | Calls IntersectTest unless global byte 0x145F0CC90 (never written; debug "skip checks" flag) |
| 0x1421DFA40 / 0x1421E0270 | TransferLink / PowerLink status | write codes 0x22–0x28 – not hooked |
| 0x140F38A40 | CanPlaceOutpostBeacon(out, ?, reason*, formID) | Scanner-side beacon gate. reason: 1 restricted, 2 skill, 3 max deployed, 4 invalid location, 5 in vehicle |
| 0x1415F6A90 | BeaconPlacementMessage | shows the sWorkshopBeaconPlacement_* message |
| 0x140F7BA90 | GetWorkshopCodeMessage(code) | switch → GMST |
| 0x1421F4CC0 | WorkshopUI::ShowPlacementCode | skips codes 0 and 2 |

## Data-side hooks the engine already has (no DLL needed)
Hardcoded actor values (set on base objects through object properties):
`WorkshopIgnoreSimpleIntersections`, `WorkshopItemOverlap`, `WorkshopAnythingIsGround`,
`WorkshopAllowUnsupportedStacking`, `WorkshopItemClampDirection`, `WorkshopItemZOffset`, `WorkshopSnapPointRadius`.
Default objects: `WorkshopSkipIntersections_DO`, `OutpostHarvesterPlaceAnywhere_DO`,
`OutpostAllowPlacementConditions_DO`, `OutpostNoPlacementKeyword_DO`, `WorkshopRestrictedBuildRadius_DO`,
`WorkshopItemMustSnap_DO`, `OutpostNoSnappedGroundIntersectionAV_DO`, `WorkshopBuildRadiusAV`,
`WorkshopPlacementTerrainOnlyAV`, `WorkshopPlacementWaterAllowedAV`, `WorkshopPlacementWaterOnlyAV`.

INI: `[Workshop] bConstrainWorkshopIsoCamToBuildArea=0`, `fPlayerOutsideBuildAreaTimerDuration`, `bBudgetEnabled=0`.

## Startup safety checks
* Code numbers are re-derived from GetWorkshopCodeMessage (ID 74295, 0x140F7BA90): `mov ecx,[rdx+rax*4+TBL]` jump table, each case `mov rax,[rip+Setting]; ret`, GMST name at Setting+0x10. INI keys are matched by GMST name, so a renumbered enum is followed; a missing name is never relaxed.
* All direct callers are found in a single pass over .text. No signature fallback: the plugin declares Address Library v2, so SFSE won't load it without one.

## Known gaps / next steps
* When a code is relaxed after the fact, EvaluatePlacement's "on VALID" side effect (vcall +0x248 on root+0x30) has already been skipped. If the preview misbehaves, move the hook inside the function, just before `cmp byte ptr [r?+0xC3], 2` at the LAB_140F400F4 block.
* Restricted-area outpost beacons are also blocked earlier, by CanPlaceOutpostBeacon in the scanner. The plugin doesn't hook that yet.
* Hotkeys also fire while typing in the console (GetAsyncKeyState).
* Hotkeys show the game's own HUD notification (ShowHUDMessageEvent); set ShowMessages=0 to silence them.

## Address Library IDs (format 5 / "v2", Steam)
| ID | 1.16.244 RVA | Function |
|---|---|---|
| 73783 | 0xF3F9A0 | EvaluatePlacement |
| 121599 | 0x21D74B0 | UpdatePlacementStatus |
| 121268 / 121287 / 121360 | 0x21B2360 / 0x21B4B70 / 0x21BA300 | commit states used to auto-detect the status offset |
| 121290 / 121588 / 121280 / 121363 | – | callers (StateUpdateSnapped, EvaluateTempItem, StateUpdatePlacement, StateBulldoze) |
| 74058 | 0xF5A1A0 | AutoFoundationHeight: `cmp byte [rbp-0x80],0; je; mov byte [r15+0x192],1` sets "foundation too tall" flag (root+0x192 -> code 26). NOP'd while No More Red relaxes FoundationTooTall or foundations are off |
| 74057 | 0xF5A050 | FoundationPieceCount(data, parts, height, flag, ?, float* outHeight) -> pieces (base + middles + top). 3 callers, all in AutoFoundationHeight; the "already built" call passes outHeight = null. Foundations off: new counts return 0 |
| 121569 | 0x21D19E0 | GetSnapCandidates(out vector, root, …) – object snapping; empty result = nothing to snap to |
| 929062 | 0x5F5E120 | Setting `bWorkshopTempItemFollowGround:Workshop` (+8 value, +0x18 name). 0 = crosshair cast ignores class-3 hits (statics, foundations) but NOT terrain (tested). Not used |
| 430809 / 74019 | 0x4C250C0 / 0xF563A0 | ItemPlacementForwardCastCollector vtable / AddHit (slot 4). Surface snapping off: AddHit drops all hits during SurfaceCast (class-0-only via ClassifyHit 0xF8D490, ID 74435, let outpost objects still block - tested) |
| 121584 | 0x21D3DD0 | SurfaceCast(root, …) -> hit. Writes hit normal.z at root+0x5A4 (FLT_MAX on miss). Surface snapping off: on a miss, normal.z = 0 (skips the drop pass) |
| 121565 | 0x21D09C0 | DropToGround(root, ref*, ref*): ground info for the held item (releases both refs). 3 callers, all in UpdatePlacementStatus; each is followed by a clamp `z = ground + offset` when ground type (root+0x12C) is 1/2/6. Surface snapping off: ground type cleared after the call |
| 1016851 | 0x21E3800 | SelectPlacementBehavior(root, mode) -> &table[mode], table @ 0x145F5E400, stride 0x78. [0] follow ground (place 0x21D4280), [1] follow camera / Precise Placement (place 0x21D5500). Not hooked |
| 121583 | 0x21D3C70 | FollowCameraCastLength(root): length of hold offset vec3 at root+0x4D0 (x1.02), else fWorkshopTempItemDefaultFollowCameraCastLength (8.0) + half bounds. Page Up/Down scale that vec3; when it is zero (new items) they change the cast length settings (IDs 929065 default / 929068 decorate) instead |
| 74295 | 0xF7BA90 | GetWorkshopCodeMessage(code): code -> GMST switch, used to verify code numbers by name at startup |
| 86800 | 0x14915A0 | BSTGlobalEvent::EventSource<ShowHUDMessageEvent>::GetSingleton |
| 123824 | 0x22C9300 | BSTEventSource::Notify(source, functor) |
| 1186742 / 139340 | 0x28CBA80 / 0x28CAEA0 | BSFixedString ctor / release |
| 442192 | 0x4C7E350 | ShowHUDMessageEvent notify-lambda vtable |

ShowHUDMessageEvent layout (from beacon error caller @ 0x1415F6C01): `{ BSFixedString text; BSFixedString sound; u16 flags = 1 }`.
Functor passed to Notify: `{ vtbl, &event, source }`.

Surface snapping notes (in-game name; code calls it ground collision):
* ItemPlacementForwardCastCollector (vtbl 0x144C250C0, ctor 0x140F56230(this, ignoreRef, ignoreGround)) stores ignoreGround at +0xE0.
  Its hit filter (0x140F563A0) classifies each hit with 0x140F8D490 and drops class-3 hits when the flag is set.
  Class 0 = collision layers 13/17 (terrain/ground) or matching material 0x145F0B6C8; 2/3 = workshop item (item+0x40 == 1 -> 2); 3 = everything else (statics, rocks).
  Tested: setting the flag made foundations pass-through but terrain still blocked -> terrain is class 0.
* The game's Precise Placement mode = placement behaviour [1] (follow camera). Earlier versions forced it; the game has its own toggle.

Older surface snapping notes:
* Toggling `bWorkshopTempItemFollowGround` at runtime had no visible effect (tested).
* Forcing a cast miss + normal.z 0 only stopped the item tilting/lifting to the surface; it still sat at the crosshair hit and dropped when aiming at the sky (tested). The position comes from the placement behaviour.
* Hidden INI settings that pick the behaviour (all default 0): `bUseAlwaysFollowGround`, `bUsePrecisePlacing`, `bUseFollowCameraForInteriors` ([Workshop]). In first person the game forces behaviour [0] unless bUseAlwaysFollowGround is set.
The drop-to-ground pass in UpdatePlacementStatus runs when `fWorkshopDownwardShapeCastMinNormal (0.4) < root+0x5A4`
(or `root+0x5A4 < fWorkshopUpwardShapeCastMinNormal (-0.4)` for ceiling items), or when the placement controller's +0x40 == 1.

## Compatibility (v4)
* Call sites are found at plugin load but redirected at SFSE PostPostLoad, after every plugin's
  Load/PostLoad. A site another plugin already hooked is chained (our wrapper calls its target).
* Starfield Engine Fixes 21.2 (hardcoded 1.16.244 RVAs) hooks the same calls and aborts with a
  fatal MessageBox + TerminateProcess unless each E8 still targets the game function:
  - bUnrestrictedBuildModePlacement: both callers of EvaluatePlacement (0xF3F9A0) -> forces status 2
    unless 23/24 (EnableResourceRequirement) or 29 (EnablePerkRequirement); also
    0x21D770C `mov [r14+193],1` -> 2, NOPs 0x21D8453 and 0x21D84B7 status writes,
    0x21C5FA5 `lea ebp,[rcx+2]` -> `lea ebp,[rcx]`.
  - bBuildModeItemContinuousRotation: NOPs rotation store at 0x21D7950, hooks both callers of
    UpdatePlacementStatus (0x21D74B0) and writes root+0x184 itself.
  Both default to 0. With v4 both install first and No More Red wraps them.
* Log: Documents\My Games\Starfield\SFSE\Logs\NoMoreRed.log (fallback: plugin folder).

## Deploy rule (from user)
After every DLL deploy, list the target folder and confirm the DLL size/mtime matches the new build before reporting.
Stage each deploy under a NEW filename in outputs (e.g. NoMoreRed_vXX.dll): re-committing the same staged path can write a stale copy.

## Build mode gate / rotate with view (v5)
* Hotkeys act only while UI::IsMenuOpen(WorkshopMenu): UI singleton 937580 (0x145FD9B80), IsMenuOpen 130475
  (0x142544EC0), WorkshopMenu name getter 130425 (0x14253DDB0, verified by its "WorkshopMenu" lea).
* Held item yaw = root+0x184 (radians, ref angle-z convention). UpdatePlacementStatus: surface branch does
  yaw += root+0x70 (rotate input) so it stays world-fixed; follow-camera branch (0x1421D78E1..0x1421D7950)
  recomputes it from the camera transform (0x1421CE760) every frame.
* Rotate with view: before UpdatePlacementStatus, yaw += wrap(playerHeading - lastHeading); player 922868
  (0x145F43230), heading at +0x88. SEF's version instead NOPs 0x1421D7950, hooks UpdatePlacementStatus
  callers, tracks an absolute angle per held ref (handle lookup each frame) and polls its own keys.

## 1.1.0 review
* Build-area tests (WORKSHOP BOUNDARY off): ValidatePlacement 73744 -> IsInBuildArea 73891, surface cast 121585 and
  follow-camera cast 121587 -> IsPointInBuildArea 73890 (these return code 10 and skip the held-item update = vanish),
  player outside-area timer 121563 -> IsInBuildArea (player). All four calls answer "inside".
* Foundation check red tint: UpdatePlacementStatus `mov [r14+193],al; ...; call 0x1421C5F70 (highlight, r8b=1)`
  at 0x1421D8463 - told "valid" when the code will be relaxed.
* Surface snapping off: ground type (root+0x12C) cleared after each drop pass and restored after UpdatePlacementStatus
  (commit builds the auto-foundation from it).
* Rotate with view / fine rotate: own yaw fed through the two re-read atan2 calls (0x1421D7947 neg, 0x1421D7A26);
  re-read changes (= rotate keys turning the item) are added as deltas; snapped flag root+0x18B.
* Ctrl+arrows: exe IAT USER32!GetRawInputData hooked; arrow key-downs hidden while Ctrl held in build mode, and
  their key-ups only if the key-down was hidden.
* Held item tracked from UpdatePlacementStatus (was SurfaceCast), so distance / fine rotate don't depend on surface snapping.
* Controls (post-1.1.0): fine rotate = Ctrl + LMB / RMB, hold distance = Ctrl + mouse wheel. The same raw input hook
  also edits RIM_TYPEMOUSE events while Ctrl is held in build mode: the configured buttons' down/up flags are cleared
  (up only if its down was), RI_MOUSE_WHEEL is cleared and its delta queued for PollKeys (120 per notch).

## Edit placed objects (in progress)
* Build-mode states are static objects (0x14597A980..9E8): CycleSnapBehavior, CreateReferences, CreatePowerLink,
  CreateTransferLink, UpdatePowerLink, DuplicateReferences, ModifyColors, InspectColorItems, Inspect, UpdatePlacement,
  UpdateAutoFoundation, PostCommitEdit, UpdateTransferLink, SetupReferences. GoTo helpers (0x1421BCF00...) call
  current->slot3 (Exit) then new->slot1 (Enter); current state at machine+0x210.
* StateInspect (vtbl ID 467015, Enter 121230, Exit 121231) = looking at a placed object. Its target (ID 121244):
  picker 0x145F5D548 vfunc +0x148, else +0x40, else handle 0x145F5D868. Move = Inspect slots 28/30 -> selection
  at root+0x370 -> CreateReferences -> UpdatePlacement.
* Placed objects edited with MoveRefrToPosition (ID 118176; ref, &nullHandle, cell=ref+0xB0, world=cell+0x110 if
  exterior, pos*, angle*) - ref angle at +0x80, position at +0x8C - and SetScale vfunc 0x430 (read from ID 118446);
  GetScale ID 63258 (u16 at +0xD0 * 0.01). LookupRefByHandle 35638, release 38742, null handle 944496.

## Grouped packins (structures) - edit placed objects
* Re-place via the game's Move + Build was removed (buggy in game); the source with it is kept in backup/NoMoreRed_with_replace.cpp.
  Move holds a temporary copy (StateDuplicateReferences), not the original ref, so pinning by form ID never matched.
* Structures are grouped packins: the crosshair picks the root (base = PackinPivotDummy, global ID 894675, base 00015804),
  the visible pieces are separate refs that don't follow it.
* ExtraGroupedPackin = extra type 0xDA (size 0x70): +0x40 BGSPackIn, +0x58 root ref form ID, +0x60 template ref ID in the packin cell.
* GroupMembers (ID 73727, 0xF366C0)(ref, BSTArray<ref*>*, excludeSelf): root ID = own form ID if base is the pivot dummy, else
  0xDA +0x58 (after following linked ref keyword ID 883421); then the outpost (linked ref by WorkshopItemKeyword ID 913293)
  ExtraLinkedRefChildren (0x4D: +0x18 count, +0x20 cap (<0 = inline), +0x28 data; entries {u32 handle, keyword* @+8})
  -> every child with the same root ID. Re-implemented in the plugin (no game array allocator); globals verified by scanning 73727.
* Helpers: GetExtraData 37878 (list, type), GetLinkedRef 44922 (list, keyword, bool). Cell refs: cell+0x80 count, +0x88 array,
  lock +0x118; ForEachReference 63054 (cell, functor{vtbl slot 2 = call(this, ref**)}, flag).
* WorkshopItem classes: Single / Packin (handles at +0x50/+0x58, root ref at +0xD0) / DecoratedHab / BulldozeOnly / InventoryItem.
* Collision after scaling: SetScale (REFR vfunc 0x430 = 0xB38130) only sets the u16 scale and the 3D root's local scale (+0x7C);
  havok shapes keep their built size. Rebuild = TESObjectREFR::Disable (vfunc 0x7E8, ID 63387), then 100 ms later
  TESObjectREFR::Enable (ID 63386, 0xB4A9C0, (ref, bool) - `btr ecx,0Bh` clears the disabled flag; the Papyrus Enable task
  0x1F15BE0 calls it with 1). Tested: collision matches the new scale after a re-enable.
  NOT vfunc 0x7F0 (ID 63388): calling that as Enable did nothing (flag stayed set, objects stayed invisible).
  Console `enable` (0xC28940) only manages the console's selection lists; the command itself enables the ref.
* Auto-foundations: linked-ref children of the object they hold up, keyword global ID 883416 (0x145E61430). The game walks
  them breadth-first (0xF76040; recursive variant 0xF7AF80 when bWorkshopUseRecursiveForEachAutoFoundation) from
  WorkshopItem slot 32 (ID 74072). The plugin adds them (BFS, every member of a group too) to the edited set, so they
  move / turn / scale with it and get the collision rebuild. Keyword verified by its load in 74072 (`mov r14,[rip+kw]`).
* Move of a scaled object: the held preview is a fresh unscaled copy; placement + auto-foundation are built from it.
  StateInspect slot 30 (ID 121236) = EditItemEvent handler (Move button) - hooked to note the hovered object's base + scale.
  After each UpdatePlacementStatus: held item = *(root+0x268) (WorkshopItem), slot 8 (vfunc +0x40)(item, u32* out) = its main
  ref handle (Single +0x50, Packin +0xE0); if that ref's base matches, SetScale it to the noted scale. Single objects only.
* Groups on Move: StateInspect slots 28 (121238) / 29 (121237) / 30 (121236) all start a move; all hooked to note the
  hovered object + its group root's scale. Held packin item: item+8 u32 count, +0x10 entries (stride 0x70): local
  transform rows +0x20..+0x5F (translation +0x50), piece handle +0x60. ApplyHeld -> 0xF5E8B0 (ID 74082) places each copy
  from its row every frame (position/rotation, not scale). Preview fix: SetScale every copy to the root's scale.
* Correction: the UI Move (121800) posts events handled by StateInspect slot 25 (ID 121241, 0x21AF270: EditItemEvent,
  event+8 = ref; non-'<' base -> GoTo StateDuplicateReferences 0x14597A9A8) or, when the target has variants
  (0x1421F1500), slot 15 (121234) / 13 (121233) (target handle at root+0x588). Dispatcher: engine+0x490 state vfunc +200 /
  +0x78 / +0x68 with (state, event, engine+0x4A0); R9 is passed through. Slots 28-30 are not the Move path.
* The group root (PackinPivotDummy) ignores SetScale (SetScale returns early for bases whose vfunc 0x340 is true unless
  type 0x09/0x38) and always reads 1.0. A group's scale is read from its first real piece (edit step and Move).
* After a Move is placed the game keeps the ORIGINAL refs (all found again by form ID; copies are deleted) and builds a
  new auto-foundation for them at scale 1.0. Plugin: 1.5 s later, LookupFormByID (ID 47401, raw TESForm*) the moved
  pieces, walk their foundations (with parent index), size each new piece to its parent's scale; per x/y column the
  top piece stays and the gaps below are scaled; then the collision rebuild.
* TESBoundObject bounds: min xyz at +0x50/+0x54/+0x58, max at +0x5C/+0x60/+0x64 (FoundationPieceHeight 74056 = +0x64 - +0x58
  for non-packin bases). Placed foundation pieces are scaled about their column's top surface (z + maxZ * scale).
* Group scale = most common member scale other than 1.0 (max-deviation picked outliers). GroupMembers now dedupes:
  a piece linked twice was being scaled twice per press, so piece scales drifted apart.
* Placed foundation pieces also get their x/y spacing from their first non-foundation ancestor scaled (the game lays
  them out from the unscaled foundation points). Layout is computed from a position snapshot before any piece moves.
* Placed foundation sizing: the whole foundation under an object is scaled rigidly about (object x, y, highest piece top
  under that object) - per-column anchoring left stairs/platform pieces at the wrong heights.
* Timing: placed-foundation sizing runs as soon as the new foundation pieces exist and their count is unchanged for 3
  polls (gives up 3 s after placing); its collision rebuild skips the wait. Scale-key rebuild wait 400 -> 250 ms.
* Cleanup (post-1.1.0 edit-placed work): Move hook slots 13/15 read the target handle at root+0x588 (as the game does)
  instead of a per-frame hovered-handle cache; collision rebuild uses pending/active lists (requests during a rebuild
  are no longer dropped) and waits for the scale keys to be released + 150 ms; preview copies that ignore SetScale
  are remembered and not retried each frame; shared ModeScale helper; diagnostics moved behind VerboseLog=1.
* Full-file cleanup: shared LoadsGlobal / CallsFn helpers verify AL globals (groups, foundation keyword); Holding() helper
  replaces three held-item time checks; startup logs a config summary (toggles, keys, steps) for support.
* Game-code check pass: MoveRefrToPosition now called exactly like Papyrus SetPosition/SetAngle (118197/118200: local
  null-handle copy, own cell, cell+0x110 worldspace for exteriors; it re-cells the ref itself). ExtraLinkedRefChildren is
  read under the extra-data list's read lock (LockRead 123862 on list+0x20, inlined UnlockRead: lock dec list+0x24,
  WakeByAddressSingle when (old & 0xFFC00FFF)==1 && (old & 0x3FF000)), presence bit byte 9 / 0x20, like 0xF42DF0 (73806).
  Enable's bool only matters for base types '$' and '2'. Disable unloads 3D via 0x18C1AD0 -> vfunc 0x538.
* Missing pickup/place sounds on some objects (reported once) was a one-off, not caused by the collision rebuild.
* Game-system reference: GAME_SYSTEMS.md (how the decompiled parts work, with readable reconstructions).
* Placing a moved object: the game removes the old foundation and builds the new one over several frames, so acting on
  "count stable for 3 polls" sized the OLD (already scaled) pieces and missed the new ones. Now waits until fresh pieces
  (scale 1.0, not disabled 0x800 / deleted 0x20) exist and the count has been unchanged for 150 ms; removed pieces skipped.
* Scalable(ref) mirrors SetScale's opening test: base vfunc 0x340 true -> ignored unless base type 0x09 / 0x38 or one of
  forms IDs 883405/883406/883407 (verified as `cmp rax,[rip+form]` in SetScale). Group scale = mode over scalable members
  (1.0 included; ignoring 1.0 misread groups whose drifted minority wasn't 1.0). Numpad scaling now sets every scalable
  piece to the same absolute scale (evens out drift); foundation pieces keep their ratio.
* Placed foundation: the pieces from before the move are recorded at Move; after placing, only pieces not in that set
  (and not disabled/deleted) are sized, once no old piece is left and the count has held for 150 ms (3 s timeout).
* Variant swaps (changeable walls etc.): Move on a target with variants goes to StateInspect slot 13/15, and the game
  replaces the piece in place with a new ref at 1.0 - nothing is held. Plugin: for up to 2 s after the swap, re-reads the
  group (via a recorded member, LookupFormByID) and scales any new scalable member to the old piece's scale.
* Step keys / axes / resets (numpad 7/9 scale step, -/+ move step, / and * rotate step, 0 reset rotation, . reset scale,
  Enter cycle axis). Steps 0.1..200% of 1.0 scale / 1 m / 90 deg. Numpad Enter = VK_RETURN with RI_KEY_E0 (raw input
  only; INI code 0x10D). Rotation maths: ToEuler 0x142BCDE50 (ID 147124) gives M = Rz(z)Ry(y)Rx(x) from a 3x4-stride
  matrix and the game uses it transposed, so a ref's world rotation W = Rx(-x)Ry(-y)Rz(-z) (positive heading = clockwise).
  World rotation A about pivot: W' = A W, p' = P + A(p - P). Reset rotation undoes the session's accumulated A per object.
