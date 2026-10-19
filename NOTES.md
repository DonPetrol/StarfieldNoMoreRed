# No More Red - Starfield 1.16.244 outpost placement RE notes

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
