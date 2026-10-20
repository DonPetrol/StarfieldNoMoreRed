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
//   Fine rotate      Turn the held item by a small step (Ctrl + Left / Right arrow)
//   Hold distance    Move the held item closer / further (Ctrl + Down / Up arrow)
//                    While Ctrl is held in build mode those arrow keys don't reach the game's menus.
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
    0x01010000,   // 1.1.0
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
    int      modifier = 0x11;                // held for the distance / fine rotate keys (0 = none)
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
    cfg.kFineLeft.vk    = I("Rotation", "FineRotateLeftKey", 0x25);     // Left arrow
    cfg.kFineRight.vk   = I("Rotation", "FineRotateRightKey", 0x27);    // Right arrow
    cfg.fineStep        = F("Rotation", "FineRotateStep", "1.0");
    cfg.kFarther.vk     = I("Distance", "FartherKey", 0x26);            // Up arrow
    cfg.kCloser.vk      = I("Distance", "CloserKey", 0x28);             // Down arrow
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

template <int N> static void Hook_UpdatePlacementStatus(void* root)
{
    if (root) { g_heldRoot = root; g_heldTick = GetTickCount64(); }
    bool rotate = RotateBefore(root);
    if (rotate) g_rotThread = GetCurrentThreadId();
    g_UpdatePlacementStatus[N](root);
    g_rotThread = 0;
    RestoreGroundType(root);
    if (root) {
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

// Fine rotate: turn the held item's own yaw by a step; the next update applies it. Not for pieces
// snapped to another piece (the game sets their yaw from the snap point).
static void FineRotate(float dir)
{
    if (!g_rotReady || !g_heldRoot || GetTickCount64() - g_heldTick > 500) return;   // nothing held
    if (g_snappedOffset && *((uint8_t*)g_heldRoot + g_snappedOffset)) return;
    g_ownYaw = WrapPi(g_ownYaw + dir * cfg.fineStep * (kPi / 180.0f));
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

// ---- modifier keys: the distance / fine rotate keys only act with the modifier (Ctrl) held, and
// while it is held in build mode the game doesn't get those keys (its menus would navigate with
// the arrows). The game reads the keyboard with raw input (USER32 GetRawInputData, one import);
// that import is redirected and, while blocking, those keys' events come back as "no key".
static volatile bool g_blockKeys = false;
static UINT (WINAPI* g_GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT) = nullptr;
static void** g_rawInputSlot = nullptr;

static UINT WINAPI Hook_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    UINT r = g_GetRawInputData(h, cmd, data, size, header);
    if (cmd != RID_INPUT || !data || r == (UINT)-1 || r < sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)) return r;
    RAWINPUT* ri = (RAWINPUT*)data;
    if (ri->header.dwType != RIM_TYPEKEYBOARD || !ri->data.keyboard.VKey) return r;
    const int vk = ri->data.keyboard.VKey, keys[4] = { cfg.kFarther.vk, cfg.kCloser.vk, cfg.kFineLeft.vk, cfg.kFineRight.vk };
    static bool swallowed[4] = {};   // key-down was hidden, so hide its key-up too (and only then)
    for (int i = 0; i < 4; ++i) {
        if (vk != keys[i]) continue;
        bool up = (ri->data.keyboard.Flags & RI_KEY_BREAK) != 0;
        bool hide = up ? swallowed[i] : g_blockKeys;
        if (!up) swallowed[i] = hide;
        else swallowed[i] = false;
        if (hide) { ri->data.keyboard.VKey = 0xFF; ri->data.keyboard.MakeCode = 0; }
        break;
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

static void PollKeys()
{
    // Every key's state is read every poll so a key held from before never counts as a press.
    bool fired[sizeof(g_toggles) / sizeof(g_toggles[0])], any = false;
    for (size_t i = 0; i < sizeof(g_toggles) / sizeof(g_toggles[0]); ++i) any |= fired[i] = Fired(*g_toggles[i].key, false);
    bool mod = ModifierDown();
    bool farther = Fired(cfg.kFarther, true) && mod, closer = Fired(cfg.kCloser, true) && mod;
    bool left = Fired(cfg.kFineLeft, true) && mod, right = Fired(cfg.kFineRight, true) && mod;

    bool act = any || farther || closer || left || right;
    bool wantBlock = g_rawInputSlot && cfg.modifier && mod;
    if (!act && !wantBlock) { g_blockKeys = false; return; }
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool ready = pid == GetCurrentProcessId() && InBuildMode();   // focused, in build mode
    g_blockKeys = wantBlock && ready;
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
    if (farther) AdjustHoldDistance(+1.0f);
    if (closer)  AdjustHoldDistance(-1.0f);
    if (left)    FineRotate(-1.0f);
    if (right)   FineRotate(+1.0f);
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

    // modifier key blocking: chain to whatever the game's GetRawInputData import points at now
    if (cfg.modifier) {
        void** slot = FindImport("USER32.dll", "GetRawInputData");
        if (slot && Executable(*slot)) {
            char name[MAX_PATH];
            g_GetRawInputData = (decltype(g_GetRawInputData))*slot;
            void* hook = (void*)&Hook_GetRawInputData;
            Patch(slot, &hook, 8);
            g_rawInputSlot = slot;
            Log("modifier keys: raw input hooked (calls %s)", OwnerOf((uint8_t*)g_GetRawInputData, name, sizeof(name)));
        } else Log("modifier keys: raw input import not found - arrow keys will also reach the game");
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
