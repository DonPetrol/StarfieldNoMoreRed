# Starfield outpost building: how the game works (No More Red reference)

> These notes were written by AI while creating this mod, from decompiling the game. They are kept up to date as development continues.

Target: Starfield 1.16.244 (Steam). Addresses are `0x14xxxxxxx` (image base 0x140000000) and are only for reading
alongside a disassembler. The plugin never uses them directly: everything is found through **Address Library IDs**
(shown as `ID 12345`) and then checked against the game's own instructions at startup.

The code blocks below are hand-written reconstructions of what the decompiled functions do, cleaned up into readable
C++. Names are ours; the game has no symbols. Offsets are into the game's objects.

---

## Contents

1. [Basics: forms, references, handles](#1-basics-forms-references-handles)
2. [Build mode: the workshop engine and its states](#2-build-mode-the-workshop-engine-and-its-states)
3. [Placement checking: the status byte and the codes](#3-placement-checking-the-status-byte-and-the-codes)
4. [Where the held item goes: casts, surfaces and the ground](#4-where-the-held-item-goes-casts-surfaces-and-the-ground)
5. [Object snapping](#5-object-snapping)
6. [Rotation of the held item](#6-rotation-of-the-held-item)
7. [Hold distance](#7-hold-distance)
8. [The build area (workshop boundary)](#8-the-build-area-workshop-boundary)
9. [Auto-foundations](#9-auto-foundations)
10. [Looking at a placed object: StateInspect](#10-looking-at-a-placed-object-stateinspect)
11. [Moving a placed object: Move, the preview and placing](#11-moving-a-placed-object-move-the-preview-and-placing)
12. [Workshop items (the game's wrapper around what you hold)](#12-workshop-items)
13. [Structures: grouped packins](#13-structures-grouped-packins)
14. [Linked references and extra data](#14-linked-references-and-extra-data)
15. [Changing a placed reference: position, rotation, scale](#15-changing-a-placed-reference-position-rotation-scale)
16. [Disable / Enable and why collision needs it](#16-disable--enable-and-why-collision-needs-it)
17. [UI: menus, HUD messages, input](#17-ui-menus-hud-messages-input)
18. [Address Library and how the plugin verifies things](#18-address-library-and-how-the-plugin-verifies-things)

---

## 1. Basics: forms, references, handles

Everything in the game data is a **form** (`TESForm`). A placed object in the world is a **reference**
(`TESObjectREFR`, "ref") that points at a **base form** (what it is: a hab, a bed, a wall).

| Offset | Field |
|---|---|
| +0x08 | reference count (packed; see below) |
| +0x20 | form flags (bit 11, `0x800` = disabled) |
| +0x24 | handle index (0 = no handle created yet) |
| +0x28 | form ID |
| +0x2E | form type (byte) |
| +0x80 | angle (x, y, z radians; z = heading) |
| +0x8C | position (x, y, z) |
| +0x98 | base form |
| +0xB0 | parent cell |
| +0xC8 | extra data list (`ExtraDataList*`) |
| +0xD0 | scale (u16, x 0.01) |

**Handles.** The game rarely keeps raw pointers to other refs. It keeps a 32-bit **handle** and looks it up when it
needs the ref. `LookupRefByHandle` (ID 35638, `0x1402B65E0`) turns a handle into a ref and *adds a reference*; the
caller must release it with ID 38742 (`0x140384600`). The "no ref" handle is a global (ID 944496).

```cpp
// LookupRefByHandle(out, &handle): out = strong reference or null
TESObjectREFR* ref = nullptr;
LookupRefByHandle(&ref, &handle);
if (ref) { /* use it */ RefRelease(ref); }
```

**Reference count.** The 64-bit word at +0x08 packs two counters. An add-reference is `lock add [ref+8], 0x80000200001`
and the release function undoes that. The plugin only ever adds references through game functions and releases them
with the game's release.

**Form by ID.** `LookupFormByID` (ID 47401, `0x1405DE3B0`) returns a raw `TESForm*` from the global form map (no
reference added). It's fine for a quick look at a placed object you know exists.

**Bounds.** A base form that has a size (`TESBoundObject`) stores its local bounding box: min x/y/z at +0x50/+0x54/+0x58,
max x/y/z at +0x5C/+0x60/+0x64 (unscaled, relative to the object's origin).

---

## 2. Build mode: the workshop engine and its states

Build mode is run by a single **workshop engine** object (global, ID 928905, `0x145F5CE40`). It is a state machine:

| Offset | Field |
|---|---|
| +0x490 | current state (pointer to a static state object) |
| +0x4A0 | the "root" workshop struct passed to every state (`RootWorkshopStruct`) |
| +0x1008 / +0x1009 | event filter flag / build mode active |

The states are static objects with a vtable each, at `0x14597A980`..`0x14597A9E8`: CycleSnapBehavior,
CreateReferences, CreatePowerLink, CreateTransferLink, UpdatePowerLink, **DuplicateReferences**, ModifyColors,
InspectColorItems, **Inspect**, **UpdatePlacement**, **UpdateAutoFoundation**, PostCommitEdit, UpdateTransferLink,
SetupReferences.

Changing state ("GoTo") always calls the old state's slot 3 (Exit) and the new state's slot 1 (Enter):

```cpp
void GoTo(Machine* m, State* next) {
    m->current->Exit(m->root);      // vtable slot 3
    m->current = next;
    next->Enter(m->root);           // vtable slot 1
}
```

**Events.** The UI doesn't call the engine directly. It posts small task objects to a `BSService` queue. Each task
holds the engine, an event, and a handler that forwards the event to the *current state* through a fixed vtable
slot:

```cpp
// e.g. the handler for EditItemEvent (0x1421C0090)
void Handle(Task* t) {
    Engine* e = t->engine;
    if (!e->active) return;
    if (e->filterOn && !t->event.Allowed()) return;
    e->current->OnEditItem(&t->event, &e->root);   // state vtable +200 (slot 25)
}
```

So the same event can do different things depending on which state is active, and hooking the state's slot catches
it exactly when the game acts on it.

---

## 3. Placement checking: the status byte and the codes

The root struct holds one byte, the **WorkshopCode** of the current placement attempt (`root+0x193` in 1.16.244).
`2` = VALID. Anything else turns the preview red, shows a message, and the commit states refuse to build:

```cpp
// every commit state (e.g. ID 121268, 121287, 121360)
if (root->status != VALID) return;          // cmp byte ptr [r8+193h], 2
```

The plugin finds the offset by reading that `cmp byte ptr [r8+X], 2` in three commit states and requiring that all three
agree.

**Codes.** `GetWorkshopCodeMessage` (ID 74295) is a switch from code to message GMST
(`sWorkshopPlacement_FLOATING`, ...). The plugin reads its jump table at startup and matches codes by GMST name, so a
renumbered code is followed rather than guessed:

```
cmp eax, N ; ja default
mov ecx, [rdx + rax*4 + TABLE]   ; per-code case offset
case: mov rax, [rip + GMST] ; ret   ; GMST+0x10 = its name string
```

Main codes: 1 NO_ITEM, 2 VALID, 3 FLOATING, 4 INTERSECTING, 5 TERRAIN_ONLY, 6 IN_WATER, 7 NOT_IN_WATER, 8 NAVMESH_ONLY,
10 OUTSIDE_BUILD_AREA, 13 UNSUPPORTED, 14/15 unsupported wall/ceiling, 16/17 NO_SNAPPING(_FLOOR), 18 RADIUS_OVERLAP,
20 OUTSIDE_PLAYABLE_AREA, 21 OVERLAPS_WORKSHOP, 22 RESTRICTED_AREA, 23 MISSING_RESOURCES (extractor not on a vein),
24 MISSING_COMPONENTS, 25 TOO_CLOSE_TO_PLAYER, 26 FOUNDATION_TOO_TALL, 27 ON_ANIMATED_DOOR, 28 INVALID_SURFACE,
29 perk requirement, 30 EXCEEDED_BUDGET, 31 EXCEEDED_MAX_COUNT.

**Who writes it.**

* `EvaluatePlacement` (ID 73783, `0x140F3F9A0`): the core validator. Runs the intersection test, then
  `ValidatePlacement` (ID 73744: build area, playable area, other workshop, restricted area, terrain/water), and
  writes the code. Two callers.
* `UpdatePlacementStatus` (ID 121599, `0x1421D74B0`): runs every frame for the held item. Resets the code to 1, places
  the item, re-evaluates, may set 4 / 26. Two callers (StateUpdatePlacement, StateBulldoze).

```cpp
void UpdatePlacementStatus(Root* root) {
    root->status = NO_ITEM;
    Item* item = root->heldItem;                 // root+0x268
    TESObjectREFR* ref = Lookup(item->MainHandle());
    // yaw: re-read from the item's world rotation (see section 6)
    // position: crosshair cast / follow camera (section 4), snapping (section 5)
    ApplyHeld(&ref, &position, root);            // 0x1421D5820: moves every piece, then EvaluatePlacement
    // foundation check: may set INTERSECTING / FOUNDATION_TOO_TALL and colour the preview red right away
}
```

**How No More Red relaxes codes.** Both callers of each function are redirected to a wrapper that calls the original
and then rewrites a relaxed code to VALID. The foundation check colours the preview red *before* that wrapper runs, so
the plugin also wraps that one highlight call (`mov [r14+status],al; ...; call highlight(root, invalid=1)`) and tells
it "valid" when the code will be relaxed.

---

## 4. Where the held item goes: casts, surfaces and the ground

`SelectPlacementBehavior` (ID 1016851) picks one of two behaviours: [0] follow the ground, [1] follow the camera
(Precise Placement).

* **SurfaceCast** (ID 121584): a shape cast from the crosshair with `ItemPlacementForwardCastCollector`
  (vtable ID 430809). The collector's `AddHit` (vtable slot 4, ID 74019) classifies every hit (ID 74435) and keeps or
  drops it. The cast writes the hit normal's z to `root+0x5A4`, or FLT_MAX on a miss.
* **Drop to ground**: when `fWorkshopDownwardShapeCastMinNormal (0.4) < root+0x5A4`, UpdatePlacementStatus runs
  `DropToGround` (ID 121565), which writes a **ground type** to `root+0x12C`, and then clamps
  `z = ground + offset` for ground types 1, 2 and 6.

```cpp
uint64_t SurfaceCast(Root* root, ...) {
    Collector c(...);                 // AddHit filters hits
    Cast(&c);
    root->hitNormalZ = c.hit ? c.normal.z : FLT_MAX;
    ...
}
```

Surface snapping off, as done by the plugin: while SurfaceCast runs, AddHit drops everything; on a miss the normal z is
set to 0 so the drop pass is skipped. Some items (structures) force the drop pass anyway, so the ground type is cleared
for the rest of that update and put back afterwards, because building the auto-foundation on commit reads it.

---

## 5. Object snapping

`GetSnapCandidates` (ID 121569) fills a vector with the snap points the held item could attach to. An empty vector is a
normal state the game handles ("nothing to snap to"), so returning it empty is a safe way to turn object snapping off.
The snapped flag is a byte in the root (`root+0x18B`).

---

## 6. Rotation of the held item

The held item's yaw is `root+0x184` (radians, same convention as a ref's angle z). Each UpdatePlacementStatus:

1. re-reads the yaw from the item's *current world rotation* (two `atan2` call sites, one stored negated);
2. adds the rotate keys' input (`root+0x70`);
3. lets snapping override it.

```cpp
yaw = -atan2(m.x, m.y);           // or atan2(...), at the second site
root->yaw = yaw;                  // the store at 0x1421D7950
root->yaw += root->rotateInput;   // rotate keys
```

That's why the item stays fixed in the world while you turn. Starfield Engine Fixes removes the store. No More Red
instead hooks the two `atan2` calls and returns its own yaw (with your view turn added when Rotate with view is on), so
game code is unchanged and the rotate keys still work.

---

## 7. Hold distance

`FollowCameraCastLength` (ID 121583) gives the hold distance: the length of the vec3 camera offset at `root+0x4D0` (x
1.02). A new item has a zero offset and uses the INI settings `fWorkshopTempItemDefaultFollowCameraCastLength` (ID
929065) / `...Decorate...` (ID 929068) plus half its bounds. The plugin scales that vec3, or the settings while it is
zero.

---

## 8. The build area (workshop boundary)

Four places test whether something is inside the outpost's build area:

| Where | Test | What happens on "outside" |
|---|---|---|
| ValidatePlacement (73744) | IsInBuildArea (73891) | code 10 |
| surface cast handler (121585) | IsPointInBuildArea (73890) | code 10 **and the held item isn't updated** (it vanishes) |
| follow-camera cast handler (121587) | IsPointInBuildArea | same |
| player timer (121563) | IsInBuildArea(player) | countdown, then build mode exits |

Because two of them skip the item update, relaxing code 10 afterwards isn't enough: the plugin makes those four calls
answer "inside" when the boundary is off. Each call site is found as "the call to the test that is followed by writing
code 10".

---

## 9. Auto-foundations

### Building them

`AutoFoundationHeight` (ID 74058) casts down from the item's foundation points and works out the height to fill.
`FoundationPieceCount` (ID 74057, `0x140F5A050`) turns that height into a piece count (base + middles + top):

```cpp
uint32_t FoundationPieceCount(Data* d, Parts* p, float height, bool exact, void* ctx, float* outHeight) {
    uint32_t n = 0; float built = 0;
    if (!p->middle) goto done;
    if (p->base) { float h = PieceHeight(p->base, ctx); height = max(height - h, 0); built += h; n = 1; }
    if (p->top)  { float h = PieceHeight(p->top, ctx);  height = max(height - h, 0); built += h; n++; }
    if (height > 0) {
        float h = PieceHeight(p->middle, ctx);
        uint32_t m = (uint32_t)(height / h);
        if (!exact && (height / h - m) * h > threshold) m++;   // round up past the threshold
        n += m; built += m * h;
    }
done:
    if (outHeight) *outHeight = built;
    return n;
}

float PieceHeight(TESBoundObject* b, ...) {                 // ID 74056
    if (b->type != PACKIN) return b->boundMax.z - b->boundMin.z;   // +0x64 - +0x58
    ...                                                      // packin: bounds of its cell's refs
}
```

AutoFoundationHeight calls it for the new count (with an out-height) and once for the count already built (out-height
null). Returning 0 for the new count makes the game remove the built pieces and build none, which is how the
Foundations toggle works. It also sets a "foundation too tall" byte (status - 1) from its cast:
`cmp byte [rbp-0x80],0; je; mov byte [r15+0x192],1`. The plugin NOPs that one write while FoundationTooTall is relaxed.

### How a foundation is linked to its object

The foundation pieces are **separate references**, linked to the object they hold up as **linked-ref children with the
foundation keyword** (global, ID 883416). A piece can have its own children, so the game walks them breadth-first
(`0x140F76040`; recursive variant `0x140F7AF80` when `bWorkshopUseRecursiveForEachAutoFoundation`):

```cpp
// WorkshopItem slot 32 (ID 74072): for each auto-foundation piece of this item
void ForEachFoundation(Item* item, Fn fn) {
    queue<Ref*> q = { item->MainRef() };
    while (!q.empty()) {
        Ref* r = q.pop();
        for (auto& child : LinkedChildren(r, kwFoundation)) {    // section 14
            fn(child);
            q.push(child);
        }
    }
}
```

The game builds foundations at scale 1.0 from the object's **unscaled** foundation points, and never resizes them.

---

## 10. Looking at a placed object: StateInspect

In build mode, while you look at a placed object and hold nothing, the engine is in **StateInspect** (vtable ID 467015;
Enter ID 121230, Exit ID 121231). Its target is what it highlights (ID 121244):

```cpp
Handle InspectTarget() {
    Handle h = g_lastTarget;                            // 0x145F5D868
    if (Picker* p = g_picker) {                         // 0x145F5D548 (crosshair picker)
        Handle a = p->AimTarget();                      // vfunc +0x148
        if (a == null) a = p->AltTarget();              // vfunc +0x40
        if (a != null) h = a;
    }
    return h;
}
```

Its event slots that matter here: 13 and 15 (Move for objects with variants), 25 (EditItemEvent = Move).

---

## 11. Moving a placed object: Move, the preview and placing

**Pressing Move.** The UI's Move button (ID 121800):

```cpp
void OnMoveButton() {
    Ref* target = Lookup(g_lastTarget);                 // 0x145F5D868
    if (target && HasVariants(target)) {                // 0x1421F1500
        Post(cycleVariants ? EventA : EventB);          // -> StateInspect slot 13 / 15 (they read root+0x588)
        return;
    }
    Ref* picked = Lookup(picker->AltTarget());
    Post(EditItemEvent{ picked });                      // -> StateInspect slot 25
}
```

**Slot 25** (ID 121241, `0x1421AF270`):

```cpp
void Inspect_OnEditItem(State*, EditItemEvent* ev, Root* root) {
    Ref* ref = ev->ref;                                 // event+8
    if (!ref) return;
    ...
    if (ref->base->type != '<') {
        GoTo(StateDuplicateReferences);                 // 0x14597A9A8: make the held copies
        return;
    }
    ...                                                 // linked/transfer objects: other path
}
```

**The preview is a copy.** StateDuplicateReferences creates fresh copies of the object (and of every piece, for a
structure) at scale 1.0. You hold the copies, not the originals; the originals are hidden as the ghost.

**Placing every frame.** `ApplyHeld` (`0x1421D5820`) ends in `0x140F5E8B0` (ID 74082), which positions every copy from
a list stored in the held item:

```cpp
// item+0x08: u32 count, item+0x10: entries of 0x70 bytes
struct Entry {               // offsets inside one entry
    /* +0x20..+0x5F */ float rows[4][4];   // local transform relative to the held point (translation row at +0x50)
    /* +0x60 */        Handle piece;
};
void PlacePieces(Root* root, float* pos, float yaw, Entries* list) {
    Matrix held = RotZ(yaw) * Translate(pos);
    for (Entry& e : *list) {
        Ref* r = Lookup(e.piece);
        Matrix world = held * e.rows;
        SetPosition(r, world.translation);     // 0x140B4E410
        SetAngles(r, world.rotation);          // never touches scale
    }
}
```

The same list also holds the game's foundation-preview pieces, which it rebuilds as you move.

**Placing it down.** On commit the game keeps the **original** refs (moved to where the copies were, scale intact),
deletes the copies, and builds a **new** auto-foundation for the originals at scale 1.0.

---

## 12. Workshop items

The engine wraps what you hold in a **workshop item** object. Classes seen: `SingleWorkshopItem`,
`PackinWorkshopItem`, `DecoratedHabWorkshopItem`, `BulldozeOnlyWorkshopItem`, `InventoryItemWorkshopItem`,
`WireWorkshopItem`, `PowerLinkWorkshopItem`, `TransferLinkWorkshopItem`. They share a base layout:

| Offset | Field |
|---|---|
| +0x08 | entry list (section 11) |
| +0x44 | item id (a global counter) |
| slot 1 (vfunc +0x08) | span of member handles (Packin: +0x50 count, +0x58 data) |
| slot 8 (vfunc +0x40) | main ref handle (Single: +0x50, Packin: +0xE0) |
| Packin +0xD0 | the packin root ref |

The held item is `root+0x268`.

---

## 13. Structures: grouped packins

A hab or other structure is a **grouped packin**. The crosshair picks its **root**, an invisible marker whose base is
`PackinPivotDummy` (global, ID 894675; form 00015804). The walls, floors and stairs are **separate refs** that don't
follow the root when it moves.

Each piece carries **ExtraGroupedPackin** (extra type `0xDA`, 0x70 bytes):

| Offset | Field |
|---|---|
| +0x40 | the BGSPackIn form |
| +0x58 | the root ref's form ID |
| +0x60 | the piece's template ref ID inside the packin's cell |

**Finding the pieces.** The game does it in `GroupMembers` (ID 73727, `0x140F366C0`):

```cpp
bool GroupMembers(Ref* ref, BSTArray<Ref*>* out, bool excludeSelf) {
    uint32_t rootId = RootID(ref);
    if (!rootId) return false;
    Ref* outpost = GetLinkedRef(ref, kwWorkshopItem);           // ID 913293
    if (!outpost) outpost = GetLinkedRef(ref, kwPackinLink);    // ID 883421
    for (auto& child : LinkedChildren(outpost, kw))             // every object in the outpost
        if (RootID(child) == rootId && !(excludeSelf && child == ref))
            out->push(child);
    return true;
}

uint32_t RootID(Ref* r) {
    if (Ref* l = GetLinkedRef(r, kwPackinLink)) r = l;
    if (r->base == PackinPivotDummy) return r->formID;          // it is the root
    auto* x = GetExtra(r, 0xDA);
    return x ? x->rootFormID : 0;                               // +0x58
}
```

The plugin re-implements it (the game's version allocates its result array with the game's scratch allocator, which
the plugin can't free correctly). The same object can appear twice in the outpost's list, so the plugin removes
duplicates.

**The root ignores scaling.** `SetScale` returns early for some base types (vfunc 0x340 true, unless type 0x09 / 0x38)
and the pivot dummy is one of them, so it always reads 1.0. Markers in a group behave the same. A group's scale is
therefore its pieces' scale, not the root's.

---

## 14. Linked references and extra data

**Extra data.** `ref+0xC8` points at an `ExtraDataList`:

| Offset | Field |
|---|---|
| +0x08 | first extra (linked list through each extra's +0x08) |
| +0x18 | presence bits (one bit per type) |
| +0x20 | read/write lock (`BSReadWriteLock`) |

Each extra has its type at +0x12. `GetExtraData(list, type)` (ID 37878) takes the lock and returns the first match.

**Linked refs.** `GetLinkedRef(list, keyword, bool)` (ID 44922) returns the ref linked with that keyword (a raw pointer,
no reference added).

**Linked-ref children** (the reverse direction: everything that links *to* this ref). This is extra `0x4D`:

| Offset | Field |
|---|---|
| +0x18 | count |
| +0x20 | capacity (negative = stored inline) |
| +0x28 | data pointer, or the inline data |
| entry (16 bytes) | u32 handle, keyword* at +8 |

The game reads it under the list's read lock (`0x140F42DF0`, ID 73806):

```cpp
template <class Fn> void ForEachLinkedChild(ExtraDataList* xl, Fn fn, Keyword* kw) {
    xl->lock.LockRead();                                     // ID 123862 on xl+0x20
    if (xl->presence[0x4D >> 3] & (1 << (0x4D & 7))) {       // byte 9, bit 0x20
        for (Extra* x = xl->first; x; x = x->next)
            if (x->type == 0x4D) { for (auto& e : *(LinkedChildren*)x) if (e.kw == kw) fn(e.handle); break; }
    }
    xl->lock.UnlockRead();                                   // inlined, see below
}

// UnlockRead as the game inlines it
LONG old = InterlockedExchangeAdd(&lock->count, -1);         // xl+0x24
if ((old & 0xFFC00FFF) == 1 && (old & 0x3FF000)) WakeByAddressSingle(&lock->count);
```

The plugin does exactly this. It copies the handles under the lock, releases it, and only then looks the refs up.

---

## 15. Changing a placed reference: position, rotation, scale

**Position and rotation.** `MoveRefrToPosition` (ID 118176, `0x142062F50`) is what Papyrus `SetPosition` (ID 118197)
and `SetAngle` (ID 118200) end in. It updates the model, collision and cell, re-cells the ref if needed, and marks the
change for the save. The natives call it like this:

```cpp
Handle none = g_nullHandle;                                  // a local copy
TESObjectCELL* cell = ref->parentCell;                       // +0xB0
TESWorldSpace* world = (cell && !(cell->flags & INTERIOR))   // cell+0x48 bit 0
                     ? cell->worldSpace : nullptr;           // cell+0x110
MoveRefrToPosition(ref, &none, cell, world, &pos, &angle);
```

**Scale.** `GetScale` (ID 63258) = `ref->scale * 0.01`. `SetScale` is REFR vfunc 0x430 (`0x140B38130`, found from
Papyrus SetScale's task, ID 118446):

```cpp
void SetScale(Ref* r, float s) {
    if (r->base->IgnoresScale() && !(type == 0x09 || type == 0x38 || ...)) return;   // markers, pivot dummies
    uint16_t v = clamp(round(s * 100), 1, 1000);
    if (v != r->scale) r->MarkChanged(0x10);                 // saved
    r->scale = v;
    if (NiAVObject* root3D = r->Get3D()) {
        root3D->localScale = s;                              // +0x7C
        root3D->UpdateWorldData();
    }
    // havok collision is NOT rebuilt here
}
```

---

## 16. Disable / Enable and why collision needs it

Collision shapes are built when the 3D loads, at the scale the ref has then. SetScale only rescales the visible model,
so a scaled object keeps its old collision until its 3D is rebuilt.

* **Disable**: REFR vfunc 0x7E8 (ID 63387, `0x140B4B340`). Sets flag 0x800, marks it changed, disables its
  enable-state children, and unloads the 3D through `0x1418C1AD0` -> vfunc 0x538 (queued if not on the main thread).
* **Enable**: ID 63386 (`0x140B4A9C0`), `Enable(ref, bool)`. Clears flag 0x800 (`btr ecx, 0Bh`), re-enables children,
  and reloads the 3D, rebuilding collision at the current scale. The bool only matters for two base types not used
  by outposts. Papyrus' Enable task (`0x141F15BE0`) calls it with 1.

```cpp
void Disable(Ref* r) {
    if (r->flags & DISABLED) return;
    ...
    r->flags |= DISABLED; r->MarkChanged(1);
    Unload3D(r);                       // 0x1418C1AD0 -> vfunc 0x538
    for (Ref* c : EnableChildren(r)) c->Disable();
}
```

Things learned the hard way:

* **REFR vfunc 0x7F0 (ID 63388) is not Enable.** Calling it left the flag set and the object invisible.
* **Enable straight after Disable doesn't work.** The 3D unload finishes after the Enable, so the object stays gone.
  Waiting about 100 ms in between does work.
* **The console's `enable`** (`0x140C28940`) only updates the console's selection lists. The command itself enables
  the ref through the normal path.

---

## 17. UI: menus, HUD messages, input

* **Is build mode open**: `UI::IsMenuOpen(ui, name)` (ID 130475), UI singleton ID 937580, WorkshopMenu name getter
  ID 130425 (it loads the string "WorkshopMenu", which the plugin checks).
* **HUD message**: `BSTGlobalEvent::EventSource<ShowHUDMessageEvent>` (ID 86800). Event `{ BSFixedString text, sound;
  u16 flags = 1 }`, sent with `Notify(source, functor{ vtbl ID 442192, &event, source })` (ID 123824). Strings via
  BSFixedString ctor / release (IDs 1186742 / 139340).
* **Input**: the game reads keyboard and mouse through raw input, `USER32!GetRawInputData`, a single import in the exe.
  Redirecting that import lets the plugin hide a key or wheel notch from the game. A release is hidden only if its
  press was, so nothing is left stuck.

---

## 18. Address Library and how the plugin verifies things

`versionlib-1-16-244-0.bin` (format 5): header `{ u32 format, u32 version[4], char name[64], u32 pointerSize, u32 reserved,
u32 count }` followed by `u32 offset[count]` indexed by ID. An ID gives an RVA.

An ID only says where something *was* in the version the library was made for, so every one is checked before use:

* **Call sites** are found by scanning `.text` for `E8 rel32` calls to the function, and the caller count is compared
  with 1.16.244's.
* **Offsets** (status byte, yaw, ground type, hold offset, normal z, ...) are read out of the instructions that use them,
  e.g. `cmp byte ptr [r8+X], 2`.
* **Globals** (keywords, the pivot dummy, the picker) are accepted only if the function that uses them in the game
  loads them: `mov r64, [rip+global]` inside that function.
* **Vtable slots** are accepted only if the slot still holds the Address Library function.
* **Hooks** are planned at load and applied after every plugin has loaded. A call another plugin already hooked is
  chained (No More Red calls their hook); one they replaced outright is left alone, and the feature that needed it is
  switched off with a log line.
