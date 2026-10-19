// No More Red - SFSE plugin for Starfield outpost building
// Address Library v2 (format 5); tested on Steam 1.16.244.
//
// Features (each one is set up on its own and skipped, with a log line, if it can't be verified)
//   No More Red      Relax placement failure codes chosen in the INI (Insert)
//   Object snapping  Hide snap points so items don't attach to other pieces (Home)
//   Surface snapping Precise Placement ignores every surface the crosshair hits (End)
//   Foundations      Don't build the auto-foundation pieces under structures (Delete)
//   Hold distance    Move the held item closer / further (Page Down / Page Up)
//   HUD messages     The game's own top-left notification for every toggle
//
// How the core works: build mode keeps one WorkshopCodes byte per placement attempt
// (RootWorkshopStruct+0x193 in 1.16.244, 2 = VALID). Anything else turns the preview red and
// the commit states refuse to build. EvaluatePlacement and UpdatePlacementStatus write that
// byte; their call sites are redirected to wrappers that call the original, then turn relaxed
// codes into VALID. Code numbers and struct offsets are read from the game's own code at
// startup, so a renumbered enum or moved field is followed or detected rather than guessed.

#include <windows.h>
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
    3,
    "No More Red",
    "prototype",
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

// Redirect every direct call to `target` into `wrapper`. Returns the number of call sites
// (0 = not hooked). `expect` is only logged, for comparing against 1.16.244.
static int HookCalls(const char* what, uint8_t* target, void* wrapper, int expect)
{
    if (CallerSet* cs = Callers(target)) {
        CallerSet& c = *cs;
        Log("%s: %p, %d caller(s) (1.16.244 had %d)", what, target, c.count, expect);
        if (!Hookable(target)) return 0;
        for (int k = 0; k < c.count; ++k) {
            uint8_t* stub = g_stubs + g_stubsUsed;
            g_stubsUsed += 16;
            uint8_t jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
            memcpy(jmp + 6, &wrapper, 8);
            memcpy(stub, jmp, sizeof(jmp));
            int32_t rel = (int32_t)(stub - (c.sites[k] + 5));
            Patch(c.sites[k] + 1, &rel, 4);
        }
        return c.count;
    }
    Log("%s: function not found", what);
    return 0;
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
    volatile LONG enabled = 1, objectSnap = 1, surfaceSnap = 1, foundations = 1;
    bool     showMessages = true;
    uint32_t statusOffset = 0;            // 0 = auto-detect
    float    distStep = 0.25f, distMin = 0.5f, distMax = 50.0f;
    Key      kEnabled{}, kObjectSnap{}, kSurfaceSnap{}, kFoundations{}, kFarther{}, kCloser{};
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
    { 22, "RestrictedArea",        0, "sWorkshopPlacement_PLACEMENT_RESTRICTED_AREA",   "Area is restricted" },
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
    cfg.kEnabled.vk     = I("General", "ToggleKey", 0x2D);              // Insert
    cfg.kObjectSnap.vk  = I("Snapping", "ObjectSnapToggleKey", 0x24);   // Home
    cfg.kSurfaceSnap.vk = I("Snapping", "SurfaceSnapToggleKey", 0x23);  // End
    cfg.kFoundations.vk = I("Foundations", "ToggleKey", 0x2E);          // Delete
    cfg.kFarther.vk     = I("Distance", "FartherKey", 0x21);            // Page Up
    cfg.kCloser.vk      = I("Distance", "CloserKey", 0x22);             // Page Down
    cfg.distStep = F("Distance", "Step", "0.25");
    cfg.distMin  = F("Distance", "Min", "0.5");
    cfg.distMax  = F("Distance", "Max", "50");
    cfg.statusOffset = I("Advanced", "StatusOffset", 0);
    for (auto& c : kCodes) c.want = I("Relax", c.key, c.def) != 0;
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

static void ShowHUD(const char* text)
{
    Log("%s", text);
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
// Call-site wrappers around EvaluatePlacement / UpdatePlacementStatus: run the original, then
// turn relaxed codes into VALID. With object snapping off, "must be snapped" is relaxed too,
// since there is nothing left to snap to.
static char (*g_EvaluatePlacement)(void*, void*, void*, void*) = nullptr;
static void (*g_UpdatePlacementStatus)(void*) = nullptr;
static bool g_pollByTask = false;
static void PollKeys();

static bool Relax(void* root, int site)
{
    uint8_t& code = *((uint8_t*)root + cfg.statusOffset);
    if (g_verbose) {
        static uint8_t last[2] = { 0xFF, 0xFF };
        if (code != last[site]) {
            Log("[%s] code %u%s", site ? "Update" : "Evaluate", code,
                (code != g_valid && g_relax[code]) ? (cfg.enabled ? " -> relaxed" : " (relaxable, mod off)") : "");
            last[site] = code;
        }
    }
    if (!cfg.enabled || code == g_valid) return false;
    bool relax = g_relax[code] || (!cfg.objectSnap && code != 0xFF &&
                                   (code == kCodes[kNoSnap].code || code == kCodes[kNoSnapFloor].code));
    if (relax) code = g_valid;
    return relax;
}

static char Hook_EvaluatePlacement(void* root, void* a2, void* a3, void* a4)
{
    if (!g_pollByTask) PollKeys();
    char ok = g_EvaluatePlacement(root, a2, a3, a4);
    return (root && Relax(root, 0)) ? 1 : ok;
}

static void Hook_UpdatePlacementStatus(void* root)
{
    g_UpdatePlacementStatus(root);
    if (root) Relax(root, 1);
}

// ================================================================ object snapping
// GetSnapCandidates builds the list of snap points the held item can attach to; an empty list
// is a normal state the game handles.
static int64_t* (*g_GetSnapCandidates)(int64_t*, void*, void*, void*) = nullptr;

static int64_t* Hook_GetSnapCandidates(int64_t* out, void* root, void* a3, void* a4)
{
    if (!cfg.objectSnap) { out[0] = out[1] = out[2] = 0; return out; }
    return g_GetSnapCandidates(out, root, a3, a4);
}

// ================================================================ foundations
// FoundationCount turns a foundation height into the number of pieces (base + middles + top).
// AutoFoundationHeight calls it for the new count (with an out-height) and once for the count
// already built (out-height null); returning 0 for the new count makes the game remove the
// built pieces and build none.
// AutoFoundationHeight also sets the "foundation too tall" flag (status-1) from its height
// cast, and the game colours the preview red from it before our wrappers run, so that one
// flag write is NOP'd while No More Red relaxes FoundationTooTall or foundations are off.
static uint32_t (*g_FoundationCount)(void*, void*, float, char, void*, float*) = nullptr;
static uint8_t* g_tooTallWrite = nullptr;
static uint8_t  g_tooTallOrig[8];
static bool     g_tooTallNopped = false;

static uint32_t Hook_FoundationCount(void* data, void* parts, float height, char flag, void* a5, float* outHeight)
{
    if (!cfg.foundations && outHeight) { *outHeight = 0.0f; return 0; }
    return g_FoundationCount(data, parts, height, flag, a5, outHeight);
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
// distance; the cast then misses, and on a miss the drop-to-ground pass is skipped by storing
// hit normal.z = 0 instead of FLT_MAX. Some items (structures) force the drop pass anyway, so
// after it runs the ground type it stored is cleared; UpdatePlacementStatus only clamps the
// item's height to the ground for ground types 1/2/6. SurfaceCast also tells us which item is held.
static uint64_t (*g_SurfaceCast)(void*, void*) = nullptr;
static void     (*g_AddHit)(void*, void*) = nullptr;
static uint32_t  g_normalZOffset = 0;
static DWORD     g_castThread = 0;
static void     (*g_DropToGround)(void*, void*, void*) = nullptr;
static uint32_t  g_groundTypeOffset = 0;
static void*     g_heldRoot = nullptr;
static ULONGLONG g_heldTick = 0;

static void Hook_AddHit(void* collector, void* hit)
{
    if (!cfg.surfaceSnap && g_castThread == GetCurrentThreadId()) return;
    g_AddHit(collector, hit);
}

static uint64_t Hook_SurfaceCast(void* root, void* a2)
{
    g_heldRoot = root;
    g_heldTick = GetTickCount64();
    g_castThread = GetCurrentThreadId();
    uint64_t hit = g_SurfaceCast(root, a2);
    g_castThread = 0;
    if (!cfg.surfaceSnap && root && (hit & 0xFF) == 0)
        *(float*)((uint8_t*)root + g_normalZOffset) = 0.0f;
    return hit;
}

static void Hook_DropToGround(void* root, void* a2, void* a3)
{
    g_DropToGround(root, a2, a3);   // also releases the refs passed in a2/a3, so always call it
    if (!cfg.surfaceSnap && root) *((uint8_t*)root + g_groundTypeOffset) = 0;
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
    if (!g_holdOffset || !g_heldRoot || GetTickCount64() - g_heldTick > 500) return;   // nothing held
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

// ================================================================ hotkeys
// Polled once per frame on the main thread by an SFSE task; keys only act while the game has
// focus, but their state is tracked regardless so alt-tabbing back with a key held isn't a press.
struct Toggle { Key* key; volatile LONG* state; const char* label; bool available; void (*changed)(); };
static Toggle g_toggles[] = {
    { &cfg.kEnabled,     &cfg.enabled,     "NO MORE RED",      false, UpdateTooTallPatch },
    { &cfg.kObjectSnap,  &cfg.objectSnap,  "OBJECT SNAPPING",  false, nullptr },
    { &cfg.kSurfaceSnap, &cfg.surfaceSnap, "SURFACE SNAPPING", false, nullptr },
    { &cfg.kFoundations, &cfg.foundations, "FOUNDATIONS",      false, UpdateTooTallPatch },
};
enum { kToggleEnabled, kToggleObjectSnap, kToggleSurfaceSnap, kToggleFoundations };

// Fires on press; with `repeat`, again every 50 ms after 300 ms held.
static bool Fired(Key& k, bool repeat)
{
    if (!k.vk) return false;
    bool d = (GetAsyncKeyState(k.vk) & 0x8000) != 0;
    ULONGLONG now = GetTickCount64();
    bool fire = d && (!k.down || (repeat && now - k.since > 300 && now - k.last >= 50));
    if (d && !k.down) k.since = now;
    if (fire) k.last = now;
    k.down = d;
    return fire;
}

static void PollKeys()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool focused = pid == GetCurrentProcessId();
    for (auto& t : g_toggles) {
        if (!Fired(*t.key, false) || !focused || !t.available) continue;
        bool on = (InterlockedXor(t.state, 1) ^ 1) != 0;
        if (t.changed) t.changed();
        char msg[64];
        snprintf(msg, sizeof(msg), "%s: %s", t.label, on ? "ON" : "OFF");
        ShowHUD(msg);
    }
    if (Fired(cfg.kFarther, true) && focused) AdjustHoldDistance(+1.0f);
    if (Fired(cfg.kCloser, true) && focused)  AdjustHoldDistance(-1.0f);
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
        if (c.code != 0xFF) g_relax[c.code] = c.want;
        Log("  [%3u] %-22s %s  (%s)", c.code, c.key, (c.code != 0xFF && c.want) ? "RELAX" : "keep ", c.desc);
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
    g_EvaluatePlacement     = (decltype(g_EvaluatePlacement))fEval;
    g_UpdatePlacementStatus = (decltype(g_UpdatePlacementStatus))fUpd;
    if (!Hookable(fEval) || !Hookable(fUpd)) {   // check both before patching either
        Log("core functions have unexpected callers (%d / %d) - nothing patched",
            Callers(fEval) ? Callers(fEval)->count : 0, Callers(fUpd) ? Callers(fUpd)->count : 0);
        return false;
    }
    HookCalls("EvaluatePlacement", fEval, (void*)&Hook_EvaluatePlacement, 2);
    HookCalls("UpdatePlacementStatus", fUpd, (void*)&Hook_UpdatePlacementStatus, 2);
    g_toggles[kToggleEnabled].available = true;

    // object snapping
    g_GetSnapCandidates = (decltype(g_GetSnapCandidates))fSnap;
    g_toggles[kToggleObjectSnap].available =
        fSnap && HookCalls("object snapping: GetSnapCandidates", fSnap, (void*)&Hook_GetSnapCandidates, 2);

    // foundations: piece count hook + "too tall" flag write (status-1, right before the status byte)
    g_FoundationCount = (decltype(g_FoundationCount))fCount;
    g_toggles[kToggleFoundations].available =
        fCount && HookCalls("foundations: FoundationCount", fCount, (void*)&Hook_FoundationCount, 3);
    if (uint8_t* f = Code(ID::AutoFoundationHeight))
        if (uint8_t* m = Scan(f, 0x600, "80 7D ?? 00 74 08 41 C6 87 ?? ?? ?? ?? 01"))
            if (U32(m + 9) == cfg.statusOffset - 1) { g_tooTallWrite = m + 6; memcpy(g_tooTallOrig, g_tooTallWrite, 8); }
    Log(g_tooTallWrite ? "foundation too tall: flag write at %p" : "foundation too tall: flag write not found", g_tooTallWrite);
    UpdateTooTallPatch();

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
        g_SurfaceCast = (decltype(g_SurfaceCast))fCast;
        if (g_normalZOffset && HookCalls("surface snapping: SurfaceCast", fCast, (void*)&Hook_SurfaceCast, 1)) {
            g_AddHit = (decltype(g_AddHit))fAdd;
            void* hook = (void*)&Hook_AddHit;
            Patch(vtbl + 4 * 8, &hook, 8);
            g_toggles[kToggleSurfaceSnap].available = true;
            // ground clamp: `movzx ecx,byte ptr [r14+X]; lea eax,[rcx-1]; test al,0FAh` after the drop pass
            if (uint8_t* m = Scan(fUpd, 0x1400, "41 0F B6 8E ?? ?? ?? ?? 8D 41 FF A8 FA")) g_groundTypeOffset = U32(m + 4);
            g_DropToGround = (decltype(g_DropToGround))fDrop;
            Log("surface snapping: ground type offset 0x%X", g_groundTypeOffset);
            if (!fDrop || !g_groundTypeOffset || g_groundTypeOffset >= 0x2000 ||
                !HookCalls("surface snapping: DropToGround", fDrop, (void*)&Hook_DropToGround, 3))
                Log("surface snapping: ground clamp not hooked (items that force a drop may still settle)");
        }
    } else Log("surface snapping: collector vtable/AddHit not found");

    // hold distance: `vmovss xmm0,[rcx+X]; vmovss xmm2,[rcx+X+4]; vmovss xmm3,[rcx+X+8]`
    // (needs the SurfaceCast hook to know which item is held)
    if (uint8_t* f = Code(ID::FollowCamCastLength); f && g_toggles[kToggleSurfaceSnap].available)
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
    return true;
}

extern "C" __declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&SFSEPlugin_Load, &self);
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    if (char* s = strrchr(g_dir, '\\')) s[1] = 0;

    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%sNoMoreRed.log", g_dir);
    g_log = fopen(logPath, "w");
    Log("No More Red - runtime %08X, sfse %08X", sfse->runtimeVersion, sfse->sfseVersion);

    LoadConfig();
    if (!Install(sfse->runtimeVersion)) return true;

    if (auto task = (SFSETaskInterface*)sfse->QueryInterface(kInterface_Task)) {
        task->AddTaskPermanent((SFSETaskInterface::ITaskDelegate*)&g_pollTask);
        g_pollByTask = true;
        Log("hotkeys polled every frame (SFSE task)");
    } else Log("SFSE task interface unavailable - hotkeys only work while placing an item");
    return true;
}
