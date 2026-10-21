// No More Red - SFSE plugin for Starfield outpost building
// Address Library v2 (format 5); tested on Steam 1.16.244.
//
// Features (each one is set up on its own and skipped, with a log line, if it can't be verified)
//   No More Red      Relax placement failure codes chosen in the INI (Insert)
//   Object snapping  Hide snap points so items don't attach to other pieces (Home)
//   Surface snapping Precise Placement ignores every surface the crosshair hits (End)
//   Foundations      Don't build the auto-foundation pieces under structures (Delete)
//   Workshop boundary Off = build and stay in build mode outside the build area (Page Up)
//   Rotate with view The held item turns with the player, like Fallout 4 (Page Down)
//   Fine rotate      Turn the held item by a small step (Ctrl + left / right mouse button)
//   Hold distance    Move the held item closer / further (Ctrl + mouse wheel)
//                    While Ctrl is held in build mode those buttons / the wheel don't reach the game.
//   Edit placed      The object you're looking at in build mode (not holding anything): numpad
//                    4/6/8/2 nudge it left / right / up / down relative to your view, numpad 1/3
//                    scale it, Ctrl + wheel pushes / pulls it, Ctrl + left / right click rotates it.
//                    No camera follow, snapping or placement checks - it moves exactly by the step.
//                    Structures (grouped packins) and auto-foundations move with it; after scaling,
//                    collision is rebuilt; a scaled object keeps its scale when moved (preview and
//                    placed), and the foundation the game builds for it is sized to match.
//   HUD messages     The game's own top-left notification for every toggle
// Hotkeys only act in build mode (the game's WorkshopMenu is open).
//
// How the core works: build mode keeps one WorkshopCodes byte per placement attempt
// (RootWorkshopStruct+0x193 in 1.16.244, 2 = VALID). Anything else turns the preview red and
// the commit states refuse to build. EvaluatePlacement and UpdatePlacementStatus write that
// byte; their call sites are redirected to wrappers that call the original, then turn relaxed
// codes into VALID. Code numbers and struct offsets are read from the game's own code at
// startup, so a renumbered enum or moved field is followed or detected rather than guessed.

#include <windows.h>
#include <shlobj.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "sfse/PluginAPI.h"

#define MAKE_EXE_VERSION(major, minor, build) ((((major) & 0xFF) << 24) | (((minor) & 0xFF) << 16) | (((build) & 0xFFF) << 4))

extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
    SFSEPluginVersionData::kVersion,
    0x01020000,   // 1.2.0
    "No More Red",
    "DonPetrol",
    SFSEPluginVersionData::kAddressIndependence_AddressLibraryV2,
    SFSEPluginVersionData::kStructureIndependence_1_14_70_Layout,
    { MAKE_EXE_VERSION(1, 16, 244), 0 },
    0,
    0, 0,
};
}

// ================================================================ utilities

static FILE* g_log = nullptr;
static char  g_dir[MAX_PATH] = {};
static bool  g_verbose = false;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list va; va_start(va, fmt);
    vfprintf(g_log, fmt, va);
    va_end(va);
    fputc('\n', g_log);
    fflush(g_log);
}

static uint8_t* ExeBase() { return (uint8_t*)GetModuleHandleA(nullptr); }

struct Range { uint8_t* beg; uint8_t* end; };
static Range g_text{};
static bool InText(const void* p) { return p && (uint8_t*)p >= g_text.beg && (uint8_t*)p < g_text.end; }

static bool FindTextSection()
{
    uint8_t* base = ExeBase();
    auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (!memcmp(sec->Name, ".text", 5)) {
            g_text = { base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize };
            return true;
        }
    return false;
}

// First match of a "48 8B ?? C4" style pattern in [p, p+len). Used for small in-function scans.
static uint8_t* Scan(uint8_t* p, int len, const char* pat)
{
    uint8_t bytes[64]; bool mask[64]; int n = 0;
    for (const char* s = pat; *s && n < 64; ) {
        if (*s == ' ') { ++s; continue; }
        bool fixed = s[0] != '?';
        mask[n] = fixed;
        bytes[n] = fixed ? (uint8_t)strtoul(s, nullptr, 16) : 0;
        ++n;
        s += 2;
    }
    for (uint8_t* q = p; q + n <= p + len; ++q) {
        int k = 0;
        while (k < n && (!mask[k] || q[k] == bytes[k])) ++k;
        if (k == n) return q;
    }
    return nullptr;
}

static uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int32_t  I32(const uint8_t* p) { int32_t v;  memcpy(&v, p, 4); return v; }

// Does the code at [f, f+len) load the global at `target` (`mov r64,[rip+x]`) / call `target` (E8)?
// Used to check Address Library globals against the function that uses them.
static bool LoadsGlobal(const uint8_t* f, int len, const void* target)
{
    for (const uint8_t* q = f; f && target && q + 7 <= f + len; ++q)
        if ((q[0] & 0xFB) == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x05 && q + 7 + I32(q + 3) == target) return true;
    return false;
}
static bool CallsFn(const uint8_t* f, int len, const void* target)
{
    for (const uint8_t* q = f; f && target && q + 5 <= f + len; ++q)
        if (q[0] == 0xE8 && q + 5 + I32(q + 1) == target) return true;
    return false;
}

static void Patch(void* dst, const void* src, size_t n)
{
    DWORD old;
    VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
}

// ---- call-site hooks
// Direct callers (E8 rel32) of every hooked function, found in one pass over .text.
struct CallerSet { uint8_t* target; uint8_t* sites[8]; int count; };
static CallerSet g_callers[8];
static int       g_callerSets = 0;

static void ScanCallers(uint8_t* const* targets, int n)
{
    for (int i = 0; i < n && g_callerSets < 8; ++i)
        if (targets[i]) g_callers[g_callerSets++] = { targets[i], {}, 0 };
    for (uint8_t* s = g_text.beg; s + 5 <= g_text.end; ++s) {
        if (*s != 0xE8) continue;
        uint8_t* t = s + 5 + I32(s + 1);
        for (int i = 0; i < g_callerSets; ++i)
            if (t == g_callers[i].target) {
                if (g_callers[i].count < 8) g_callers[i].sites[g_callers[i].count] = s;
                ++g_callers[i].count;
            }
    }
}

// Stubs (jmp [rip+0]; dq target) within +/-2GB of the exe so a rel32 CALL can reach them.
static uint8_t* g_stubs = nullptr;
static size_t   g_stubsUsed = 0;

static bool AllocStubs()
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity, start = (uintptr_t)g_text.beg & ~(gran - 1);
    for (uintptr_t d = gran; d < 0x70000000 && !g_stubs; d += gran)
        for (int sign = -1; sign <= 1 && !g_stubs; sign += 2)
            g_stubs = (uint8_t*)VirtualAlloc((void*)(start + sign * (intptr_t)d), 0x1000,
                                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    return g_stubs != nullptr;
}

static CallerSet* Callers(uint8_t* target)
{
    for (int i = 0; i < g_callerSets; ++i) if (target && g_callers[i].target == target) return &g_callers[i];
    return nullptr;
}

// True if `target` has 1-8 direct callers and there is stub space for them.
static bool Hookable(uint8_t* target)
{
    CallerSet* c = Callers(target);
    return c && c->count >= 1 && c->count <= 8 && g_stubsUsed + 16 * c->count <= 0x1000;
}

// Call sites are found at plugin load (code still untouched) but only redirected once every
// plugin has loaded (SFSE PostPostLoad). Other plugins that hook the same calls - e.g. Starfield
// Engine Fixes, which refuses to install unless each call still points at the game function -
// get there first, and No More Red then chains to whatever each call points at by that time.
// Each site has its own wrapper instance (template slot N) and its own `next` pointer.
struct PlannedSite { const char* what; uint8_t* site; uint8_t* target; uint8_t* stub; void** next; };
static constexpr int kMaxPlanned = 32;
static PlannedSite g_plan[kMaxPlanned];
static int         g_planned = 0;

// Plan one call site: a stub that jumps to `wrapper`; the wrapper calls *next (the original target
// for now - ApplyCallHooks updates it if another plugin got there first).
static void PlanSite(const char* what, uint8_t* site, void* wrapper, void** next)
{
    uint8_t* stub = g_stubs + g_stubsUsed;
    g_stubsUsed += 16;
    uint8_t jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    memcpy(jmp + 6, &wrapper, 8);
    memcpy(stub, jmp, sizeof(jmp));
    *next = site + 5 + I32(site + 1);
    g_plan[g_planned++] = { what, site, (uint8_t*)*next, stub, next };
}

// Plan redirecting every direct call to `target` into wrappers[k], which call *next[k].
// Returns the number of call sites (0 = not hooked). `expect` is only logged.
static int HookCalls(const char* what, uint8_t* target, void* const* wrappers, void** next, int expect)
{
    CallerSet* cs = Callers(target);
    if (!cs) { Log("%s: function not found", what); return 0; }
    Log("%s: %p, %d caller(s) (1.16.244 had %d)", what, target, cs->count, expect);
    if (!Hookable(target) || g_planned + cs->count > kMaxPlanned) return 0;
    for (int k = 0; k < cs->count; ++k) PlanSite(what, cs->sites[k], wrappers[k], &next[k]);
    return cs->count;
}

// Plan redirecting one specific call site (E8 at `site`) into `wrapper`, which calls *next.
static bool HookOneCall(const char* what, uint8_t* site, void* wrapper, void** next)
{
    if (!InText(site) || site[0] != 0xE8 || g_planned >= kMaxPlanned || g_stubsUsed + 16 > 0x1000) return false;
    PlanSite(what, site, wrapper, next);
    return true;
}

static bool Executable(const void* p)
{
    MEMORY_BASIC_INFORMATION mi;
    return p && VirtualQuery(p, &mi, sizeof(mi)) && mi.State == MEM_COMMIT &&
           (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

// Name of the module behind a call target, following one `jmp [rip+x]` / `jmp rel32` stub.
static const char* OwnerOf(uint8_t* p, char* buf, size_t n)
{
    if (Executable(p) && p[0] == 0xFF && p[1] == 0x25) memcpy(&p, p + 6 + I32(p + 2), 8);
    else if (Executable(p) && p[0] == 0xE9)             p = p + 5 + I32(p + 1);
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)p, &m) || !GetModuleFileNameA(m, buf, (DWORD)n)) return "unknown";
    const char* s = strrchr(buf, '\\');
    return s ? s + 1 : buf;
}

static void ApplyCallHooks()
{
    static bool done = false;
    if (done) return;
    done = true;
    int hooked = 0, chained = 0;
    for (int i = 0; i < g_planned; ++i) {
        PlannedSite& p = g_plan[i];
        uint8_t* cur = p.site[0] == 0xE8 ? p.site + 5 + I32(p.site + 1) : nullptr;
        if (cur != p.target) {
            char name[MAX_PATH];
            if (!cur || !Executable(cur)) {
                Log("%s: call at %p was replaced by another plugin - left alone", p.what, p.site);
                continue;
            }
            Log("%s: call at %p already hooked by %s - chaining to it", p.what, p.site, OwnerOf(cur, name, sizeof(name)));
            ++chained;
        }
        *p.next = cur;
        int32_t rel = (int32_t)(p.stub - (p.site + 5));
        Patch(p.site + 1, &rel, 4);
        ++hooked;
    }
    Log("call hooks applied: %d of %d site(s), %d chained to other plugins", hooked, g_planned, chained);
}

// ================================================================ address library
// Format 5 (game 1.15.216+): u32 format, u32 version[4], char name[64], u32 pointerSize,
// u32 reserved, u32 count, u32 offsets[count] indexed by ID.
namespace ID {
    constexpr uint32_t EvaluatePlacement      = 73783;
    constexpr uint32_t UpdatePlacementStatus  = 121599;
    constexpr uint32_t StatusProbe[]          = { 121268, 121287, 121360 };  // commit states: cmp [r8+status],2
    constexpr uint32_t GetCodeMessage         = 74295;
    constexpr uint32_t GetSnapCandidates      = 121569;
    constexpr uint32_t AutoFoundationHeight   = 74058;
    constexpr uint32_t FoundationCount        = 74057;
    constexpr uint32_t SurfaceCast            = 121584;
    constexpr uint32_t DropToGround           = 121565;
    constexpr uint32_t ForwardCastVtbl        = 430809;
    constexpr uint32_t ForwardCastAddHit      = 74019;
    constexpr uint32_t FollowCamCastLength    = 121583;
    constexpr uint32_t SettingCastLenDefault  = 929065;
    constexpr uint32_t SettingCastLenDecorate = 929068;
    constexpr uint32_t HUDMessageSource       = 86800;
    constexpr uint32_t EventNotify            = 123824;
    constexpr uint32_t FixedStringCtor        = 1186742;
    constexpr uint32_t FixedStringRelease     = 139340;
    constexpr uint32_t HUDNotifyFunctorVtbl   = 442192;
    constexpr uint32_t UISingleton            = 937580;
    constexpr uint32_t IsMenuOpen             = 130475;
    constexpr uint32_t WorkshopMenuName       = 130425;
    constexpr uint32_t PlayerSingleton        = 922868;
    constexpr uint32_t ValidatePlacement      = 73744;
    constexpr uint32_t IsInBuildArea          = 73891;
    constexpr uint32_t IsPointInBuildArea     = 73890;
    constexpr uint32_t PlaceOnSurface         = 121585;   // placement-mode cast handlers that return
    constexpr uint32_t PlaceFollowCamera      = 121587;   // OutsideBuildArea for the crosshair point
    constexpr uint32_t PlayerOutsideTimer     = 121563;   // exits build mode when the player leaves the area
    constexpr uint32_t InspectTarget          = 121244;   // StateInspect: highlight the targeted object
    constexpr uint32_t InspectVtbl            = 467015;   // WorkshopEngine::StateInspect
    constexpr uint32_t InspectEnter           = 121230;   // its Enter (slot 1) / Exit (slot 3)
    constexpr uint32_t InspectExit            = 121231;
    constexpr uint32_t InspectEditItem        = 121241;   // its slot 25: EditItemEvent (Move)
    constexpr uint32_t InspectMoveVariantA    = 121233;   // slot 13 / 15: Move for objects with variants
    constexpr uint32_t InspectMoveVariantB    = 121234;
    constexpr uint32_t LookupRefByHandle      = 35638;
    constexpr uint32_t RefRelease             = 38742;
    constexpr uint32_t MoveRefrToPosition     = 118176;   // what Papyrus SetPosition / SetAngle end in
    constexpr uint32_t GetScale               = 63258;
    constexpr uint32_t SetScaleTask           = 118446;   // Papyrus SetScale's task: calls the SetScale vfunc
    constexpr uint32_t RefrVtbl               = 413258;   // TESObjectREFR
    constexpr uint32_t LookupFormByID         = 47401;
    constexpr uint32_t ScaleAlways[3]         = { 883405, 883406, 883407 };   // forms SetScale always scales
    constexpr uint32_t LockRead               = 123862;   // BSReadWriteLock::LockRead
    constexpr uint32_t ForEachLinkedChild     = 73806;    // walks ExtraLinkedRefChildren under that lock
    constexpr uint32_t RefrDisable            = 63387;    // its vfunc 0x7E8
    constexpr uint32_t RefrEnable             = 63386;    // TESObjectREFR::Enable(ref, bool) - what Papyrus Enable runs
    constexpr uint32_t KwFoundation           = 883416;   // keyword linking an object to its auto-foundation pieces
    constexpr uint32_t ForEachFoundation      = 74072;    // WorkshopItem slot 32, walks them (verifies the keyword)
    constexpr uint32_t GroupMembers           = 73727;    // the game's grouped-packin member search (verified, not called)
    constexpr uint32_t GetExtraData           = 37878;    // ExtraDataList::GetByType(list, type)
    constexpr uint32_t GetLinkedRef           = 44922;    // ExtraDataList linked ref by keyword
    constexpr uint32_t KwWorkshopItem         = 913293;   // WorkshopItemKeyword (form pointer global)
    constexpr uint32_t KwPackinLink           = 883421;   // second keyword GroupMembers follows
    constexpr uint32_t PackinPivotDummy       = 894675;   // the root object of a grouped packin (structures)
}

static uint32_t* g_al = nullptr;
static uint32_t  g_alCount = 0;

static bool LoadAddressLibrary(uint32_t runtime)
{
    unsigned maj = (runtime >> 24) & 0xFF, min = (runtime >> 16) & 0xFF, bld = (runtime >> 4) & 0xFFF;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "Data\\SFSE\\Plugins\\versionlib-%u-%u-%u-0.bin", maj, min, bld);
    FILE* f = fopen(path, "rb");
    if (!f) { Log("address library: cannot open %s", path); return false; }
    struct { uint32_t format, ver[4]; char name[64]; uint32_t ptrSize, reserved, count; } h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.format == 5 &&
              h.ver[0] == maj && h.ver[1] == min && h.ver[2] == bld &&
              (g_al = (uint32_t*)malloc(4ull * h.count)) && fread(g_al, 4, h.count, f) == h.count;
    fclose(f);
    if (!ok) { Log("address library: %s is not a format-5 file for %u.%u.%u", path, maj, min, bld); return false; }
    g_alCount = h.count;
    Log("address library: %s, %u entries", path, g_alCount);
    return true;
}

static uint8_t* Addr(uint32_t id) { return (g_al && id < g_alCount && g_al[id]) ? ExeBase() + g_al[id] : nullptr; }
static uint8_t* Code(uint32_t id) { uint8_t* p = Addr(id); return InText(p) ? p : nullptr; }

// Game INI setting object (+8 value, +0x18 name), checked by name.
static uint8_t* IniSetting(uint32_t id, const char* name)
{
    uint8_t* s = Addr(id);
    const char* n = nullptr;
    if (s) memcpy(&n, s + 0x18, 8);
    return (n && !IsBadReadPtr(n, 64) && !strcmp(n, name)) ? s : nullptr;
}

// ================================================================ state & config

struct Key { int vk; bool down; ULONGLONG since, last; };

static struct {
    volatile LONG enabled = 1, objectSnap = 1, surfaceSnap = 1, foundations = 1, buildAnywhere = 0, rotateWithView = 0;
    bool     showMessages = true;
    uint32_t statusOffset = 0;            // 0 = auto-detect
    float    distStep = 0.25f, distMin = 0.5f, distMax = 50.0f;
    Key      kEnabled{}, kObjectSnap{}, kSurfaceSnap{}, kFoundations{}, kBuildAnywhere{}, kRotate{}, kFineLeft{}, kFineRight{}, kFarther{}, kCloser{};
    float    fineStep = 1.0f;                // degrees per press
    Key      kNudgeLeft{}, kNudgeRight{}, kNudgeUp{}, kNudgeDown{}, kScaleDown{}, kScaleUp{};
    Key      kScaleStepDown{}, kScaleStepUp{}, kMoveStepDown{}, kMoveStepUp{}, kRotStepDown{}, kRotStepUp{};
    Key      kResetRot{}, kResetScale{}, kAxis{};    // kAxis 0x10D = numpad Enter (raw input E0 flag)
    int      axis = 0;                       // edit rotation axis: 0 yaw, 1 pitch, 2 roll (relative to the view)
    float    nudgeStep = 0.05f, scaleStep = 0.05f, scaleMin = 0.1f, scaleMax = 10.0f;
    int      modifier = 0x11;                // held for the distance / fine rotate controls (0 = none)
    bool     wheel = true;                   // modifier + mouse wheel = hold distance
} cfg;

// WorkshopCodes. Numbers are 1.16.244's; at startup they are re-derived from the game's
// code->message switch by GMST name, and a name that isn't found is never relaxed.
struct CodeInfo { uint8_t code; const char* key; bool def; const char* gmst; const char* desc; bool want = false; };
static CodeInfo kCodes[] = {
    {  3, "Floating",              1, "sWorkshopPlacement_FLOATING",                    "Selected item is floating" },
    {  4, "Intersecting",          1, "sWorkshopPlacement_INTERSECTING",                "Intersects with an existing object" },
    {  5, "TerrainOnly",           1, "sWorkshopPlacement_TERRAIN_ONLY",                "Must be placed directly on terrain" },
    {  6, "InWater",               1, "sWorkshopPlacement_IN_WATER",                    "Must not be placed in water" },
    {  7, "NotInWater",            1, "sWorkshopPlacement_NOT_IN_WATER",                "Must be placed in water" },
    {  8, "NavmeshOnly",           1, "sWorkshopPlacement_NAVMESH_ONLY",                "Invalid location for turrets" },
    { 10, "OutsideBuildArea",      0, "sWorkshopPlacement_OUTSIDE_BUILD_AREA",          "Outside the build area" },
    { 13, "Unsupported",           1, "sWorkshopPlacement_UNSUPPORTED",                 "Needs support" },
    { 14, "UnsupportedWall",       1, "sWorkshopPlacement_UNSUPPORTED_FORWARD_CAST",    "Must be built on walls" },
    { 15, "UnsupportedCeiling",    1, "sWorkshopPlacement_UNSUPPORTED_CLAMP_UP",        "Must be built on ceilings" },
    { 16, "NoSnapping",            1, "sWorkshopPlacement_NO_SNAPPING",                 "Must be snapped to an existing piece" },
    { 17, "NoSnappingFloor",       1, "sWorkshopPlacement_NO_SNAPPING_FLOOR",           "Must be snapped (floor)" },
    { 18, "RadiusOverlap",         1, "sWorkshopPlacement_RADIUS_OVERLAP",              "Radius overlaps an existing object" },
    { 20, "OutsidePlayableArea",   0, "sWorkshopPlacement_OUTSIDE_PLAYABLE_AREA",       "Outside playable area (risky)" },
    { 21, "OverlapsOtherWorkshop", 0, "sWorkshopPlacement_PLACEMENT_OVERLAPS_WORKSHOP", "Overlaps an existing build area" },
    { 22, "RestrictedArea",        1, "sWorkshopPlacement_PLACEMENT_RESTRICTED_AREA",   "Area is restricted" },
    { 23, "NotOnResourceVein",     1, "sWorkshopPlacement_PLACEMENT_MISSING_RESOURCES", "Extractor not on a resource vein" },
    { 24, "MissingComponents",     0, "sWorkshopPlacement_PLACEMENT_MISSING_COMPONENTS","Missing components (cheat)" },
    { 25, "TooCloseToPlayer",      1, "sWorkshopPlacement_TOO_CLOSE_TO_PLAYER",         "Overlapping player" },
    { 26, "FoundationTooTall",     1, "sWorkshopPlacement_FOUNDATION_TOO_TALL",         "Foundation is too tall" },
    { 27, "OnAnimatedDoor",        1, "sWorkshopPlacement_ON_ANIMATED_DOOR",            "Cannot be placed on movable doors" },
    { 28, "InvalidSurface",        1, "sWorkshopPlacement_INVALID_SURFACE",             "Cannot place on this surface" },
    { 29, "MissingAttributes",     0, "sBuildRules_PERK_REQUIREMENT_NOT_MET",           "Perk requirement not met (cheat)" },
    { 30, "ExceededBudget",        0, "sWorkshop_EXCEEDED_BUDGET",                      "Workshop budget exceeded" },
    { 31, "ExceededMaxCount",      0, "sWorkshop_EXCEEDED_MAX_COUNT",                   "Max amount of this item reached" },
};
static constexpr int kNoSnap = 10, kNoSnapFloor = 11, kFoundationTooTall = 19;   // indices into kCodes
static constexpr int kOutsideBuildArea = 6;   // index into kCodes: driven by the WORKSHOP BOUNDARY toggle
static uint8_t g_valid = 2;
static bool    g_relax[256] = {};

static void LoadConfig()
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%sNoMoreRed.ini", g_dir);
    auto I = [&](const char* sec, const char* key, int def) { return (int)GetPrivateProfileIntA(sec, key, def, ini); };
    auto F = [&](const char* sec, const char* key, const char* def) {
        char b[32]; GetPrivateProfileStringA(sec, key, def, b, sizeof(b), ini); return (float)atof(b);
    };
    cfg.enabled      = I("General", "Enabled", 1) != 0;
    cfg.showMessages = I("General", "ShowMessages", 1) != 0;
    g_verbose        = I("General", "VerboseLog", 0) != 0;
    cfg.objectSnap   = I("Snapping", "ObjectSnapping", 1) != 0;
    cfg.surfaceSnap  = I("Snapping", "SurfaceSnapping", 1) != 0;
    cfg.foundations  = I("Foundations", "Foundations", 1) != 0;
    cfg.buildAnywhere = I("WorkshopBoundary", "WorkshopBoundary", 0) == 0;   // boundary off = build anywhere
    cfg.rotateWithView = I("Rotation", "RotateWithView", 0) != 0;
    cfg.kEnabled.vk     = I("General", "ToggleKey", 0x2D);              // Insert
    cfg.kObjectSnap.vk  = I("Snapping", "ObjectSnapToggleKey", 0x24);   // Home
    cfg.kSurfaceSnap.vk = I("Snapping", "SurfaceSnapToggleKey", 0x23);  // End
    cfg.kFoundations.vk = I("Foundations", "ToggleKey", 0x2E);          // Delete
    cfg.kBuildAnywhere.vk = I("WorkshopBoundary", "ToggleKey", 0x21);   // Page Up
    cfg.modifier        = I("General", "ModifierKey", 0x11);            // Ctrl
    cfg.kRotate.vk      = I("Rotation", "ToggleKey", 0x22);             // Page Down
    cfg.kFineLeft.vk    = I("Rotation", "FineRotateLeftKey", 0x01);     // left mouse button
    cfg.kFineRight.vk   = I("Rotation", "FineRotateRightKey", 0x02);    // right mouse button
    cfg.fineStep        = F("Rotation", "FineRotateStep", "1.0");
    cfg.kNudgeLeft.vk   = I("EditPlaced", "LeftKey", 0x64);             // numpad 4
    cfg.kNudgeRight.vk  = I("EditPlaced", "RightKey", 0x66);            // numpad 6
    cfg.kNudgeUp.vk     = I("EditPlaced", "UpKey", 0x68);               // numpad 8
    cfg.kNudgeDown.vk   = I("EditPlaced", "DownKey", 0x62);             // numpad 2
    cfg.kScaleDown.vk   = I("EditPlaced", "ScaleDownKey", 0x61);        // numpad 1
    cfg.kScaleUp.vk     = I("EditPlaced", "ScaleUpKey", 0x63);          // numpad 3
    cfg.kScaleStepDown.vk = I("EditPlaced", "ScaleStepDownKey", 0x67);  // numpad 7
    cfg.kScaleStepUp.vk   = I("EditPlaced", "ScaleStepUpKey", 0x69);    // numpad 9
    cfg.kMoveStepDown.vk  = I("EditPlaced", "MoveStepDownKey", 0x6D);   // numpad -
    cfg.kMoveStepUp.vk    = I("EditPlaced", "MoveStepUpKey", 0x6B);     // numpad +
    cfg.kRotStepDown.vk   = I("EditPlaced", "RotateStepDownKey", 0x6F); // numpad /
    cfg.kRotStepUp.vk     = I("EditPlaced", "RotateStepUpKey", 0x6A);   // numpad *
    cfg.kResetRot.vk      = I("EditPlaced", "ResetRotationKey", 0x60);  // numpad 0
    cfg.kResetScale.vk    = I("EditPlaced", "ResetScaleKey", 0x6E);     // numpad .
    cfg.kAxis.vk          = I("EditPlaced", "RotateAxisKey", 0x10D);    // numpad Enter
    cfg.nudgeStep       = F("EditPlaced", "MoveStep", "0.05");
    cfg.scaleStep       = F("EditPlaced", "ScaleStep", "0.05");
    cfg.scaleMin        = F("EditPlaced", "ScaleMin", "0.1");
    cfg.scaleMax        = F("EditPlaced", "ScaleMax", "10");
    cfg.wheel           = I("Distance", "MouseWheel", 1) != 0;
    cfg.kFarther.vk     = I("Distance", "FartherKey", 0);               // optional keys (0 = none)
    cfg.kCloser.vk      = I("Distance", "CloserKey", 0);
    cfg.distStep = F("Distance", "Step", "0.25");
    cfg.distMin  = F("Distance", "Min", "0.5");
    cfg.distMax  = F("Distance", "Max", "50");
    cfg.statusOffset = I("Advanced", "StatusOffset", 0);
    for (auto& c : kCodes) c.want = I("Relax", c.key, c.def) != 0;
    Log("config: enabled %d, object snap %d, surface snap %d, foundations %d, workshop boundary %d, rotate with view %d, "
        "messages %d, verbose %d", (int)cfg.enabled, (int)cfg.objectSnap, (int)cfg.surfaceSnap, (int)cfg.foundations,
        (int)!cfg.buildAnywhere, (int)cfg.rotateWithView, cfg.showMessages, g_verbose);
    Log("config: modifier 0x%X, wheel %d, fine rotate %.2f deg, distance step %.2f (%.2f-%.2f), edit move %.3f, "
        "scale step %.3f (%.2f-%.2f)", cfg.modifier, cfg.wheel, cfg.fineStep, cfg.distStep, cfg.distMin, cfg.distMax,
        cfg.nudgeStep, cfg.scaleStep, cfg.scaleMin, cfg.scaleMax);
}

// ================================================================ HUD messages
// The game's top-left notification: BSTGlobalEvent::EventSource<ShowHUDMessageEvent>.
// Event layout copied from a vanilla caller (outpost beacon error, 1.16.244 @ 0x1415F6C01).
struct BSFixedString { void* data; };
struct ShowHUDMessageEvent { BSFixedString text, sound; uint16_t flags; };
struct NotifyFunctor { void* vtbl; ShowHUDMessageEvent* ev; void* source; };

static struct {
    void* (*source)() = nullptr;
    void  (*notify)(void*, NotifyFunctor*) = nullptr;
    void  (*make)(BSFixedString*, const char*, bool) = nullptr;
    void  (*release)(BSFixedString*) = nullptr;
    void*  functorVtbl = nullptr;
} hud;

static void ShowHUD(const char* text, bool log = true)
{
    if (log || g_verbose) Log("%s", text);
    if (!cfg.showMessages || !hud.source) return;
    ShowHUDMessageEvent ev{};
    hud.make(&ev.text, text, true);
    ev.flags = 1;
    void* src = hud.source();
    NotifyFunctor fn{ hud.functorVtbl, &ev, src };
    hud.notify(src, &fn);
    hud.release(&ev.sound);
    hud.release(&ev.text);
}

// ================================================================ No More Red
// Call-site wrappers around EvaluatePlacement / UpdatePlacementStatus: run the original (or the
// hook another plugin already put on that call), then turn relaxed codes into VALID. With object
// snapping off, "must be snapped" is relaxed too, since there is nothing left to snap to.
// Every wrapper is a template on its call-site slot N so each site calls its own `next`.
#define SITE_WRAPPERS(fn) { (void*)&fn<0>, (void*)&fn<1>, (void*)&fn<2>, (void*)&fn<3>, \
                            (void*)&fn<4>, (void*)&fn<5>, (void*)&fn<6>, (void*)&fn<7> }
static char (*g_EvaluatePlacement[8])(void*, void*, void*, void*) = {};
static void (*g_UpdatePlacementStatus[8])(void*) = {};
static bool g_pollByTask = false;
static void PollKeys();
// The held item: UpdatePlacementStatus runs for it every frame (distance / fine rotate use this).
static void*     g_heldRoot = nullptr;
static ULONGLONG g_heldTick = 0;
static bool Holding() { return g_heldRoot && GetTickCount64() - g_heldTick <= 250; }

static bool WouldRelax(uint8_t code)
{
    if (!cfg.enabled || code == g_valid) return false;
    if (code == 0xFF) return false;
    return (code == kCodes[kOutsideBuildArea].code ? cfg.buildAnywhere != 0 : g_relax[code]) ||
           (!cfg.objectSnap && (code == kCodes[kNoSnap].code || code == kCodes[kNoSnapFloor].code));
}

static bool Relax(void* root, int site)
{
    uint8_t& code = *((uint8_t*)root + cfg.statusOffset);
    bool relax = WouldRelax(code);
    if (g_verbose) {
        static uint8_t last[2] = { 0xFF, 0xFF };
        if (code != last[site]) {
            Log("[%s] code %u%s", site ? "Update" : "Evaluate", code, relax ? " -> relaxed" : "");
            last[site] = code;
        }
    }
    if (relax) code = g_valid;
    return relax;
}

// The foundation check in UpdatePlacementStatus sets Intersecting / FoundationTooTall and colours
// the preview red straight away (highlight(root, invalid=1)), before Relax runs - so a relaxed
// structure still showed red. That one call is told "valid" when the code will be relaxed.
static void (*g_Highlight)(void*, void*, char, char) = nullptr;

static void Hook_Highlight(void* a1, void* root, char invalid, char a4)
{
    if (invalid && root && WouldRelax(*((uint8_t*)root + cfg.statusOffset))) invalid = 0;
    g_Highlight(a1, root, invalid, a4);
}

// WORKSHOP BOUNDARY off: three checks return OutsideBuildArea and skip the rest of their work -
// ValidatePlacement (item), and the surface / follow-camera cast handlers (crosshair point; on
// failure UpdatePlacementStatus skips updating the held item, which then vanishes). Relaxing the
// code afterwards can't undo that, so those calls to the build-area test answer "inside" instead.
// The fourth is the player check behind the "outside the build area" countdown that exits build mode.
static char (*g_InBuildArea[4])(void*, void*, void*, void*) = {};

template <int N> static char Hook_InBuildArea(void* ws, void* item, void* area, void* a4)
{
    char r = g_InBuildArea[N](ws, item, area, a4);
    return (!r && cfg.enabled && cfg.buildAnywhere && kCodes[kOutsideBuildArea].code != 0xFF) ? 1 : r;
}

template <int N> static char Hook_EvaluatePlacement(void* root, void* a2, void* a3, void* a4)
{
    if (!g_pollByTask) PollKeys();
    char ok = g_EvaluatePlacement[N](root, a2, a3, a4);
    return (root && Relax(root, 0)) ? 1 : ok;
}

// ================================================================ rotate with view
// The held item's yaw is root+0x184 (radians, same convention as a ref's angle z). At the start of
// each UpdatePlacementStatus the game re-reads it from the item's current world rotation
// (yaw = -atan2(...) or atan2(...) at two call sites, then stored); later in the update the rotate
// keys add their input and snapping can override it. So the item stays fixed in the world when the
// player turns. Rotate with view keeps its own yaw for the held item: before each update it adds the
// player's turn and makes the two re-reads return it; after the update it takes the game's result
// (rotate keys, snapping) as its new own yaw. So the item keeps its angle relative to the view, like
// Fallout 4. Starfield Engine Fixes gets the same effect by removing the re-read store; here no game
// code is changed, the toggle works live and the rotate keys keep working. Snapped items are left alone.
static uint8_t** g_player = nullptr;   // PlayerCharacter*; heading (angle z, radians) at +0x88
static uint32_t  g_yawOffset = 0;
static uint32_t  g_snappedOffset = 0;  // byte: item is snapped to another piece
static uint8_t*  g_yawStore = nullptr; // the game's yaw re-read store, checked for other plugins
static uint8_t   g_yawStoreOrig[13];
static float     (*g_yawRead[2])(float, float) = {};   // the two re-read atan2 calls
static float     g_ownYaw = 0;
static bool      g_rotReady = false;   // re-reads hooked and nothing else changes the yaw code
static DWORD     g_rotThread = 0;      // set while an update with an active override runs
static constexpr float kPi = 3.14159265f;

// Wrap to [-pi, pi]. Bounded for any input (no loops); non-finite values become 0.
static float WrapPi(float a)
{
    if (!std::isfinite(a)) return 0.0f;
    a = remainderf(a, 2 * kPi);
    return a > kPi ? a - 2 * kPi : a < -kPi ? a + 2 * kPi : a;
}

// The re-read value is the item's world rotation. It isn't updated with our yaw, only by the rotate
// keys, so a change in it is a key turn: add that change to our yaw (adopting the value itself
// would jump back to the old world angle first).
static bool  g_haveNatural = false;   // cleared for a new item
static float OwnYaw(float natural)
{
    static float lastNatural = 0;
    if (!std::isfinite(natural)) return g_ownYaw;
    if (!g_haveNatural) { g_ownYaw = natural; g_haveNatural = true; }
    else g_ownYaw = WrapPi(g_ownYaw + WrapPi(natural - lastNatural));
    lastNatural = natural;
    return g_ownYaw;
}
static float Hook_YawReadNeg(float a, float b)   // stored negated: yaw = -ret
{
    float r = g_yawRead[0](a, b);
    return g_rotThread == GetCurrentThreadId() ? -OwnYaw(WrapPi(-r)) : r;
}
static float Hook_YawRead(float a, float b)      // stored as is: yaw = ret
{
    float r = g_yawRead[1](a, b);
    return g_rotThread == GetCurrentThreadId() ? OwnYaw(WrapPi(r)) : r;
}

// Before the update: true if this update gets the override. The held item keeps our yaw whether or
// not rotate with view is on (so its angle persists when the toggle changes); only the player's
// turn is added when it is on.
static bool RotateBefore(void* root)
{
    static void* lastRoot = nullptr;
    static ULONGLONG lastTick = 0;
    static float heading = 0;
    uint8_t* player = g_player ? *g_player : nullptr;
    if (!root || !player || !g_yawOffset) return false;
    float h = *(float*)(player + 0x88);
    if (!(h > -10.0f && h < 10.0f)) return false;   // not a heading
    ULONGLONG now = GetTickCount64();
    bool fresh = root != lastRoot || now - lastTick > 250;   // new item / first update after a gap
    bool snapped = g_snappedOffset && *((uint8_t*)root + g_snappedOffset);
    float turn = WrapPi(h - heading);
    heading = h; lastRoot = root; lastTick = now;
    if (fresh) g_haveNatural = false;
    if (fresh || snapped || !g_rotReady) return false;
    if (cfg.rotateWithView) g_ownYaw = WrapPi(g_ownYaw + turn);
    *(float*)((uint8_t*)root + g_yawOffset) = g_ownYaw;
    return true;
}

static void RestoreGroundType(void* root);   // surface snapping
static void KeepMoveScale(void* root);       // edit placed: the held copy of a scaled object

template <int N> static void Hook_UpdatePlacementStatus(void* root)
{
    if (root) { g_heldRoot = root; g_heldTick = GetTickCount64(); }
    bool rotate = RotateBefore(root);
    if (rotate) g_rotThread = GetCurrentThreadId();
    g_UpdatePlacementStatus[N](root);
    g_rotThread = 0;
    RestoreGroundType(root);
    if (root) {
        KeepMoveScale(root);
        Relax(root, 1);
        if (g_yawOffset) g_ownYaw = *(float*)((uint8_t*)root + g_yawOffset);   // keys / snapping / fresh item
    }
}

// ================================================================ object snapping
// GetSnapCandidates builds the list of snap points the held item can attach to; an empty list
// is a normal state the game handles.
static int64_t* (*g_GetSnapCandidates[8])(int64_t*, void*, void*, void*) = {};

template <int N> static int64_t* Hook_GetSnapCandidates(int64_t* out, void* root, void* a3, void* a4)
{
    if (!cfg.objectSnap) { out[0] = out[1] = out[2] = 0; return out; }
    return g_GetSnapCandidates[N](out, root, a3, a4);
}

// ================================================================ foundations
// FoundationCount turns a foundation height into the number of pieces (base + middles + top).
// AutoFoundationHeight calls it for the new count (with an out-height) and once for the count
// already built (out-height null); returning 0 for the new count makes the game remove the
// built pieces and build none.
// AutoFoundationHeight also sets the "foundation too tall" flag (status-1) from its height
// cast, and the game colours the preview red from it before our wrappers run, so that one
// flag write is NOP'd while No More Red relaxes FoundationTooTall or foundations are off.
static uint32_t (*g_FoundationCount[8])(void*, void*, float, char, void*, float*) = {};
static uint8_t* g_tooTallWrite = nullptr;
static uint8_t  g_tooTallOrig[8];
static bool     g_tooTallNopped = false;

template <int N>
static uint32_t Hook_FoundationCount(void* data, void* parts, float height, char flag, void* a5, float* outHeight)
{
    if (!cfg.foundations && outHeight) { *outHeight = 0.0f; return 0; }
    return g_FoundationCount[N](data, parts, height, flag, a5, outHeight);
}

static void UpdateTooTallPatch()
{
    bool want = (cfg.enabled && g_relax[kCodes[kFoundationTooTall].code]) || !cfg.foundations;
    if (!g_tooTallWrite || want == g_tooTallNopped) return;
    static const uint8_t nop8[8] = { 0x0F, 0x1F, 0x84, 0, 0, 0, 0, 0 };
    Patch(g_tooTallWrite, want ? nop8 : g_tooTallOrig, 8);
    g_tooTallNopped = want;
}

// ================================================================ surface snapping
// Precise Placement holds the item in front of the camera but stops it at whatever the
// crosshair cast (SurfaceCast, ItemPlacementForwardCastCollector) hits. Surface snapping off:
// the collector's AddHit drops every hit while SurfaceCast runs, so the item stays at its hold
// distance; SurfaceCast still returns 0 (no placement error), and the drop-to-ground pass is then
// skipped by storing hit normal.z = 0 instead of FLT_MAX. Some items (structures) force the drop pass anyway, so
// after it runs the ground type it stored is cleared; UpdatePlacementStatus only clamps the
// item's height to the ground for ground types 1/2/6.
static uint64_t (*g_SurfaceCast[8])(void*, void*) = {};
static void     (*g_AddHit)(void*, void*) = nullptr;
static uint8_t** g_addHitSlot = nullptr;
static uint32_t  g_normalZOffset = 0;
static DWORD     g_castThread = 0;
static void     (*g_DropToGround[8])(void*, void*, void*) = {};
static uint32_t  g_groundTypeOffset = 0;

static void Hook_AddHit(void* collector, void* hit)
{
    if (!cfg.surfaceSnap && g_castThread == GetCurrentThreadId()) return;
    g_AddHit(collector, hit);
}

template <int N> static uint64_t Hook_SurfaceCast(void* root, void* a2)
{
    g_castThread = GetCurrentThreadId();
    uint64_t hit = g_SurfaceCast[N](root, a2);
    g_castThread = 0;
    if (!cfg.surfaceSnap && root && (hit & 0xFF) == 0)
        *(float*)((uint8_t*)root + g_normalZOffset) = 0.0f;
    return hit;
}

// The ground type is cleared only for the rest of UpdatePlacementStatus (where the height clamp
// reads it) and put back afterwards: building the auto-foundation on commit needs it.
static void*   g_groundRoot = nullptr;
static uint8_t g_groundSaved = 0;

static void RestoreGroundType(void* root)
{
    if (root && root == g_groundRoot) *((uint8_t*)root + g_groundTypeOffset) = g_groundSaved;
    g_groundRoot = nullptr;
}

template <int N> static void Hook_DropToGround(void* root, void* a2, void* a3)
{
    g_DropToGround[N](root, a2, a3);   // also releases the refs passed in a2/a3, so always call it
    if (!cfg.surfaceSnap && root) {
        uint8_t& type = *((uint8_t*)root + g_groundTypeOffset);
        if (type) { g_groundRoot = root; g_groundSaved = type; }
        type = 0;
    }
}

// ================================================================ hold distance
// The held item's camera offset is a vec3 (root+0x4D0 in 1.16.244); its length is the hold
// distance. New items have a zero offset and use the cast length settings instead
// (fWorkshopTempItemDefault/DecorateFollowCameraCastLength + half the item's bounds).
static uint32_t g_holdOffset = 0;
static float*   g_castLen[2] = {};

static float Clamp(float v) { return v < cfg.distMin ? cfg.distMin : v > cfg.distMax ? cfg.distMax : v; }

static void AdjustHoldDistance(float dir)
{
    if (!g_holdOffset || !Holding()) return;
    float* v = (float*)((uint8_t*)g_heldRoot + g_holdOffset);
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len >= 0.01f) {
        float k = Clamp(len + dir * cfg.distStep) / len;
        v[0] *= k; v[1] *= k; v[2] *= k;
        if (g_verbose) Log("hold distance %.2f -> %.2f", len, len * k);
        return;
    }
    for (float* s : g_castLen) {
        if (!s) continue;
        float to = Clamp(*s + dir * cfg.distStep);
        if (g_verbose) Log("hold distance setting %.2f -> %.2f", *s, to);
        *s = to;
    }
}

// Fine rotate: turn the held item's own yaw by a step; the next update applies it. Not for pieces
// snapped to another piece (the game sets their yaw from the snap point).
static void FineRotate(float dir)
{
    if (!g_rotReady || !Holding()) return;
    if (g_snappedOffset && *((uint8_t*)g_heldRoot + g_snappedOffset)) return;
    g_ownYaw = WrapPi(g_ownYaw + dir * cfg.fineStep * (kPi / 180.0f));
}

// ================================================================ edit placed objects
// In build mode the game is in StateInspect while you look at a placed object (its Enter / Exit are
// hooked to know that). The object is the one StateInspect highlights: the crosshair picker's
// target (vfunc +0x148, else +0x40), else the workshop's last target handle. It is changed in place
// with the engine's own MoveRefrToPosition (as Papyrus SetPosition / SetAngle do: model, collision
// and cell are updated and the change is saved) and the ref's SetScale vfunc (as Papyrus SetScale).
static volatile bool g_inspecting = false;
static uint64_t (*g_InspectEnter)(void*, void*, void*, void*) = nullptr;
static uint64_t (*g_InspectExit)(void*, void*, void*, void*) = nullptr;
static void**    g_inspectSlots = nullptr;   // StateInspect vtable (slots 1 and 3 patched in ApplyDeferred)

static uint64_t Hook_InspectEnter(void* s, void* a, void* b, void* c) { uint64_t r = g_InspectEnter(s, a, b, c); g_inspecting = true; return r; }
static uint64_t Hook_InspectExit(void* s, void* a, void* b, void* c)  { g_inspecting = false; return g_InspectExit(s, a, b, c); }
// The UI's Move (ID 121800) posts one of three events, each handled by a StateInspect slot with
// (state, event, workshop root): 25 = EditItemEvent {vtbl, ref}; 13 / 15 = objects with variants,
// which read the target handle from root+0x588.
static uint64_t (*g_InspectMove[3])(void*, void*, void*, void*) = {};
static void NoteMoveSource(int slot, uint8_t* ref, uint32_t handle);
static const int kMoveSlots[3] = { 13, 15, 25 };
template <int N> static uint64_t Hook_InspectMove(void* s, void* ev, void* root, void* c)
{
    uint8_t* ref = (N == 2 && ev) ? *(uint8_t**)((uint8_t*)ev + 8) : nullptr;
    uint32_t h = (N != 2 && root) ? *(uint32_t*)((uint8_t*)root + 0x588) : 0;
    NoteMoveSource(kMoveSlots[N], ref, h);
    return g_InspectMove[N](s, ev, root, c);
}

static struct {
    void**    picker = nullptr;        // crosshair picker object (pointer global)
    uint32_t  vfAim = 0, vfAlt = 0;    // its two target-handle vfuncs
    uint32_t* lastTarget = nullptr;    // fallback handle
    uint32_t* nullHandle = nullptr;
    void  (*lookup)(void**, const uint32_t*) = nullptr;
    void  (*release)(void*) = nullptr;
    void  (*move)(void*, const uint32_t*, void*, void*, const float*, const float*) = nullptr;
    float (*getScale)(void*) = nullptr;
    uint32_t vfSetScale = 0;
    uint32_t vfDisable = 0;                 // TESObjectREFR::Disable slot (0 = no collision rebuild)
    void   (*enable)(void*, bool) = nullptr;
    uint8_t* (*byId)(uint32_t) = nullptr;    // LookupFormByID (raw TESForm*)
    void**   scaleAlways[3] = {};            // SetScale's exceptions to "this base ignores scale"
    bool     scaleRule = false;              // the rule below was verified against SetScale
    void   (*lockRead)(void*) = nullptr;     // BSReadWriteLock::LockRead (the extra-data list lock, list+0x20)
    void   (WINAPI* wake)(void*) = nullptr;  // WakeByAddressSingle (UnlockRead is inlined in the game)
    // grouped packins (structures)
    uint8_t* (*getExtra)(void*, uint32_t) = nullptr;
    uint8_t* (*linked)(void*, void*, bool) = nullptr;
    void**   kwWorkshop = nullptr;
    void**   kwLink = nullptr;
    void**   pivotDummy = nullptr;
    void**   kwFoundation = nullptr;   // null = foundations don't follow
    bool     groups = false;
} g_edit;

static bool EditReady() { return g_edit.picker && g_edit.lookup && g_edit.release && g_edit.move && g_inspectSlots; }

// The hovered object (a strong reference; release it), or null.
static uint8_t* HoveredRef()
{
    uint32_t h = *g_edit.lastTarget, tmp = 0;
    if (void* p = *g_edit.picker) {
        using Get = uint32_t* (*)(void*, uint32_t*);
        void** vt = *(void***)p;
        uint32_t aim = *((Get)vt[g_edit.vfAim / 8])(p, &tmp);
        uint32_t alt = aim != *g_edit.nullHandle ? aim : *((Get)vt[g_edit.vfAlt / 8])(p, &tmp);
        if (alt != *g_edit.nullHandle) h = alt;
    }
    if (!h || h == *g_edit.nullHandle) return nullptr;
    void* ref = nullptr;
    g_edit.lookup(&ref, &h);
    return (uint8_t*)ref;
}

// Handles of a ref's linked-ref children with keyword `kw` (ExtraLinkedRefChildren, extra 0x4D:
// +0x18 count, +0x20 capacity (< 0 = stored inline), +0x28 data; entries {u32 handle, keyword* @+8}).
// Read under the extra-data list's read lock exactly as the game's walkers do (0xF42DF0 / 0xF79CF0);
// the refs are looked up afterwards, outside the lock.
static int LinkedChildren(uint8_t* ref, void* kw, uint32_t* out, int max)
{
    uint8_t* xl = ref ? *(uint8_t**)(ref + 0xC8) : nullptr;
    if (!xl || !kw || !g_edit.lockRead || !g_edit.wake) return 0;
    g_edit.lockRead(xl + 0x20);
    int n = 0;
    uint8_t* bits = *(uint8_t**)(xl + 0x18);
    if (bits && (bits[0x4D >> 3] & (1 << (0x4D & 7)))) {   // the presence bit the game checks first
        for (uint8_t* x = *(uint8_t**)(xl + 8); x; x = *(uint8_t**)(x + 8)) {
            if (x[0x12] != 0x4D) continue;
            uint32_t count = *(uint32_t*)(x + 0x18);
            uint8_t* e = *(int32_t*)(x + 0x20) >= 0 ? *(uint8_t**)(x + 0x28) : x + 0x28;
            for (uint32_t k = 0; e && k < count && n < max; ++k, e += 16)
                if (*(void**)(e + 8) == kw) out[n++] = *(uint32_t*)e;
            break;
        }
    }
    volatile LONG* c = (volatile LONG*)(xl + 0x24);   // UnlockRead, as inlined by the game
    LONG old = InterlockedExchangeAdd(c, -1);
    if ((old & 0xffc00fff) == 1 && (old & 0x3ff000)) g_edit.wake((void*)c);
    return n;
}

// ---- groups: a structure (and some multi-part furniture) is a grouped packin - a root object
// (PackinPivotDummy, what the crosshair picks) plus separate pieces that don't follow it. The game
// finds the pieces in GroupMembers (ID 73727): every object linked to the outpost (WorkshopItem
// keyword, via the outpost's linked-ref children extra) whose group root ID matches - the root's own
// form ID, or ExtraGroupedPackin (0xDA) +0x58 on a piece. Re-done here so no game allocator is needed.
static uint32_t PackinRootID(uint8_t* ref)
{
    uint8_t* r = ref;
    if (*g_edit.kwLink)
        if (uint8_t* l = g_edit.linked(*(void**)(ref + 0xC8), *g_edit.kwLink, true)) r = l;
    if (*(void**)(r + 0x98) == *g_edit.pivotDummy) return *(uint32_t*)(r + 0x28);
    uint8_t* x = *(void**)(r + 0xC8) ? g_edit.getExtra(*(void**)(r + 0xC8), 0xDA) : nullptr;
    return x ? *(uint32_t*)(x + 0x58) : 0;
}

// Fills `out` with strong references (release them) to every member of ref's group, root included.
// Returns 0 when ref isn't grouped.
static int GroupMembers(uint8_t* ref, uint8_t** out, int max)
{
    if (!g_edit.groups || !ref || !*(void**)(ref + 0xC8)) return 0;
    uint32_t id = PackinRootID(ref);
    if (!id) return 0;
    void* kw = *g_edit.kwWorkshop;
    uint8_t* ws = kw ? g_edit.linked(*(void**)(ref + 0xC8), kw, false) : nullptr;
    if (!ws && (kw = *g_edit.kwLink)) ws = g_edit.linked(*(void**)(ref + 0xC8), kw, true);
    static uint32_t hs[2048];   // an outpost can have many objects
    int nh = LinkedChildren(ws, kw, hs, 2048), n = 0;
    for (int k = 0; k < nh && n < max; ++k) {
        uint32_t h = hs[k];
        if (!h || h == *g_edit.nullHandle) continue;
        void* m = nullptr;
        g_edit.lookup(&m, &h);
        if (!m) continue;
        bool dup = false;   // the same object can be linked more than once: never scale it twice
        for (int j = 0; j < n && !dup; ++j) dup = out[j] == m;
        if (!dup && *(void**)((uint8_t*)m + 0xC8) && PackinRootID((uint8_t*)m) == id) out[n++] = (uint8_t*)m;
        else g_edit.release(m);
    }
    return n;
}

// Scaling only resizes the 3D; collision keeps the size it was built with. After scaling, the objects
// are disabled (TESObjectREFR vfunc 0x7E8, ID 63387) and 100 ms later enabled again (Enable, ID 63386 -
// what Papyrus Enable runs), which rebuilds their 3D and collision at the new scale. Enabling straight
// away left them invisible (the unload Disable starts finishes after it). Scale presses wait until the
// scale keys are released + 150 ms; placed foundations go straight away. New requests that arrive
// while a rebuild is in progress wait in `pend` for the next one.
static struct {
    uint8_t*  pend[512]; int nPend; ULONGLONG pendSince; bool now;   // queued
    uint8_t*  act[512];  int nAct;  ULONGLONG actSince;              // disabled, waiting to enable
} g_reload = {};

static void QueueReload(uint8_t* ref, bool now = false)   // takes over a strong reference
{
    for (int k = 0; k < g_reload.nPend; ++k)
        if (g_reload.pend[k] == ref) { g_edit.release(ref); g_reload.pendSince = GetTickCount64(); return; }
    if (!g_edit.vfDisable || g_reload.nPend >= 512) { g_edit.release(ref); return; }
    g_reload.pend[g_reload.nPend++] = ref;
    g_reload.pendSince = GetTickCount64();
    g_reload.now |= now;
}

static void ReloadTick()
{
    if (!g_reload.nPend && !g_reload.nAct) return;
    ULONGLONG now = GetTickCount64();
    if (g_reload.nAct && now - g_reload.actSince >= 100) {
        int back = 0;
        for (int k = 0; k < g_reload.nAct; ++k) {
            uint8_t* r = g_reload.act[k];
            if (*(uint32_t*)(r + 0x20) & 0x800) g_edit.enable(r, true);
            back += !(*(uint32_t*)(r + 0x20) & 0x800);
            g_edit.release(r);
        }
        if (g_verbose || back != g_reload.nAct) Log("edit placed: rebuilt collision for %d of %d object(s)", back, g_reload.nAct);
        g_reload.nAct = 0;
    }
    if (!g_reload.nPend || g_reload.nAct) return;
    if (cfg.kScaleUp.down || cfg.kScaleDown.down) g_reload.pendSince = now;   // still scaling
    if (!g_reload.now && now - g_reload.pendSince < 150) return;
    using Disable = void (*)(void*);
    for (int k = 0; k < g_reload.nPend; ++k) {
        uint8_t* r = g_reload.pend[k];
        if (*(uint32_t*)(r + 0x20) & 0x800) { g_edit.release(r); continue; }   // already disabled: leave it
        ((Disable)(*(void***)r)[g_edit.vfDisable / 8])(r);
        g_reload.act[g_reload.nAct++] = r;
    }
    g_reload.nPend = 0; g_reload.now = false; g_reload.actSince = now;
}

// Auto-foundations are separate objects linked to the object they hold up (linked-ref children with
// the foundation keyword, ID 883416); the game walks them breadth-first (each piece can have its own).
// Appends them to `list` (strong references, no duplicates); returns how many were added.
static int AddFoundations(uint8_t** list, int n, int max, int* parent = nullptr)   // parent[k]: index it hangs from
{
    if (!g_edit.kwFoundation || !*g_edit.kwFoundation || !g_edit.getExtra) return 0;
    void* kw = *g_edit.kwFoundation;
    int start = n;
    for (int i = 0; i < n && n < max; ++i) {
        uint32_t hs[64];
        int nh = LinkedChildren(list[i], kw, hs, 64);
        for (int k = 0; k < nh && n < max; ++k) {
            uint32_t h = hs[k];
            if (!h || h == *g_edit.nullHandle) continue;
            void* m = nullptr;
            g_edit.lookup(&m, &h);
            if (!m) continue;
            bool dup = false;
            for (int j = 0; j < n && !dup; ++j) dup = list[j] == m;
            if (dup) g_edit.release(m);
            else { if (parent) parent[n] = i; list[n++] = (uint8_t*)m; }
        }
    }
    return n - start;
}

// Does SetScale act on this ref? The same test SetScale (REFR vfunc 0x430, 0x140B38130) starts with:
// it returns early when the base's vfunc 0x340 says so (markers, the structure root), unless the base
// is type 0x09 / 0x38 or one of three particular forms. Those refs always read 1.0.
static bool Scalable(uint8_t* ref)
{
    uint8_t* b = *(uint8_t**)(ref + 0x98);
    if (!b || !g_edit.scaleRule) return true;
    using Ignores = bool (*)(void*);
    if (!((Ignores)(*(void***)b)[0x340 / 8])(b)) return true;
    if (b[0x2E] == 0x09 || b[0x2E] == 0x38) return true;
    for (void** g : g_edit.scaleAlways) if (g && *g == b) return true;
    return false;
}

// The most common value in sc[0..n) (ties: the first), or `fallback` when n is 0.
static float ModeScale(const float* sc, int n, float fallback)
{
    float best = fallback;
    int bestCount = 0;
    for (int k = 0; k < n; ++k) {
        int c = 0;
        for (int j = 0; j < n; ++j) c += fabsf(sc[j] - sc[k]) < 0.005f;
        if (c > bestCount) { bestCount = c; best = sc[k]; }
    }
    return best;
}

// A group's scale: the most common scale among the members that can be scaled.
static float GroupScale(uint8_t** m, int n, uint8_t* fallback)
{
    static float sc[512];
    int c = 0;
    for (int k = 0; k < n && c < 512; ++k) if (Scalable(m[k])) sc[c++] = g_edit.getScale(m[k]);
    return ModeScale(sc, c, g_edit.getScale(fallback));
}

static void SetScale(uint8_t* ref, float to)
{
    using Set = void (*)(void*, float);
    ((Set)(*(void***)ref)[g_edit.vfSetScale / 8])(ref, to);
}

// Called exactly as Papyrus SetPosition / SetAngle do (IDs 118197 / 118200): a local copy of the null
// handle, the ref's own cell, and the cell's worldspace for an exterior. The engine moves the ref to
// another cell itself if the new position needs it. False if the ref has no cell (not placed in the world).
static bool MoveRef(uint8_t* ref, const float* pos, const float* ang)
{
    uint8_t* cell = *(uint8_t**)(ref + 0xB0);
    if (!cell) return false;
    void* world = (cell[0x48] & 1) ? nullptr : *(void**)(cell + 0x110);   // interior flag; else worldspace
    uint32_t none = *g_edit.nullHandle;
    g_edit.move(ref, &none, cell, world, pos, ang);
    return true;
}

// ---- rotation maths. A ref's angles (x, y, z at +0x80) give its world rotation
//   W = Rx(-x) * Ry(-y) * Rz(-z)   (standard counter-clockwise matrices, column vectors)
// - the game's own conversion (ToEuler 0x142BCDE50 / FromEuler 0x142BCE180) builds M = Rz(z)Ry(y)Rx(x)
// and uses it transposed; that is why a positive heading turns clockwise. A world-space rotation A
// about a pivot P gives W' = A * W and p' = P + A * (p - P).
struct M3 { float m[3][3]; };

static M3 Mul(const M3& a, const M3& b)
{
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
static M3 Transpose(const M3& a) { M3 r; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i]; return r; }
static M3 Identity() { return M3{ { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } }; }

// Counter-clockwise rotation by `a` radians about the unit vector k (Rodrigues).
static M3 AxisAngle(const float* k, float a)
{
    float c = cosf(a), s = sinf(a), t = 1 - c, x = k[0], y = k[1], z = k[2];
    return M3{ { { t * x * x + c,     t * x * y - s * z, t * x * z + s * y },
                 { t * x * y + s * z, t * y * y + c,     t * y * z - s * x },
                 { t * x * z - s * y, t * y * z + s * x, t * z * z + c } } };
}

static M3 FromAngles(const float* ang)
{
    float a[3] = { 1, 0, 0 }, b[3] = { 0, 1, 0 }, c[3] = { 0, 0, 1 };
    return Mul(Mul(AxisAngle(a, -ang[0]), AxisAngle(b, -ang[1])), AxisAngle(c, -ang[2]));
}

static void ToAngles(const M3& w, float* ang)   // inverse of FromAngles (as the game's ToEuler on W^T)
{
    float s = w.m[0][2] < -1 ? -1 : w.m[0][2] > 1 ? 1 : w.m[0][2];
    ang[1] = -asinf(s);
    if (fabsf(s) < 0.9999f) {
        ang[0] = atan2f(w.m[1][2], w.m[2][2]);
        ang[2] = atan2f(w.m[0][1], w.m[0][0]);
    } else {                                      // straight up / down: fold x into z
        ang[0] = 0;
        ang[2] = atan2f(-w.m[1][0], w.m[1][1]);
    }
}

// The total rotation the mod has applied to each object (keyed by its group root's / its own form ID),
// so Reset rotation can undo exactly that. Kept for this game session only (not saved).
static struct { uint32_t id; M3 total; } g_turned[256];
static int g_nTurned = 0;

static M3* TurnedFor(uint32_t id, bool create)
{
    for (int k = 0; k < g_nTurned; ++k) if (g_turned[k].id == id) return &g_turned[k].total;
    if (!create) return nullptr;
    int k = g_nTurned < 256 ? g_nTurned++ : 255;   // full: reuse the last slot
    g_turned[k] = { id, Identity() };
    return &g_turned[k].total;
}
static void ForgetTurned(uint32_t id)
{
    for (int k = 0; k < g_nTurned; ++k)
        if (g_turned[k].id == id) { g_turned[k] = g_turned[--g_nTurned]; return; }
}

static const char* const kAxisNames[3] = { "Z", "X", "Y" };   // cfg.axis 0 / 1 / 2: the object's own axes

struct EditOp {
    float forward = 0, right = 0, up = 0;   // movement, relative to the view
    float turnDeg = 0;                      // rotation about the chosen axis (cfg.axis)
    float scaleStep = 0;                    // added to the scale
    bool  resetRot = false, resetScale = false;
};

// Edit-control messages (steps, size, axis, resets) wait until the keys have been left alone for
// half a second, so holding or tapping a key shows only the final value.
static struct { char text[64]; bool pending; ULONGLONG since; } g_editMsg = {};

static void EditMsg(const char* fmt, ...)
{
    va_list va; va_start(va, fmt);
    vsnprintf(g_editMsg.text, sizeof(g_editMsg.text), fmt, va);
    va_end(va);
    g_editMsg.pending = true; g_editMsg.since = GetTickCount64();
}

static void EditMsgTick()
{
    if (!g_editMsg.pending || GetTickCount64() - g_editMsg.since < 500) return;
    g_editMsg.pending = false;
    ShowHUD(g_editMsg.text, false);
}

// Move / turn / scale the hovered object - or its whole group (and foundations), turned and scaled
// about the group's root. Movement is relative to the view: forward and right on the ground, up = world up.
static void EditHovered(const EditOp& op)
{
    uint8_t* ref = HoveredRef();
    if (!ref) return;
    static uint8_t* members[512];
    int n = GroupMembers(ref, members, 512);
    bool group = n > 1;
    if (!group) { for (int k = 0; k < n; ++k) g_edit.release(members[k]); members[0] = ref; n = 1; }
    int pieces = n, found = AddFoundations(members, n, 512);   // foundations follow too
    n += found;

    uint8_t* pivot = ref;   // the group's root when there is one
    for (int k = 0; k < n && group; ++k)
        if (*(void**)(members[k] + 0x98) == *g_edit.pivotDummy) { pivot = members[k]; break; }
    uint32_t pivotId = *(uint32_t*)(pivot + 0x28);

    float ratio = 1.0f, target = 0.0f;
    if ((op.scaleStep || op.resetScale) && g_edit.getScale && g_edit.vfSetScale) {
        float cur = GroupScale(members, pieces, ref), to = op.resetScale ? 1.0f : cur + op.scaleStep;
        to = to < cfg.scaleMin ? cfg.scaleMin : to > cfg.scaleMax ? cfg.scaleMax : to;
        target = to;
        if (cur > 0.0f) ratio = to / cur;
        if (g_verbose) Log("edit placed: scale %.3f -> %.3f", cur, to);
        EditMsg("SCALE: %.1f%%", to * 100.0f);
    }

    float h = g_player && *g_player ? *(float*)(*g_player + 0x88) : 0.0f;
    float s = sinf(h), c = cosf(h);
    float d[3] = { op.forward * s + op.right * c, op.forward * c - op.right * s, op.up };
    // the rotation, as a world-space matrix about the pivot
    bool turning = op.turnDeg != 0 || op.resetRot;
    M3 A = Identity();
    if (op.resetRot) {
        M3* t = TurnedFor(pivotId, false);
        if (t) { A = Transpose(*t); ForgetTurned(pivotId); }
        else turning = false;
        EditMsg("%s", t ? "ROTATION RESET" : "ROTATION: NOT CHANGED BY NO MORE RED");
    } else if (op.turnDeg) {
        // about the object's own axis (the group root's): column i of its world rotation W
        float a = op.turnDeg * (kPi / 180.0f);
        M3 W = FromAngles((const float*)(pivot + 0x80));
        int col = cfg.axis == 1 ? 0 : cfg.axis == 2 ? 1 : 2;
        float k[3] = { W.m[0][col], W.m[1][col], W.m[2][col] };
        A = AxisAngle(k, col == 2 ? -a : a);   // Z: + turns clockwise, like the game's heading
        M3* t = TurnedFor(pivotId, true);
        *t = Mul(A, *t);
    }
    float P[3];
    memcpy(P, pivot + 0x8C, sizeof(P));

    if (g_verbose) {
        uint8_t* base = *(uint8_t**)(ref + 0x98);
        Log("edit placed: %08X (base %08X type %u)%s, %d piece(s) + %d foundation; move %.3f %.3f %.3f turn %.2f (%s)%s scale x%.3f",
            *(uint32_t*)(ref + 0x28), base ? *(uint32_t*)(base + 0x28) : 0, base ? base[0x2E] : 0,
            group ? " group" : "", pieces, found, d[0], d[1], d[2], op.turnDeg, kAxisNames[cfg.axis],
            op.resetRot ? " reset rotation" : "", ratio);
    }

    for (int k = 0; k < n; ++k) {
        uint8_t* m = members[k];
        if (d[0] || d[1] || d[2] || turning || ratio != 1.0f) {
            float pos[3], ang[3];
            memcpy(pos, m + 0x8C, sizeof(pos));
            memcpy(ang, m + 0x80, sizeof(ang));
            float v[3] = { (pos[0] - P[0]) * ratio, (pos[1] - P[1]) * ratio, (pos[2] - P[2]) * ratio };
            for (int i = 0; i < 3; ++i)
                pos[i] = P[i] + A.m[i][0] * v[0] + A.m[i][1] * v[1] + A.m[i][2] * v[2] + d[i];
            if (turning) {
                const float* pa = (const float*)(pivot + 0x80);
                bool level = pa[0] == 0.0f && pa[1] == 0.0f && ang[0] == 0.0f && ang[1] == 0.0f;
                if (cfg.axis == 0 && !op.resetRot && level) ang[2] = WrapPi(ang[2] + op.turnDeg * (kPi / 180.0f));   // exact
                else ToAngles(Mul(A, FromAngles(ang)), ang);
            }
            MoveRef(m, pos, ang);
        }
        // one absolute scale for the whole group (also evens out pieces that drifted apart);
        // foundation pieces keep their own ratio to it
        if (ratio != 1.0f && Scalable(m)) {
            float cur = g_edit.getScale(m), to = k < pieces ? target : cur * ratio;
            if (cur > 0.0f && fabsf(to - cur) > 0.0005f) SetScale(m, to);
        }
    }
    for (int k = 0; k < n; ++k) {
        if (ratio != 1.0f) QueueReload(members[k]);   // each member (ref included) is a strong reference
        else g_edit.release(members[k]);
    }
    if (group) g_edit.release(ref);   // members hold their own references
}

// ---- Move keeps the scale: the game picks an object up as fresh, unscaled copies (the preview);
// on placing it moves the originals (scale kept) and builds a new auto-foundation at 1.0.
// Preview: the held item (single or structure) lists its copies at item+8 (u32 count, +0x10 entries
// of 0x70: local transform rows at +0x20..+0x5F, piece handle at +0x60), placed every frame from
// those rows (0xF5E8B0) - position and rotation only. The rows come from the pieces' current spacing, which
// the edit already scaled, so only each copy's own scale is missing. The same list also holds the
// game's foundation preview pieces, rebuilt as it goes: those are left to the game, so only copies
// whose base matches one of the moved pieces get that piece's scale.
static struct {
    bool     active, held;
    int      nPieces;
    void*    bases[512];
    float    scales[512];
    uint32_t ids[512];          // the moved objects' form IDs (found again after placing)
    bool     scalable[512];
    float    groupScale;
    bool     variant;           // swapping a piece for one of its variants (a different base, e.g. a wall type)
    float    variantScale;
    uint8_t  variantType;       // the swapped piece's base form type
    void*    oldFound[512];     // the foundation pieces before the move (compared only, not referenced)
    int      nOldFound;
    uint32_t stuck[512];        // copies that ignore SetScale (always 1.0): not retried every frame
    int      logs, lastFound, nStuck;
    ULONGLONG foundSince;       // when the foundation piece count last changed
    ULONGLONG since;
} g_moveSrc = {};

static float MoveScaleFor(void* base)
{
    for (int k = 0; k < g_moveSrc.nPieces; ++k) if (g_moveSrc.bases[k] == base) return g_moveSrc.scales[k];
    return 0.0f;
}

static void NoteMoveSource(int slot, uint8_t* evRef, uint32_t handle)
{
    g_moveSrc.active = false;
    if (!EditReady() || !g_edit.getScale || !g_edit.vfSetScale) return;
    // the event's ref is kept alive by the event while its handler runs; else the target handle's
    uint8_t* looked = nullptr;
    if (!evRef && handle && handle != *g_edit.nullHandle) g_edit.lookup((void**)&looked, &handle);
    uint8_t* ref = evRef ? evRef : looked;
    if (!ref) { if (g_verbose) Log("edit placed: move via inspect slot %d - no target", slot); return; }
    uint8_t* members[512];
    int n = GroupMembers(ref, members, 512);
    bool any = false;
    g_moveSrc.nPieces = 0;
    auto add = [&](uint8_t* m) {
        if (g_moveSrc.nPieces >= 512) return;
        float sc = g_edit.getScale(m);
        any |= fabsf(sc - 1.0f) > 0.001f;
        int k = g_moveSrc.nPieces++;
        g_moveSrc.bases[k] = *(void**)(m + 0x98);
        g_moveSrc.scales[k] = sc;
        g_moveSrc.scalable[k] = Scalable(m);
        g_moveSrc.ids[k] = *(uint32_t*)(m + 0x28);
    };
    if (n > 1) for (int k = 0; k < n; ++k) add(members[k]);
    else add(ref);
    g_moveSrc.groupScale = n > 1 ? GroupScale(members, n, ref) : g_edit.getScale(ref);
    // slots 13 / 15: the target is swapped for a variant - a new piece with a different base - which
    // gets the scale of the piece it replaces
    uint8_t* tb = *(uint8_t**)(ref + 0x98);
    g_moveSrc.variant = slot != 25 && tb;
    g_moveSrc.variantScale = g_edit.getScale(ref);
    g_moveSrc.variantType = tb ? tb[0x2E] : 0;
    if (g_moveSrc.variant) any = Scalable(ref) && fabsf(g_moveSrc.variantScale - 1.0f) > 0.001f;
    // the current foundation: on placing, the game removes it and builds a new one
    g_moveSrc.nOldFound = 0;
    if (any && !g_moveSrc.variant) {
        static uint8_t* lst[512];
        int m0 = 0;
        if (n > 1) for (int k = 0; k < n && m0 < 512; ++k) lst[m0++] = members[k];
        else lst[m0++] = ref;
        int f = AddFoundations(lst, m0, 512);
        for (int k = m0; k < m0 + f; ++k) {
            if (g_moveSrc.nOldFound < 512) g_moveSrc.oldFound[g_moveSrc.nOldFound++] = lst[k];
            g_edit.release(lst[k]);
        }
    }
    if (any) {
        g_moveSrc.active = true; g_moveSrc.held = false;
        g_moveSrc.logs = 0; g_moveSrc.nStuck = 0; g_moveSrc.since = GetTickCount64();
        if (g_verbose) Log("edit placed: %s scaled %08X (%d piece(s), scale %.2f, %d foundation piece(s), inspect slot %d)",
            g_moveSrc.variant ? "swapping variant of" : "moving", *(uint32_t*)(ref + 0x28), g_moveSrc.nPieces,
            g_moveSrc.variant ? g_moveSrc.variantScale : g_moveSrc.groupScale, g_moveSrc.nOldFound, slot);
    } else if (g_verbose) Log("edit placed: move via inspect slot %d (%08X, not scaled)", slot, *(uint32_t*)(ref + 0x28));
    for (int k = 0; k < n; ++k) g_edit.release(members[k]);
    if (looked) g_edit.release(looked);
}

static void KeepMoveScale(void* root)   // after each UpdatePlacementStatus
{
    if (!g_moveSrc.active) return;
    void** item = *(void***)((uint8_t*)root + 0x268);   // the held workshop item
    if (!item) return;
    uint32_t count = *(uint32_t*)((uint8_t*)item + 8);
    uint8_t* e = *(uint8_t**)((uint8_t*)item + 0x10);
    if (!count || count > 4096 || !e) return;
    int scaled = 0, matched = 0, others = 0;
    for (uint32_t k = 0; k < count; ++k, e += 0x70) {
        uint32_t h = *(uint32_t*)(e + 0x60);
        if (!h || h == *g_edit.nullHandle) continue;
        bool stuck = false;
        for (int j = 0; j < g_moveSrc.nStuck && !stuck; ++j) stuck = g_moveSrc.stuck[j] == h;
        if (stuck) { ++matched; continue; }
        void* r = nullptr;
        g_edit.lookup(&r, &h);
        if (!r) continue;
        uint8_t* rb = *(uint8_t**)((uint8_t*)r + 0x98);
        float want = g_moveSrc.variant ? (rb && rb[0x2E] == g_moveSrc.variantType ? g_moveSrc.variantScale : 0.0f)
                                       : MoveScaleFor(rb);
        if (want > 0.0f) {
            ++matched;
            if (fabsf(g_edit.getScale(r) - want) > 0.001f) {
                SetScale((uint8_t*)r, want);
                if (fabsf(g_edit.getScale(r) - want) > 0.001f) {   // ignores it: don't try again
                    if (g_moveSrc.nStuck < 512) g_moveSrc.stuck[g_moveSrc.nStuck++] = h;
                } else ++scaled;
            }
        } else ++others;
        g_edit.release(r);
    }
    if (!matched) return;   // not our item
    g_moveSrc.held = true; g_moveSrc.since = GetTickCount64();
    if (scaled && g_verbose && g_moveSrc.logs++ < 4)
        Log("edit placed: preview: scaled %d, copies %d, other pieces (foundation preview) %d", scaled, matched, others);
}

// Swapping a piece for a variant (StateInspect slots 13 / 15) isn't held: the game replaces the piece
// in place with a new ref at 1.0. Its group is looked at again until a member appears that wasn't
// there before; scalable new members get the old piece's scale (then the collision rebuild).
static void VariantTick(ULONGLONG now)
{
    if (now - g_moveSrc.since < 50) return;
    if (now - g_moveSrc.since > 2000) {
        g_moveSrc.active = false;
        if (g_verbose) Log("edit placed: variant swap - no new piece found");
        return;
    }
    uint8_t* anchor = nullptr;   // any recorded member still there
    for (int k = 0; k < g_moveSrc.nPieces && !anchor; ++k)
        if (uint8_t* f = g_edit.byId ? g_edit.byId(g_moveSrc.ids[k]) : nullptr)
            if (*(void**)(f + 0x98) == g_moveSrc.bases[k] && !(*(uint32_t*)(f + 0x20) & 0x820)) anchor = f;
    if (!anchor) return;
    static uint8_t* members[512];
    int n = GroupMembers(anchor, members, 512), sized = 0;
    for (int k = 0; k < n; ++k) {
        uint8_t* m = members[k];
        uint32_t id = *(uint32_t*)(m + 0x28);
        bool known = false;
        for (int j = 0; j < g_moveSrc.nPieces && !known; ++j) known = g_moveSrc.ids[j] == id;
        if (!known && Scalable(m) && fabsf(g_edit.getScale(m) - g_moveSrc.variantScale) > 0.001f) {
            SetScale(m, g_moveSrc.variantScale);
            QueueReload(m, true);   // takes the reference
            ++sized;
        } else g_edit.release(m);
    }
    if (sized) {
        g_moveSrc.active = false;
        if (g_verbose) Log("edit placed: variant swap - %d new piece(s) scaled to %.2f", sized, g_moveSrc.variantScale);
    }
}

// After a scaled move is placed, the new foundation is sized to the object it holds up as soon as it
// exists, then its collision is rebuilt.
static void MoveAfterTick()
{
    if (!g_moveSrc.active) return;
    ULONGLONG now = GetTickCount64();
    if (g_moveSrc.variant) { VariantTick(now); return; }
    if (!g_moveSrc.held) { if (now - g_moveSrc.since > 5000) g_moveSrc.active = false; return; }
    // As soon as the placed object's new foundation exists and has stopped changing: new (unscaled)
    // pieces are there and the piece count hasn't changed for 150 ms - the game removes the old
    // foundation and builds the new one over several frames. A cancelled move, or nothing to size,
    // gives up 3 s after placing.
    if (now - g_heldTick < 50) { g_moveSrc.lastFound = -1; g_moveSrc.foundSince = now; return; }
    bool timeUp = now - g_heldTick > 3000;
    auto byId = g_edit.byId;
    if (!byId) { g_moveSrc.active = false; return; }
    static uint8_t* list[512];
    static int parent[512];
    static float want[512];
    int n = 0;
    for (int k = 0; k < g_moveSrc.nPieces && n < 512; ++k) {
        uint8_t* f = byId(g_moveSrc.ids[k]);
        if (!f || *(void**)(f + 0x98) != g_moveSrc.bases[k] || (*(uint32_t*)(f + 0x20) & 0x800)) continue;
        // a foundation hanging from a piece that can't be scaled (the root) follows the group's scale
        want[n] = g_moveSrc.scalable[k] ? g_moveSrc.scales[k] : g_moveSrc.groupScale;   // raw pointer: this pass only
        parent[n] = -1;
        list[n++] = f;
    }
    int pieces = n;
    // AddFoundations returns strong references for what it adds
    int found = AddFoundations(list, n, 512, parent);
    n += found;
    if (found != g_moveSrc.lastFound) { g_moveSrc.lastFound = found; g_moveSrc.foundSince = now; }
    // new pieces = not in the foundation from before the move, and not being removed (disabled / deleted)
    static bool isNew[512];
    int fresh = 0, old = 0;
    for (int k = pieces; k < n; ++k) {
        bool was = false;
        for (int j = 0; j < g_moveSrc.nOldFound && !was; ++j) was = g_moveSrc.oldFound[j] == list[k];
        isNew[k] = !was && !(*(uint32_t*)(list[k] + 0x20) & 0x820);
        fresh += isNew[k]; old += was;
    }
    if (!timeUp && (!fresh || old || now - g_moveSrc.foundSince < 150)) {   // not rebuilt yet
        for (int k = pieces; k < n; ++k) g_edit.release(list[k]);
        return;
    }
    g_moveSrc.active = false;
    // the group's scale (the root a foundation often hangs from ignores scaling and reads 1.0)
    float groupScale = g_moveSrc.groupScale, lo = 1e9f, hi = 0;
    for (int k = 0; k < g_moveSrc.nPieces; ++k) {
        lo = g_moveSrc.scales[k] < lo ? g_moveSrc.scales[k] : lo;
        hi = g_moveSrc.scales[k] > hi ? g_moveSrc.scales[k] : hi;
    }
    int sized = 0;
    for (int k = pieces; k < n; ++k) {
        float w = want[parent[k]];
        want[k] = w;
        if (!isNew[k] || fabsf(g_edit.getScale(list[k]) - w) < 0.001f) want[k] = 0;   // old, or nothing to do
    }
    // Top of a piece = its z + the base's bound max z (TESBoundObject +0x64; FoundationPieceHeight 74056
    // reads +0x64 - +0x58); the foundation grows down into the ground instead of up through the object.
    // Worked out from a snapshot first, before any piece moves.
    static float orig[512][3], dest[512][3], topZ[512];
    for (int k = pieces; k < n; ++k) {
        uint8_t* b = *(uint8_t**)(list[k] + 0x98);
        memcpy(orig[k], list[k] + 0x8C, sizeof(orig[k]));
        topZ[k] = orig[k][2] + (b ? *(float*)(b + 0x64) * g_edit.getScale(list[k]) : 0.0f);
    }
    // The whole foundation under an object is scaled as one rigid piece - legs, stairs, platforms - about
    // the point where it meets that object: the object's x, y (the game lays it out from the object's
    // unscaled foundation points) and the highest top of any piece under it. Spacing and heights then
    // change 1:1 with the scale, so nothing drifts apart.
    static int anc[512];
    for (int k = pieces; k < n; ++k) { int a = parent[k]; while (a >= pieces) a = parent[a]; anc[k] = a; }
    // The attach height: the top most of the pieces hanging straight from the object share (the legs).
    // The highest top overall isn't it - a stair's handrail can rise above the platform it meets.
    static float anchorZ[512];
    for (int a = 0; a < pieces; ++a) {
        float best = -1e30f; int bestCount = 0;
        for (int k = pieces; k < n; ++k) {
            if (anc[k] != a || parent[k] != a) continue;
            int c = 0;
            for (int j = pieces; j < n; ++j) c += anc[j] == a && parent[j] == a && fabsf(topZ[j] - topZ[k]) < 1.0f;
            if (c > bestCount || (c == bestCount && topZ[k] > best)) { bestCount = c; best = topZ[k]; }
        }
        if (!bestCount)   // nothing hangs straight from it: fall back to the highest top
            for (int k = pieces; k < n; ++k) if (anc[k] == a && topZ[k] > best) best = topZ[k];
        anchorZ[a] = best;
        if (bestCount && g_verbose) {
            int under = 0;
            for (int k = pieces; k < n; ++k) under += anc[k] == a;
            Log("edit placed: foundation under %08X: %d piece(s), attach height %.1f (%d share it)",
                *(uint32_t*)(list[a] + 0x28), under, best, bestCount);
        }
    }
    for (int k = pieces; k < n; ++k) {
        if (!want[k]) continue;
        float top = anchorZ[anc[k]];
        float ratio = want[k] / g_edit.getScale(list[k]);
        float* pa = (float*)(list[anc[k]] + 0x8C);
        dest[k][0] = pa[0] + (orig[k][0] - pa[0]) * ratio;
        dest[k][1] = pa[1] + (orig[k][1] - pa[1]) * ratio;
        dest[k][2] = top + (orig[k][2] - top) * ratio;
    }
    for (int k = pieces; k < n; ++k) {
        if (!want[k]) continue;
        float ang[3];
        memcpy(ang, list[k] + 0x80, sizeof(ang));
        MoveRef(list[k], dest[k], ang);
        SetScale(list[k], want[k]);
        ++sized;
    }
    if (g_verbose) Log("edit placed: after placing (%s): %d of %d object(s) found (piece scales %.2f - %.2f, group %.2f), "
        "%d foundation piece(s) (%d new, %d old), %d sized", timeUp ? "timed out" : "rebuilt", pieces, g_moveSrc.nPieces,
        lo, hi, groupScale, found, fresh, old, sized);
    for (int k = pieces; k < n; ++k) {
        if (want[k]) QueueReload(list[k], true);   // takes the reference over; nothing to wait for
        else g_edit.release(list[k]);
    }
}

// ================================================================ hotkeys
// Polled once per frame on the main thread by an SFSE task; keys only act while the game has
// focus, but their state is tracked regardless so alt-tabbing back with a key held isn't a press.
// `invert`: the HUD shows the opposite of the state (WORKSHOP BOUNDARY: OFF = build anywhere on).
struct Toggle { Key* key; volatile LONG* state; const char* label; bool available; void (*changed)(); bool invert; };
static Toggle g_toggles[] = {
    { &cfg.kEnabled,     &cfg.enabled,     "NO MORE RED",      false, UpdateTooTallPatch },
    { &cfg.kObjectSnap,  &cfg.objectSnap,  "OBJECT SNAPPING",  false, nullptr },
    { &cfg.kSurfaceSnap, &cfg.surfaceSnap, "SURFACE SNAPPING", false, nullptr },
    { &cfg.kFoundations, &cfg.foundations, "FOUNDATIONS",      false, UpdateTooTallPatch },
    { &cfg.kBuildAnywhere, &cfg.buildAnywhere, "WORKSHOP BOUNDARY", false, nullptr, true },
    { &cfg.kRotate,      &cfg.rotateWithView, "ROTATE WITH VIEW", false, nullptr },
};
enum { kToggleEnabled, kToggleObjectSnap, kToggleSurfaceSnap, kToggleFoundations, kToggleBuildAnywhere, kToggleRotate };

// Build mode = the game's WorkshopMenu is open: UI::IsMenuOpen(UI singleton, WorkshopMenu name).
// If that can't be verified at startup, hotkeys work everywhere as before.
static struct {
    void** ui = nullptr;
    bool (*isOpen)(void*, void*) = nullptr;
    void* (*name)() = nullptr;
} g_menu;

static bool InBuildMode()
{
    if (!g_menu.isOpen) return true;
    void* ui = *g_menu.ui;
    void* name = ui ? g_menu.name() : nullptr;
    return name && g_menu.isOpen(ui, name);
}

// Numpad Enter shares VK_RETURN with the main Enter key; only raw input tells them apart (E0 flag).
// Key code 0x10D in the INI means numpad Enter, tracked by the raw input hook.
static constexpr int kVkNumpadEnter = 0x10D;
static volatile bool g_numEnterDown = false;

// Fires on press; with `repeat`, again every 50 ms after 300 ms held.
static bool Fired(Key& k, bool repeat)
{
    if (!k.vk) return false;
    bool d = k.vk == kVkNumpadEnter ? g_numEnterDown : (GetAsyncKeyState(k.vk) & 0x8000) != 0;
    ULONGLONG now = GetTickCount64();
    bool fire = d && (!k.down || (repeat && now - k.since > 300 && now - k.last >= 50));
    if (d && !k.down) k.since = now;
    if (fire) k.last = now;
    k.down = d;
    return fire;
}

// ---- modifier controls: hold distance (wheel / keys) and fine rotate (mouse buttons / keys) only
// act with the modifier (Ctrl) held, and while it is held in build mode the game doesn't get them
// (no vanilla rotate on the same click, no menu navigation on arrow keys). The game reads keyboard
// and mouse with raw input (USER32 GetRawInputData, one import); that import is redirected and,
// while blocking, those inputs come back empty. A release is hidden only if its press was hidden,
// so nothing is left stuck either way. Wheel notches seen while blocking are queued for PollKeys.
static volatile bool g_blockKeys = false;
static volatile LONG g_wheelQueued = 0;   // wheel delta (120 per notch)
static UINT (WINAPI* g_GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT) = nullptr;
static void** g_rawInputSlot = nullptr;

static Key* const kModKeys[4] = { &cfg.kFarther, &cfg.kCloser, &cfg.kFineLeft, &cfg.kFineRight };

// Mouse button virtual keys -> raw input down flag (the up flag is down << 1).
static USHORT MouseDownFlag(int vk)
{
    switch (vk) {
    case VK_LBUTTON:  return RI_MOUSE_LEFT_BUTTON_DOWN;
    case VK_RBUTTON:  return RI_MOUSE_RIGHT_BUTTON_DOWN;
    case VK_MBUTTON:  return RI_MOUSE_MIDDLE_BUTTON_DOWN;
    case VK_XBUTTON1: return RI_MOUSE_BUTTON_4_DOWN;
    case VK_XBUTTON2: return RI_MOUSE_BUTTON_5_DOWN;
    default:          return 0;
    }
}

static UINT WINAPI Hook_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    UINT r = g_GetRawInputData(h, cmd, data, size, header);
    if (cmd != RID_INPUT || !data || r == (UINT)-1 || r < sizeof(RAWINPUTHEADER)) return r;
    RAWINPUT* ri = (RAWINPUT*)data;
    static bool swallowed[4] = {};   // press was hidden, so hide its release too (and only then)
    if (ri->header.dwType == RIM_TYPEKEYBOARD && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)) {
        int vk = ri->data.keyboard.VKey;
        if (vk == VK_RETURN && (ri->data.keyboard.Flags & RI_KEY_E0))
            g_numEnterDown = !(ri->data.keyboard.Flags & RI_KEY_BREAK);
        for (int i = 0; vk && i < 4; ++i) {
            if (vk != kModKeys[i]->vk || MouseDownFlag(vk)) continue;
            bool up = (ri->data.keyboard.Flags & RI_KEY_BREAK) != 0;
            bool hide = up ? swallowed[i] : g_blockKeys;
            swallowed[i] = !up && hide;
            if (hide) { ri->data.keyboard.VKey = 0xFF; ri->data.keyboard.MakeCode = 0; }
            break;
        }
    } else if (ri->header.dwType == RIM_TYPEMOUSE && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        USHORT& flags = ri->data.mouse.usButtonFlags;
        for (int i = 0; i < 4; ++i) {
            USHORT down = MouseDownFlag(kModKeys[i]->vk), up = (USHORT)(down << 1);
            if (!down) continue;
            if ((flags & down) && g_blockKeys) { flags &= ~down; swallowed[i] = true; }
            if ((flags & up) && swallowed[i])  { flags &= ~up;   swallowed[i] = false; }
        }
        if ((flags & RI_MOUSE_WHEEL) && g_blockKeys && cfg.wheel) {
            InterlockedExchangeAdd(&g_wheelQueued, (SHORT)ri->data.mouse.usButtonData);
            flags &= ~RI_MOUSE_WHEEL;
            ri->data.mouse.usButtonData = 0;
        }
    }
    return r;
}

// The exe's import slot for USER32!GetRawInputData.
static void** FindImport(const char* dll, const char* fn)
{
    uint8_t* base = ExeBase();
    auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) || !imp->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA64*)(base + imp->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
            if (!IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) &&
                !strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, fn))
                return (void**)&slots->u1.Function;
    }
    return nullptr;
}

static bool ModifierDown() { return !cfg.modifier || (GetAsyncKeyState(cfg.modifier) & 0x8000); }

// Step sizes: 0.1% 0.5% 1% 5% 10% 25% 50% 100% 200% of a unit - 1.0 scale, 1 m, 90 degrees.
static const float kStepPct[9] = { 0.001f, 0.005f, 0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f };

// The next listed step above (dir > 0) or below cur; stays at the end of the list.
static float NextStep(float cur, float unit, int dir)
{
    if (dir > 0) {
        for (float p : kStepPct) if (p * unit > cur * 1.0001f) return p * unit;
        return kStepPct[8] * unit;
    }
    for (int k = 8; k >= 0; --k) if (kStepPct[k] * unit < cur * 0.9999f) return kStepPct[k] * unit;
    return kStepPct[0] * unit;
}

static void ChangeStep(const char* what, float& step, float unit, int dir, const char* units)
{
    step = NextStep(step, unit, dir);
    if (units[0]) EditMsg("%s STEP: %g %s", what, step, units);
    else          EditMsg("%s STEP: %g%%", what, step * 100.0f);
}

static void PollKeys()
{
    // Every key's state is read every poll so a key held from before never counts as a press.
    bool fired[sizeof(g_toggles) / sizeof(g_toggles[0])], any = false;
    for (size_t i = 0; i < sizeof(g_toggles) / sizeof(g_toggles[0]); ++i) any |= fired[i] = Fired(*g_toggles[i].key, false);
    bool mod = ModifierDown();
    bool farther = Fired(cfg.kFarther, true) && mod, closer = Fired(cfg.kCloser, true) && mod;
    bool left = Fired(cfg.kFineLeft, true) && mod, right = Fired(cfg.kFineRight, true) && mod;
    LONG wheel = g_wheelQueued;                       // whole notches only; the rest stays queued
    int notches = wheel / WHEEL_DELTA;
    if (notches) InterlockedExchangeAdd(&g_wheelQueued, -notches * WHEEL_DELTA);
    int nx = Fired(cfg.kNudgeRight, true) - Fired(cfg.kNudgeLeft, true);
    int nz = Fired(cfg.kNudgeUp, true) - Fired(cfg.kNudgeDown, true);
    int ns = Fired(cfg.kScaleUp, true) - Fired(cfg.kScaleDown, true);
    int sStep = Fired(cfg.kScaleStepUp, false) - Fired(cfg.kScaleStepDown, false);
    int mStep = Fired(cfg.kMoveStepUp, false) - Fired(cfg.kMoveStepDown, false);
    int rStep = Fired(cfg.kRotStepUp, false) - Fired(cfg.kRotStepDown, false);
    bool resetRot = Fired(cfg.kResetRot, false), resetScale = Fired(cfg.kResetScale, false);
    bool axis = Fired(cfg.kAxis, false);

    ReloadTick();
    MoveAfterTick();
    EditMsgTick();
    bool act = any || farther || closer || left || right || notches || nx || nz || ns ||
               sStep || mStep || rStep || resetRot || resetScale || axis;
    bool wantBlock = g_rawInputSlot && cfg.modifier && mod;
    if (!act && !wantBlock) { g_blockKeys = false; return; }
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool ready = pid == GetCurrentProcessId() && InBuildMode();   // focused, in build mode
    g_blockKeys = wantBlock && ready;
    if (!g_blockKeys) g_wheelQueued = 0;
    if (!act || !ready) return;
    for (size_t i = 0; i < sizeof(g_toggles) / sizeof(g_toggles[0]); ++i) {
        Toggle& t = g_toggles[i];
        if (!fired[i] || !t.available) continue;
        bool on = (InterlockedXor(t.state, 1) ^ 1) != 0;
        if (t.changed) t.changed();
        char msg[64];
        snprintf(msg, sizeof(msg), "%s: %s", t.label, (on != t.invert) ? "ON" : "OFF");
        ShowHUD(msg);
    }
    if (sStep) ChangeStep("SCALE", cfg.scaleStep, 1.0f, sStep, "");
    if (mStep) ChangeStep("MOVE", cfg.nudgeStep, 1.0f, mStep, "m");
    if (rStep) ChangeStep("ROTATE", cfg.fineStep, 90.0f, rStep, "deg");
    if (axis) {
        cfg.axis = (cfg.axis + 1) % 3;   // Z -> X -> Y
        EditMsg("ROTATE AXIS: %s", kAxisNames[cfg.axis]);
    }
    float dist = (float)(farther - closer + notches), turn = (float)(right - left);
    if (Holding()) {                          // the held item
        if (dist) AdjustHoldDistance(dist);
        if (turn) FineRotate(turn);
    } else if (!g_inspecting && (nx || nz || ns || resetRot || resetScale)) {
        if (g_verbose) Log("edit placed: key pressed but not looking at a placed object");
    } else if (g_inspecting && EditReady() && (dist || turn || nx || nz || ns || resetRot || resetScale)) {   // the hovered object
        EditOp op;
        op.forward = dist * cfg.nudgeStep; op.right = nx * cfg.nudgeStep; op.up = nz * cfg.nudgeStep;
        op.turnDeg = turn * cfg.fineStep; op.scaleStep = ns * cfg.scaleStep;
        op.resetRot = resetRot; op.resetScale = resetScale;
        EditHovered(op);
    }
}

// SFSE ITaskDelegate with a hand-built vtable (Run, Destroy), independent of compiler ABI.
struct PollTask { void** vtbl; };
static void PollTask_Run(PollTask*) { PollKeys(); }
static void PollTask_Destroy(PollTask*) {}
static void* g_pollTaskVtbl[2] = { (void*)&PollTask_Run, (void*)&PollTask_Destroy };
static PollTask g_pollTask{ g_pollTaskVtbl };

// ================================================================ install

// Status byte offset from `cmp byte ptr [r8+X], 2` in three commit states; all must agree.
static uint32_t DetectStatusOffset()
{
    uint32_t found = 0; int agree = 0, total = 0;
    for (uint32_t id : ID::StatusProbe)
        if (uint8_t* f = Code(id))
            if (uint8_t* m = Scan(f, 0x60, "41 80 B8 ?? ?? ?? ?? 02")) {
                ++total;
                if (!found) found = U32(m + 3);
                agree += U32(m + 3) == found;
            }
    Log("status offset probe: %d/%d agree on 0x%X", agree, total, found);
    return (total && agree == total && found < 0x2000) ? found : 0;
}

// Map each INI code to the game's current number via GetWorkshopCodeMessage:
//   cmp eax,N; ja | mov ecx,[rdx+rax*4+TBL] | cases: mov rax,[rip+GMST]; ret (GMST+0x10 = name)
static void ResolveCodes()
{
    uint8_t* fn  = Code(ID::GetCodeMessage);
    uint8_t* cmp = fn ? Scan(fn, 0x40, "83 F8 ?? 0F 87") : nullptr;
    uint8_t* tbl = fn ? Scan(fn, 0x40, "8B 8C 82") : nullptr;
    const char* names[128] = {};
    int count = 0;
    if (cmp && tbl && cmp[2] >= 31 && cmp[2] < 128) {
        count = cmp[2] + 1;
        for (int c = 0; c < count; ++c) {
            uint8_t* cs = ExeBase() + I32(ExeBase() + U32(tbl + 3) + 4 * c);
            if (!InText(cs) || cs[0] != 0x48 || cs[1] != 0x8B || cs[2] != 0x05 || cs[7] != 0xC3) continue;
            const char* n = nullptr;
            memcpy(&n, cs + 7 + I32(cs + 3) + 0x10, 8);
            if (n && !IsBadReadPtr(n, 8)) names[c] = n;
        }
    }
    auto find = [&](const char* gmst, int from) {
        for (int c = from; c < count; ++c) if (names[c] && !strcmp(names[c], gmst)) return c;
        return -1;
    };
    if (count) {
        int v = find("sWorkshopPlacement_VALID", 1);   // 0 shares the VALID message
        if (v > 0) g_valid = (uint8_t)v;
        for (auto& c : kCodes) {
            int n = find(c.gmst, 0);
            if (n < 0) Log("  %s not in the game's table - never relaxed", c.gmst);
            else if (n != c.code) Log("  %s moved %u -> %d", c.gmst, c.code, n);
            c.code = n < 0 ? 0xFF : (uint8_t)n;
        }
        Log("code table: verified against the game (%d codes, VALID=%u)", count, g_valid);
    } else Log("code table: could not read the game's table - using 1.16.244 numbers unverified");

    for (auto& c : kCodes) {
        bool boundary = &c == &kCodes[kOutsideBuildArea];
        if (c.code != 0xFF && !boundary) g_relax[c.code] = c.want;
        Log("  [%3u] %-22s %s  (%s)", c.code, c.key,
            boundary ? "WORKSHOP BOUNDARY toggle" : (c.code != 0xFF && c.want) ? "RELAX" : "keep ", c.desc);
    }
}

static bool Install(uint32_t runtime)
{
    if (!FindTextSection() || !LoadAddressLibrary(runtime)) return false;

    uint8_t* fEval = Code(ID::EvaluatePlacement);
    uint8_t* fUpd  = Code(ID::UpdatePlacementStatus);
    if (!cfg.statusOffset) cfg.statusOffset = DetectStatusOffset();
    if (!fEval || !fUpd || !cfg.statusOffset) { Log("core functions or status offset not found - nothing patched"); return false; }
    Log("status offset: 0x%X", cfg.statusOffset);
    ResolveCodes();

    uint8_t* fSnap  = Code(ID::GetSnapCandidates);
    uint8_t* fCount = Code(ID::FoundationCount);
    uint8_t* fCast  = Code(ID::SurfaceCast);
    uint8_t* fDrop  = Code(ID::DropToGround);
    uint8_t* targets[] = { fEval, fUpd, fSnap, fCount, fCast, fDrop };
    ScanCallers(targets, sizeof(targets) / sizeof(targets[0]));
    if (!AllocStubs()) { Log("could not allocate stub memory near the exe - nothing patched"); return false; }

    // No More Red (required)
    if (!Hookable(fEval) || !Hookable(fUpd)) {   // check both before planning either
        Log("core functions have unexpected callers (%d / %d) - nothing patched",
            Callers(fEval) ? Callers(fEval)->count : 0, Callers(fUpd) ? Callers(fUpd)->count : 0);
        return false;
    }
    static void* const wEval[] = SITE_WRAPPERS(Hook_EvaluatePlacement);
    static void* const wUpd[]  = SITE_WRAPPERS(Hook_UpdatePlacementStatus);
    HookCalls("EvaluatePlacement", fEval, wEval, (void**)g_EvaluatePlacement, 2);
    HookCalls("UpdatePlacementStatus", fUpd, wUpd, (void**)g_UpdatePlacementStatus, 2);
    g_toggles[kToggleEnabled].available = g_toggles[kToggleBuildAnywhere].available = true;

    // foundation check's red highlight: `mov [r14+status],al; xor r9d,r9d; mov r8b,1; mov rdx,r14; call highlight`
    if (uint8_t* m = Scan(fUpd, 0x1400, "41 88 86 ?? ?? ?? ?? 45 33 C9 41 B0 01 49 8B D6 E8"))
        Log(U32(m + 3) == cfg.statusOffset && HookOneCall("foundation highlight", m + 16, (void*)&Hook_Highlight, (void**)&g_Highlight)
            ? "foundations: red highlight call hooked" : "foundations: red highlight call not hooked");
    else Log("foundations: red highlight call not found");

    // WORKSHOP BOUNDARY: in each function, the call to the build-area test that is followed by
    // "status = OutsideBuildArea" (`mov byte ptr [reg+status],N` / `mov al,N` / `mov dil,N`)
    {
        static void* const wArea[] = { (void*)&Hook_InBuildArea<0>, (void*)&Hook_InBuildArea<1>, (void*)&Hook_InBuildArea<2> };
        const struct { uint32_t fn, callee, len; const char* name; } checks[3] = {
            { ID::ValidatePlacement, ID::IsInBuildArea,      0x200, "ValidatePlacement" },
            { ID::PlaceOnSurface,    ID::IsPointInBuildArea, 0x800, "surface cast" },
            { ID::PlaceFollowCamera, ID::IsPointInBuildArea, 0x200, "follow-camera cast" },
        };
        uint8_t c10 = kCodes[kOutsideBuildArea].code;
        int hooked = 0;
        for (int k = 0; k < 3; ++k) {
            uint8_t *f = Code(checks[k].fn), *callee = Code(checks[k].callee), *site = nullptr;
            for (uint8_t* q = f; f && callee && c10 != 0xFF && q + 0x30 <= f + checks[k].len && !site; ++q) {
                if (q[0] != 0xE8 || q + 5 + I32(q + 1) != callee) continue;
                for (uint8_t* m = q + 5; m < q + 0x30 && !site; ++m)
                    if ((m[0] == 0x41 && m[1] == 0xC6 && (m[2] & 0xF8) == 0x80 && U32(m + 3) == cfg.statusOffset && m[7] == c10) ||
                        (m[0] == 0xB0 && m[1] == c10) || (m[0] == 0x40 && m[1] == 0xB7 && m[2] == c10)) site = q;
            }
            bool ok = site && HookOneCall("workshop boundary: build area test", site, wArea[k], (void**)&g_InBuildArea[k]);
            hooked += ok;
            if (!ok) Log("workshop boundary: %s build area check not found", checks[k].name);
        }
        Log("workshop boundary: %d of 3 build area checks hooked", hooked);

        // player outside-area timer: `lea r8,[rbx+area]; mov rdx,[rip+player]; call IsInBuildArea`
        uint8_t *f = Code(ID::PlayerOutsideTimer), *callee = Code(ID::IsInBuildArea), *site = nullptr;
        uint8_t* player = Addr(ID::PlayerSingleton);
        for (uint8_t* q = f; f && callee && player && q + 12 <= f + 0x80 && !site; ++q)
            if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x15 && q + 7 + I32(q + 3) == player &&
                q[7] == 0xE8 && q + 12 + I32(q + 8) == callee) site = q + 7;
        static void* const wTimer = (void*)&Hook_InBuildArea<3>;
        Log(site && HookOneCall("workshop boundary: player timer", site, wTimer, (void**)&g_InBuildArea[3])
            ? "workshop boundary: outside-area timeout hooked" : "workshop boundary: outside-area timeout not found");
    }

    // object snapping
    static void* const wSnap[] = SITE_WRAPPERS(Hook_GetSnapCandidates);
    g_toggles[kToggleObjectSnap].available =
        fSnap && HookCalls("object snapping: GetSnapCandidates", fSnap, wSnap, (void**)g_GetSnapCandidates, 2);

    // foundations: piece count hook + "too tall" flag write (status-1, right before the status byte)
    static void* const wCount[] = SITE_WRAPPERS(Hook_FoundationCount);
    g_toggles[kToggleFoundations].available =
        fCount && HookCalls("foundations: FoundationCount", fCount, wCount, (void**)g_FoundationCount, 3);
    if (uint8_t* f = Code(ID::AutoFoundationHeight))
        if (uint8_t* m = Scan(f, 0x600, "80 7D ?? 00 74 08 41 C6 87 ?? ?? ?? ?? 01"))
            if (U32(m + 9) == cfg.statusOffset - 1) { g_tooTallWrite = m + 6; memcpy(g_tooTallOrig, g_tooTallWrite, 8); }
    Log(g_tooTallWrite ? "foundation too tall: flag write at %p" : "foundation too tall: flag write not found", g_tooTallWrite);

    // surface snapping: AddHit vtable slot + normal.z offset + SurfaceCast call site
    uint8_t* vtbl = Addr(ID::ForwardCastVtbl);
    uint8_t* fAdd = Code(ID::ForwardCastAddHit);
    if (fCast && vtbl && fAdd && *(uint8_t**)(vtbl + 4 * 8) == fAdd) {
        // FLT_MAX goes into hit position then hit normal: `vmovsd [rcx+X],xmm1; mov [rcx+X+8],eax`
        // twice, normal right after position. normal.z = second store's X+8.
        uint32_t posZ = 0;
        for (uint8_t* m = fCast; (m = Scan(m, (int)(fCast + 0x180 - m), "C5 FB 11 89 ?? ?? ?? ?? 89 81")); ++m) {
            uint32_t a = U32(m + 4), b = U32(m + 10);
            if (b != a + 8 || b >= 0x2000) continue;
            if (!posZ) posZ = b;
            else if (a == posZ + 4) { g_normalZOffset = b; break; }
        }
        Log("surface snapping: normal.z offset 0x%X", g_normalZOffset);
        static void* const wCast[] = SITE_WRAPPERS(Hook_SurfaceCast);
        static void* const wDrop[] = SITE_WRAPPERS(Hook_DropToGround);
        if (g_normalZOffset && HookCalls("surface snapping: SurfaceCast", fCast, wCast, (void**)g_SurfaceCast, 1)) {
            g_AddHit = (decltype(g_AddHit))fAdd;
            g_addHitSlot = (uint8_t**)(vtbl + 4 * 8);   // patched in ApplyDeferred
            g_toggles[kToggleSurfaceSnap].available = true;
            // ground clamp: `movzx ecx,byte ptr [r14+X]; lea eax,[rcx-1]; test al,0FAh` after the drop pass
            if (uint8_t* m = Scan(fUpd, 0x1400, "41 0F B6 8E ?? ?? ?? ?? 8D 41 FF A8 FA")) g_groundTypeOffset = U32(m + 4);
            Log("surface snapping: ground type offset 0x%X", g_groundTypeOffset);
            if (!fDrop || !g_groundTypeOffset || g_groundTypeOffset >= 0x2000 ||
                !HookCalls("surface snapping: DropToGround", fDrop, wDrop, (void**)g_DropToGround, 3))
                Log("surface snapping: ground clamp not hooked (items that force a drop may still settle)");
        }
    } else Log("surface snapping: collector vtable/AddHit not found");

    // hold distance: `vmovss xmm0,[rcx+X]; vmovss xmm2,[rcx+X+4]; vmovss xmm3,[rcx+X+8]`
    if (uint8_t* f = Code(ID::FollowCamCastLength))
        if (uint8_t* m = Scan(f, 0x100, "C5 FA 10 81 ?? ?? ?? ?? C5 ?? 10 91 ?? ?? ?? ?? C5 ?? 10 99"))
            if (U32(m + 12) == U32(m + 4) + 4 && U32(m + 20) == U32(m + 4) + 8 && U32(m + 4) < 0x2000)
                g_holdOffset = U32(m + 4);
    const uint32_t lenIds[2] = { ID::SettingCastLenDefault, ID::SettingCastLenDecorate };
    const char* lenNames[2] = { "fWorkshopTempItemDefaultFollowCameraCastLength:Workshop",
                                "fWorkshopTempItemDecorateFollowCameraCastLength:Workshop" };
    for (int k = 0; k < 2; ++k)
        if (uint8_t* s = IniSetting(lenIds[k], lenNames[k])) g_castLen[k] = (float*)(s + 8);
    Log("hold distance: offset 0x%X, cast length settings %s", g_holdOffset,
        (g_castLen[0] && g_castLen[1]) ? "found" : "missing");

    // HUD messages
    uint8_t *src = Code(ID::HUDMessageSource), *ntf = Code(ID::EventNotify), *mk = Code(ID::FixedStringCtor),
            *rel = Code(ID::FixedStringRelease), *vt = Addr(ID::HUDNotifyFunctorVtbl);
    if (src && ntf && mk && rel && vt) {
        hud.source  = (decltype(hud.source))src;
        hud.notify  = (decltype(hud.notify))ntf;
        hud.make    = (decltype(hud.make))mk;
        hud.release = (decltype(hud.release))rel;
        hud.functorVtbl = vt;
    }
    Log(hud.source ? "HUD messages ready" : "HUD messages unavailable");

    // build mode check for hotkeys: the name getter must load the "WorkshopMenu" string
    uint8_t *ui = Addr(ID::UISingleton), *isOpen = Code(ID::IsMenuOpen), *nameFn = Code(ID::WorkshopMenuName);
    uint8_t* lea = nameFn ? Scan(nameFn, 0x80, "48 8D 15") : nullptr;
    const char* str = lea ? (const char*)(lea + 7 + I32(lea + 3)) : nullptr;
    if (ui && isOpen && str && !IsBadReadPtr(str, 13) && !strcmp(str, "WorkshopMenu")) {
        g_menu.ui = (void**)ui;
        g_menu.isOpen = (decltype(g_menu.isOpen))isOpen;
        g_menu.name = (decltype(g_menu.name))nameFn;
    }
    Log(g_menu.isOpen ? "hotkeys: build mode only (WorkshopMenu)" : "hotkeys: build mode check not found - keys work everywhere");

    // rotate with view: the follow-camera yaw store `vxorps xmm0,xmm0,xmm6; vmovss [r14+X],xmm0`
    if (uint8_t* m = Scan(fUpd, 0x800, "C5 F8 57 C6 C4 C1 7A 11 86 ?? ?? ?? ??"))
        if (U32(m + 9) < 0x2000) { g_yawOffset = U32(m + 9); g_yawStore = m; memcpy(g_yawStoreOrig, m, sizeof(g_yawStoreOrig)); }
    g_player = (uint8_t**)Addr(ID::PlayerSingleton);
    // snapped flag: in the snap handler, `cmp byte ptr [r14+X],0; je; vmovss xmm0,[rsp+..];
    // vaddss xmm1,xmm0,[r14+70h]; vmovss [r14+yaw],xmm1` (snap yaw + rotate input)
    if (CallerSet* cs = Callers(fSnap); cs && cs->count && g_yawOffset)
        if (uint8_t* m = Scan(cs->sites[0] - 0x600, 0x600,
                              "41 80 BE ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? C5 FA 10 44 24 ?? C4 C1 7A 58 4E 70 C4 C1 7A 11 8E"))
            if (U32(m + 31) == g_yawOffset && U32(m + 3) < 0x2000) g_snappedOffset = U32(m + 3);
    // re-read calls: `call atan2; vxorps; <store>` right before the store, and `call atan2; jmp <store>`
    bool reads = false;
    if (g_yawStore && g_yawStore[-5] == 0xE8) {
        uint8_t* atan = g_yawStore + I32(g_yawStore - 4);
        uint8_t* other = nullptr;
        for (uint8_t* q = fUpd; q + 10 <= fUpd + 0x1400 && !other; ++q)
            if (q != g_yawStore - 5 && q[0] == 0xE8 && q + 5 + I32(q + 1) == atan &&
                ((q[5] == 0xEB && q + 7 + (int8_t)q[6] == g_yawStore + 4) ||
                 (q[5] == 0xE9 && q + 10 + I32(q + 6) == g_yawStore + 4))) other = q;
        reads = other && HookOneCall("rotate with view: yaw re-read", g_yawStore - 5, (void*)&Hook_YawReadNeg, (void**)&g_yawRead[0]) &&
                HookOneCall("rotate with view: yaw re-read", other, (void*)&Hook_YawRead, (void**)&g_yawRead[1]);
    }
    g_toggles[kToggleRotate].available = g_rotReady = g_yawOffset && g_player && reads;

    // edit placed objects: StateInspect's target query
    //   mov rcx,[rip+picker]; mov ebx,[rip+lastTarget]; ... call [rax+aim]; cmp eax,[rip+null]; ... call [rax+alt]
    if (uint8_t* f = Code(ID::InspectTarget)) {
        uint8_t *a = Scan(f, 0x20, "48 8B 0D ?? ?? ?? ?? 8B 1D"), *v1 = Scan(f, 0x40, "FF 90 ?? ?? 00 00 8B 00 3B 05"),
                *v2 = Scan(f, 0x60, "FF 50 ?? 8B 00 3B 05");
        if (a && v1 && v2) {
            g_edit.picker     = (void**)(a + 7 + I32(a + 3));
            g_edit.lastTarget = (uint32_t*)(a + 13 + I32(a + 9));
            g_edit.vfAim      = U32(v1 + 2);
            g_edit.nullHandle = (uint32_t*)(v1 + 14 + I32(v1 + 10));
            g_edit.vfAlt      = v2[2];
        }
    }
    g_edit.lookup   = (decltype(g_edit.lookup))Code(ID::LookupRefByHandle);
    g_edit.release  = (decltype(g_edit.release))Code(ID::RefRelease);
    g_edit.move     = (decltype(g_edit.move))Code(ID::MoveRefrToPosition);
    g_edit.getScale = (decltype(g_edit.getScale))Code(ID::GetScale);
    if (uint8_t* f = Code(ID::SetScaleTask))   // `call qword ptr [rax+X]` with the scale argument
        if (uint8_t* m = Scan(f, 0x100, "FF 90 ?? ?? 00 00"))
            if (U32(m + 2) >= 0x300 && U32(m + 2) < 0x800 && !(U32(m + 2) & 7)) g_edit.vfSetScale = U32(m + 2);
    if (uint8_t* vt = Addr(ID::RefrVtbl)) {   // check the slots hold the functions, then call by slot
        void** v = (void**)vt;
        uint8_t* en = Code(ID::RefrEnable);   // Enable clears the disabled flag: btr ecx,0Bh early on
        if (Code(ID::RefrDisable) && v[0x7E8 / 8] == Code(ID::RefrDisable) && en && Scan(en, 0x80, "0F BA F1 0B")) {
            g_edit.vfDisable = 0x7E8; g_edit.enable = (decltype(g_edit.enable))en;
        }
    }
    g_edit.byId = (decltype(g_edit.byId))Code(ID::LookupFormByID);
    if (uint8_t* vt = Addr(ID::RefrVtbl); vt && g_edit.vfSetScale) {
        uint8_t* fs = ((uint8_t**)vt)[g_edit.vfSetScale / 8];
        bool ok = InText(fs) && Scan(fs, 0x80, "FF 90 40 03 00 00") && Scan(fs, 0x80, "80 F9 09") && Scan(fs, 0x80, "80 F9 38");
        for (int k = 0; k < 3; ++k) {
            g_edit.scaleAlways[k] = (void**)Addr(ID::ScaleAlways[k]);
            ok &= LoadsGlobal(fs, 0x80, g_edit.scaleAlways[k]) || [&] {   // cmp rax,[rip+form]
                for (uint8_t* q = fs; q + 7 <= fs + 0x80; ++q)
                    if (q[0] == 0x48 && q[1] == 0x3B && q[2] == 0x05 && q + 7 + I32(q + 3) == (uint8_t*)g_edit.scaleAlways[k]) return true;
                return false; }();
        }
        g_edit.scaleRule = ok;
    }
    Log("edit placed: scale rule %s", g_edit.scaleRule ? "verified" : "not verified (every piece treated as scalable)");
    // the list lock: the game's linked-children walker takes it with this call (lea rcx,[rbx+20h]; call)
    if (uint8_t* lr = Code(ID::LockRead); lr && CallsFn(Code(ID::ForEachLinkedChild), 0x40, lr) && Scan(lr, 0x20, "F0 0F C1"))
        g_edit.lockRead = (decltype(g_edit.lockRead))lr;
    if (HMODULE kb = GetModuleHandleA("kernelbase.dll"))
        g_edit.wake = (decltype(g_edit.wake))GetProcAddress(kb, "WakeByAddressSingle");
    Log("edit placed: extra-data read lock %s", g_edit.lockRead && g_edit.wake ? "found" : "not found (groups and foundations off)");
    Log("edit placed: collision rebuild after scaling %s", g_edit.vfDisable ? "available" : "not available");
    // groups: verify the globals against the game's own member search before using them
    g_edit.getExtra   = (decltype(g_edit.getExtra))Code(ID::GetExtraData);
    g_edit.linked     = (decltype(g_edit.linked))Code(ID::GetLinkedRef);
    g_edit.kwWorkshop = (void**)Addr(ID::KwWorkshopItem);
    g_edit.kwLink     = (void**)Addr(ID::KwPackinLink);
    g_edit.pivotDummy = (void**)Addr(ID::PackinPivotDummy);
    if (uint8_t* f = Code(ID::GroupMembers))
        g_edit.groups = LoadsGlobal(f, 0x100, g_edit.kwWorkshop) && LoadsGlobal(f, 0x100, g_edit.kwLink) &&
                        LoadsGlobal(f, 0x100, g_edit.pivotDummy) && CallsFn(f, 0x100, (void*)g_edit.getExtra) &&
                        CallsFn(f, 0x100, (void*)g_edit.linked) && Scan(f, 0x100, "B2 DA E8");
    g_edit.kwFoundation = (void**)Addr(ID::KwFoundation);
    if (!LoadsGlobal(Code(ID::ForEachFoundation), 0x100, g_edit.kwFoundation)) g_edit.kwFoundation = nullptr;
    Log("edit placed: foundations %s", g_edit.kwFoundation ? "follow" : "keyword not found (they stay put)");
    Log("edit placed: structure groups %s", g_edit.groups ? "found" : "not found (single objects only)");
    if (uint8_t* vt = Addr(ID::InspectVtbl))
        if (((void**)vt)[1] == Code(ID::InspectEnter) && ((void**)vt)[3] == Code(ID::InspectExit) && Code(ID::InspectEnter) &&
            ((void**)vt)[25] == Code(ID::InspectEditItem) && ((void**)vt)[13] == Code(ID::InspectMoveVariantA) &&
            ((void**)vt)[15] == Code(ID::InspectMoveVariantB))
            g_inspectSlots = (void**)vt;
    Log("edit placed: target query %s, move %s, scale vfunc 0x%X, inspect state %s",
        g_edit.picker ? "found" : "missing", g_edit.move && g_edit.lookup && g_edit.release ? "found" : "missing",
        g_edit.vfSetScale, g_inspectSlots ? "found" : "missing");
    Log("rotate with view: yaw 0x%X, snapped flag 0x%X, player %s, yaw re-reads %s", g_yawOffset,
        g_snappedOffset, g_player ? "found" : "missing", reads ? "hooked" : "not found");
    return true;
}

// Starfield Engine Fixes' Unrestricted Build Mode Placement makes every placement VALID inside
// the call No More Red wraps, so with it on the [Relax] list and the toggle can't block anything.
static void CheckOtherPlugins()
{
    if (!GetModuleHandleA("StarfieldEngineFixes.dll")) return;
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%sStarfieldEngineFixes.ini", g_dir);
    bool unrestricted = GetPrivateProfileIntA("Settings", "bUnrestrictedBuildModePlacement", 0, ini) != 0;

    Log("Starfield Engine Fixes loaded, bUnrestrictedBuildModePlacement=%d%s", unrestricted,
        unrestricted ? " - it allows every placement, so No More Red's [Relax] settings and toggle can't"
                       " block anything. Set it to 0 to use No More Red's codes." : "");
}

// Everything that touches game code or data another plugin might also change is applied here,
// after all plugins have loaded. Whatever another plugin already hooked is chained; whatever it
// replaced outright is left alone and the No More Red feature that needed it is turned off.
static void ApplyDeferred()
{
    static bool done = false;
    if (done) return;
    done = true;
    ApplyCallHooks();

    // modifier controls / numpad Enter: chain to whatever the game's GetRawInputData import points at now
    if (cfg.modifier || cfg.kAxis.vk == kVkNumpadEnter) {
        void** slot = FindImport("USER32.dll", "GetRawInputData");
        if (slot && Executable(*slot)) {
            char name[MAX_PATH];
            g_GetRawInputData = (decltype(g_GetRawInputData))*slot;
            void* hook = (void*)&Hook_GetRawInputData;
            Patch(slot, &hook, 8);
            g_rawInputSlot = slot;
            Log("modifier controls: raw input hooked (calls %s)", OwnerOf((uint8_t*)g_GetRawInputData, name, sizeof(name)));
        } else Log("raw input import not found - Ctrl + mouse / keys will also reach the game, Ctrl + wheel and numpad Enter won't work");
    }

    // edit placed objects: StateInspect Enter / Exit (vtable slots 1 / 3), chained like the rest
    if (g_inspectSlots && EditReady()) {
        void** slots[5] = { &g_inspectSlots[1], &g_inspectSlots[3], &g_inspectSlots[13], &g_inspectSlots[15], &g_inspectSlots[25] };
        void** next[5] = { (void**)&g_InspectEnter, (void**)&g_InspectExit,
                           (void**)&g_InspectMove[0], (void**)&g_InspectMove[1], (void**)&g_InspectMove[2] };
        void* hooks[5] = { (void*)&Hook_InspectEnter, (void*)&Hook_InspectExit,
                           (void*)&Hook_InspectMove<0>, (void*)&Hook_InspectMove<1>, (void*)&Hook_InspectMove<2> };
        bool ok = true;
        for (int i = 0; i < 5; ++i) ok &= Executable(*slots[i]);
        if (ok) {
            for (int i = 0; i < 5; ++i) { *next[i] = *slots[i]; Patch(slots[i], &hooks[i], 8); }
            Log("edit placed: ready (inspect state hooked)");
        } else { g_inspectSlots = nullptr; Log("edit placed: inspect state was replaced by another plugin - disabled"); }
    }

    if (g_addHitSlot) {
        uint8_t* cur = *g_addHitSlot;
        char name[MAX_PATH];
        if (cur != (uint8_t*)g_AddHit && Executable(cur)) {
            Log("surface snapping: AddHit already hooked by %s - chaining to it", OwnerOf(cur, name, sizeof(name)));
            g_AddHit = (decltype(g_AddHit))cur;
        } else if (cur != (uint8_t*)g_AddHit) {
            Log("surface snapping: AddHit was replaced by another plugin - surface snapping toggle disabled");
            g_addHitSlot = nullptr;
            g_toggles[kToggleSurfaceSnap].available = false;
            cfg.surfaceSnap = 1;
        }
        if (g_addHitSlot) { void* hook = (void*)&Hook_AddHit; Patch(g_addHitSlot, &hook, 8); }
    }

    if (g_tooTallWrite && memcmp(g_tooTallWrite, g_tooTallOrig, sizeof(g_tooTallOrig))) {
        Log("foundation too tall: flag write was changed by another plugin - left alone");
        g_tooTallWrite = nullptr;
    }
    UpdateTooTallPatch();

    if (g_yawStore && memcmp(g_yawStore, g_yawStoreOrig, sizeof(g_yawStoreOrig))) {
        Log("rotate with view: another plugin changes the held item's rotation - disabled");
        g_toggles[kToggleRotate].available = g_rotReady = false;
        cfg.rotateWithView = 0;
    }
    CheckOtherPlugins();
}

static void OnSFSEMessage(SFSEMessagingInterface::Message* msg)
{
    if (msg && msg->type == SFSEMessagingInterface::kMessage_PostPostLoad) ApplyDeferred();
}

// Log goes to Documents\My Games\Starfield\SFSE\Logs (next to sfse.txt), else next to the DLL.
static FILE* OpenLog()
{
    char p[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, p))) {
        static const char* const parts[] = { "\\My Games", "\\Starfield", "\\SFSE", "\\Logs" };
        for (const char* part : parts) {
            strncat(p, part, MAX_PATH - strlen(p) - 1);
            CreateDirectoryA(p, nullptr);
        }
        strncat(p, "\\NoMoreRed.log", MAX_PATH - strlen(p) - 1);
        if (FILE* f = fopen(p, "w")) return f;
    }
    snprintf(p, sizeof(p), "%sNoMoreRed.log", g_dir);
    return fopen(p, "w");
}

extern "C" __declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&SFSEPlugin_Load, &self);
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    if (char* s = strrchr(g_dir, '\\')) s[1] = 0;

    g_log = OpenLog();
    Log("No More Red - runtime %08X, sfse %08X", sfse->runtimeVersion, sfse->sfseVersion);

    LoadConfig();
    if (!Install(sfse->runtimeVersion)) return true;

    // Redirect the calls once every plugin has loaded; without messaging, do it now.
    auto msg = (SFSEMessagingInterface*)sfse->QueryInterface(kInterface_Messaging);
    if (msg && msg->RegisterListener(sfse->GetPluginHandle(), "SFSE", OnSFSEMessage))
        Log("call hooks will be applied after all plugins have loaded");
    else {
        Log("SFSE messaging unavailable - applying hooks now");
        ApplyDeferred();
    }

    if (auto task = (SFSETaskInterface*)sfse->QueryInterface(kInterface_Task)) {
        task->AddTaskPermanent((SFSETaskInterface::ITaskDelegate*)&g_pollTask);
        g_pollByTask = true;
        Log("hotkeys polled every frame (SFSE task)");
    } else Log("SFSE task interface unavailable - hotkeys only work while placing an item");
    return true;
}
