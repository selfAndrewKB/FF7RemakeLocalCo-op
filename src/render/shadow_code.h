#pragma once

// The machine code of the per-view shadow levers (render/shadows_per_view; RE-shadows-per-view-build.md), built here as
// pure byte arrays and executed over stand-ins on the host (tests/shadow_code_test.cpp).
//
// Every site is one instruction inside one aligned 16-byte block of the renderer (platform::CodeRedirect); each stub
// re-does the replaced instruction, makes no call and keeps rsp. They read one state block of the mod's (switches and
// counters) whose address is built into them. B and C keep the game's code path exactly while their switch is off or
// the family has one view; A's stubs, switched on, write the view's own set: in one family of several views the record
// of the view's index, and while one family per player is drawn (render/family_now.h) the record of the family's place,
// each the game's set when the record's view state is not the view's.
//
// Lever B, a lamp's shadow per view (CreateWholeSceneProjectedShadow and InitProjectedShadowVisibility): the game drops
// a lamp's whole-scene shadow for every view when any view is beyond the lamp's shadow cutoff. The byte the view loop
// gathers becomes two bits: 1 some view beyond (the game's meaning), 2 some view that sees the lamp within it; the shadow
// is dropped only when it is exactly 1, and each view beyond the cutoff skips that shadow, as its player alone would.
// Lever C, the moving casters (the same function): a character casts a lamp's shadow only if it is within the cutoff of
// player 1's camera (Views[0]); the stub hands the game the view nearest the caster instead.
// Lever A, each view's area light settings: the scene holds one set of light-distance overrides (and the sun's cascade
// distances), the last view's pushed (render/area_chains.h keeps one chain per view); the A stub writes the view's own
// set into the scene at the head of each (light, view) pass of the light-LOD loop, the sun stub the view's own cascade
// fields into the sun's proxy at each view of the sun's cascade setup.
// A-fog, each view's height fog: the scene's first height fog holds the last view's blend of the fog controllers (the
// game's setter writes every field at each push, so a view's fog is its own last push: no chain); the fog stub writes
// the view's own into it at each view of InitFogConstants, the loop that turns it into that view's fog parameters.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

namespace lss::render::shadow_code {

// --- The state block the stubs read (game data: written by the mod's threads, read by the render thread) -------------

enum Lever : std::size_t
{
    kAreas = 0,  // A: each view's light-distance overrides
    kSun,        // A: each view's sun cascades
    kFog,        // A: each view's height fog
    kLamps,      // B
    kCasters,    // C
    kLevers
};

/// One view's (or the game's) light settings as the scene and the sun's proxy hold them after its pushes.
struct alignas(16) ViewRec
{
    std::uint8_t  valid = 0;
    std::uint8_t  pad[7]{};
    const void*   scene = nullptr;  // FScene*
    const void*   state = nullptr;  // the view's FSceneViewStateInterface* (FSceneView +0x8): a player's view, not a capture's
    const void*   sun   = nullptr;  // the scene's sun proxy when snapshot
    std::uint8_t  sceneFields[24]{};  // FScene +0x1608..+0x161F: sun off, the three cutoff overrides
    std::uint32_t sunDistance = 0;    // proxy +0x1A8
    std::uint32_t cascades[5]{};      // proxy +0x1C0..+0x1D3
    std::uint32_t biasSharpen[2]{};   // proxy +0xDC, +0xE0
};
static_assert(offsetof(ViewRec, scene) == 0x08 && offsetof(ViewRec, state) == 0x10 && offsetof(ViewRec, sun) == 0x18);
static_assert(offsetof(ViewRec, sceneFields) == 0x20 && offsetof(ViewRec, sunDistance) == 0x38);
static_assert(offsetof(ViewRec, cascades) == 0x3C && offsetof(ViewRec, biasSharpen) == 0x50 && sizeof(ViewRec) == 0x60);

/// One view's (or the game's) height fog as the scene's first fog holds it after that view's push: the fields the game's
/// setter writes (FScene::SetHeightFogControllerBlendData, render thread).
struct alignas(16) FogRec
{
    std::uint8_t  valid = 0;
    std::uint8_t  pad[7]{};
    const void*   fog   = nullptr;  // the scene's first fog (ExponentialFogs' data) when snapshot
    const void*   state = nullptr;  // the view's state (from that view's area light chain: the fog push carries no room)
    const void*   scene = nullptr;
    std::uint8_t  heightToOpacity[16]{};  // fog +0x08..+0x17: height, density, height falloff, max opacity
    std::uint8_t  inscattering[16]{};     // fog +0x24..+0x33: the inscattering colour
    std::uint8_t  appendix[16]{};         // fog +0x9C..+0xAB: Square Enix's second colour
    std::uint32_t lastField = 0;          // fog +0xC0
    std::uint8_t  pad2[4]{};
    const void*   component = nullptr;    // fog +0x00: the fog component it belongs to (the array appends and compacts,
                                          // so the first fog's address can hold another fog)
};
static_assert(offsetof(FogRec, fog) == 0x08 && offsetof(FogRec, state) == 0x10 && offsetof(FogRec, heightToOpacity) == 0x20);
static_assert(offsetof(FogRec, inscattering) == 0x30 && offsetof(FogRec, appendix) == 0x40 && offsetof(FogRec, lastField) == 0x50);
static_assert(offsetof(FogRec, component) == 0x58 && sizeof(FogRec) == 0x60);

inline constexpr std::size_t   kRecords          = 4;  // views 0..3 of the family; more take the game's set
inline constexpr std::uint32_t kSceneFields      = 0x1608;
inline constexpr std::uint32_t kSceneSunLight    = 0x1620;  // FLightSceneInfo* of the sun the controllers drive
inline constexpr std::uint32_t kLightProxy       = 0x30;
inline constexpr std::uint32_t kSunDistance      = 0x1A8;
inline constexpr std::uint32_t kSunCascades      = 0x1C0;
inline constexpr std::uint32_t kSunBiasSharpen   = 0xDC;
inline constexpr std::uint32_t kViewState        = 0x08;
inline constexpr std::uint32_t kSceneFogs        = 0x1DC0;  // ExponentialFogs: data, then the count at +0x1DC8
inline constexpr std::uint32_t kSceneFogCount    = 0x1DC8;
/// The fog fields the setter writes, as {offset in the fog, size}: FogRec keeps them in this order from +0x20.
inline constexpr std::uint32_t kFogFieldSpans[4][2] = {{0x08, 16}, {0x24, 16}, {0x9C, 16}, {0xC0, 4}};
static_assert(sizeof(FogRec::heightToOpacity) == kFogFieldSpans[0][1] && sizeof(FogRec::inscattering) == kFogFieldSpans[1][1] &&
              sizeof(FogRec::appendix) == kFogFieldSpans[2][1] && sizeof(FogRec::lastField) == kFogFieldSpans[3][1]);

/// A record's copies of the fog fields, in kFogFieldSpans' order (the stub copies the same spans from +0x20).
inline std::uint8_t* FogField(FogRec& r, std::size_t i) noexcept
{
    std::uint8_t* const fields[4] = {r.heightToOpacity, r.inscattering, r.appendix, reinterpret_cast<std::uint8_t*>(&r.lastField)};
    return fields[i];
}

struct alignas(16) State
{
    std::uint8_t  on[8]{};          // by Lever: 1 = per view
    std::uint64_t beyondViews = 0;  // B: (lamp, view) pairs beyond the lamp's shadow cutoff
    std::uint64_t withinViews = 0;  // B: (lamp, view) pairs that see the lamp within it
    std::uint64_t kept        = 0;  // B: lamp shadows built because a view needs them, another being beyond
    std::uint64_t hidden      = 0;  // B: (shadow, view) pairs skipped in a view beyond the cutoff
    std::uint64_t nearCasters = 0;  // C: casters judged from a view other than player 1's
    std::uint64_t areaOwn     = 0;  // A: (light, view) passes with the view's own set
    std::uint64_t areaGame    = 0;  // A: passes with the game's (no record for the view)
    std::uint64_t sunOwn      = 0;  // A-sun: views with their own cascade fields
    std::uint64_t sunGame     = 0;
    ViewRec       game;              // what the scene would hold without the mod (render/area_chains.h)
    ViewRec       records[kRecords];
    std::uint64_t fogOwn      = 0;  // A-fog: views given their own fog
    std::uint64_t fogShared   = 0;  // A-fog: views given the game's (no record of their own)
    FogRec        fogGame;           // the fog after the last push: what the scene holds without the mod
    FogRec        fogs[kRecords];
};
static_assert(offsetof(State, withinViews) == offsetof(State, beyondViews) + 8);  // the B counters are indexed by 1, 2
static_assert(offsetof(State, beyondViews) >= 8);

// --- A small assembler: bytes, rel32 labels, absolute jumps ---------------------------------------------------------

class Code
{
public:
    struct Label
    {
        std::ptrdiff_t           at = -1;
        std::vector<std::size_t> uses;  // rel32 fields that jump to it
    };

    void Put(std::initializer_list<std::uint8_t> b) { bytes_.insert(bytes_.end(), b); }
    void Put(std::span<const std::uint8_t> b) { bytes_.insert(bytes_.end(), b.begin(), b.end()); }
    template <class T>
    void Value(const T& v)
    {
        std::uint8_t raw[sizeof(T)];
        std::memcpy(raw, &v, sizeof(T));
        bytes_.insert(bytes_.end(), raw, raw + sizeof(T));
    }
    void Disp32(std::size_t d) { Value(static_cast<std::int32_t>(d)); }

    /// jcc rel32 (`cc` is the short form's opcode, 0x70..0x7F) and jmp rel32.
    void Jcc(std::uint8_t cc, Label& to)
    {
        Put({0x0F, static_cast<std::uint8_t>(cc + 0x10)});
        Use(to);
    }
    void Jmp(Label& to)
    {
        Put({0xE9});
        Use(to);
    }
    /// jmp [rip+0] to an absolute address (no register, no stack).
    void JmpAbsolute(std::uint64_t to)
    {
        Put({0xFF, 0x25, 0, 0, 0, 0});
        Value(to);
    }
    void Bind(Label& label)
    {
        label.at = static_cast<std::ptrdiff_t>(bytes_.size());
        for (const std::size_t use : label.uses)
        {
            Patch(use, label.at);
        }
    }
    /// The bytes; empty if a label was used but never bound.
    std::vector<std::uint8_t> Finish(std::initializer_list<const Label*> labels) const
    {
        for (const Label* label : labels)
        {
            if (label->at < 0 && !label->uses.empty())
            {
                return {};
            }
        }
        return bytes_;
    }

private:
    void Use(Label& to)
    {
        const std::size_t field = bytes_.size();
        Value(std::int32_t{0});
        if (to.at >= 0)
        {
            Patch(field, to.at);
        }
        else
        {
            to.uses.push_back(field);
        }
    }
    void Patch(std::size_t field, std::ptrdiff_t target)
    {
        const auto rel = static_cast<std::int32_t>(target - static_cast<std::ptrdiff_t>(field + 4));
        std::memcpy(&bytes_[field], &rel, sizeof(rel));
    }
    std::vector<std::uint8_t> bytes_;
};

inline constexpr std::uint8_t kJe = 0x74, kJne = 0x75, kJl = 0x7C, kJae = 0x73, kJbe = 0x76, kJns = 0x79;

inline void MovRcxImm(Code& c, const void* p)
{
    c.Put({0x48, 0xB9});
    c.Value(reinterpret_cast<std::uint64_t>(p));
}

// --- Lever B --------------------------------------------------------------------------------------------------------

/// The view loop's gather of the beyond-cutoff bit (FSceneRenderer::CreateWholeSceneProjectedShadow, SE light LOD):
/// add rcx,[rsi+r14+17B8h] (rcx = this view's VisibleLightInfos[light]), then mov eax,[rcx+40h] · mov rcx,[r15+30h] ·
/// shr eax,4 · and al,1 · or dl,al · mov rax,[rcx] · mov [rsp+42h],dl, which the stub re-does. Flags at +0x40: bit 0
/// the light in the view's frustum, bit 4 the view beyond the light's shadow cutoff (set only for lights in it).
inline constexpr std::uintptr_t kGatherSite   = 0x1425F20B6;
inline constexpr std::uintptr_t kGatherResume = 0x1425F20D3;
inline constexpr std::uint8_t   kGatherAdd[]  = {0x4A, 0x03, 0x8C, 0x36, 0xB8, 0x17, 0x00, 0x00};
/// The replaced instruction and the six it re-does, as the research read them (checked at install).
inline constexpr std::uint8_t kGatherBytes[] = {0x4A, 0x03, 0x8C, 0x36, 0xB8, 0x17, 0x00, 0x00, 0x8B, 0x41, 0x40, 0x49, 0x8B,
                                                0x4F, 0x30, 0xC1, 0xE8, 0x04, 0x24, 0x01, 0x0A, 0xD0, 0x48, 0x8B, 0x01, 0x88,
                                                0x54, 0x24, 0x42};
static_assert(sizeof(kGatherBytes) == kGatherResume - kGatherSite);

inline std::vector<std::uint8_t> GatherCode(State& s)
{
    Code        c;
    Code::Label game, notBeyond, count, none, merge;
    c.Put(kGatherAdd);                                              // add rcx,[rsi+r14+17B8h]   (replaced)
    c.Put({0x8B, 0x41, 0x40});                                      // mov eax,[rcx+40h]          the view's flags
    c.Put({0x41, 0x83, 0xBC, 0x24, 0xD8, 0x00, 0x00, 0x00, 0x02});  // cmp dword [r12+0D8h],2     views
    c.Jcc(kJl, game);                                               // one view: the game's
    MovRcxImm(c, &s);                                               // mov rcx,&state
    c.Put({0x80, 0xB9});                                            // cmp byte [rcx+on[kLamps]],0
    c.Disp32(offsetof(State, on) + kLamps);
    c.Put({0x00});
    c.Jcc(kJe, game);
    c.Put({0xA8, 0x10});  // test al,10h               beyond the cutoff?
    c.Jcc(kJe, notBeyond);
    c.Put({0xB8, 0x01, 0x00, 0x00, 0x00});  // mov eax,1
    c.Jmp(count);
    c.Bind(notBeyond);
    c.Put({0xA8, 0x01});  // test al,1                 the lamp in this view's frustum?
    c.Jcc(kJe, none);
    c.Put({0xB8, 0x02, 0x00, 0x00, 0x00});  // mov eax,2
    c.Bind(count);
    c.Put({0x48, 0xFF, 0x84, 0xC1});  // inc qword [rcx+rax*8+beyondViews-8]
    c.Disp32(offsetof(State, beyondViews) - 8);
    c.Jmp(merge);
    c.Bind(none);
    c.Put({0x31, 0xC0});  // xor eax,eax               this view neither needs nor drops it
    c.Jmp(merge);
    c.Bind(game);
    c.Put({0xC1, 0xE8, 0x04});  // shr eax,4
    c.Put({0x83, 0xE0, 0x01});  // and eax,1                1 = beyond (the game's)
    c.Bind(merge);
    c.Put({0x0A, 0xD0});                    // or dl,al
    c.Put({0x49, 0x8B, 0x4F, 0x30});        // mov rcx,[r15+30h]
    c.Put({0x48, 0x8B, 0x01});              // mov rax,[rcx]
    c.Put({0x88, 0x54, 0x24, 0x42});        // mov [rsp+42h],dl
    return c.Finish({&game, &notBeyond, &count, &none, &merge});
}

/// The decision: cmp byte [rsp+42h],0 · cmovnz ecx,r13d (r13d = 0: no shadow to build). The stub replaces the compare
/// and does the conditional move itself, then resumes after it.
inline constexpr std::uintptr_t kDecideSite    = 0x1425F2448;
inline constexpr std::uintptr_t kDecideResume  = 0x1425F2451;
inline constexpr std::uint8_t   kDecideCompare[] = {0x80, 0x7C, 0x24, 0x42, 0x00};
inline constexpr std::uint8_t   kDecideBytes[]   = {0x80, 0x7C, 0x24, 0x42, 0x00, 0x41, 0x0F, 0x45, 0xCD};
static_assert(sizeof(kDecideBytes) == kDecideResume - kDecideSite);

inline std::vector<std::uint8_t> DecideCode(State& s)
{
    Code        c;
    Code::Label decide;
    c.Put({0x48, 0xB8});  // mov rax,&state           (rax is dead here)
    c.Value(reinterpret_cast<std::uint64_t>(&s));
    c.Put({0x80, 0x7C, 0x24, 0x42, 0x03});  // cmp byte [rsp+42h],3   beyond in one view, needed in another
    c.Jcc(kJne, decide);
    c.Put({0x48, 0xFF, 0x80});  // inc qword [rax+kept]
    c.Disp32(offsetof(State, kept));
    c.Bind(decide);
    c.Put({0x80, 0x7C, 0x24, 0x42, 0x01});  // cmp byte [rsp+42h],1   dropped only when no view needs it
    c.Put({0x41, 0x0F, 0x44, 0xCD});        // cmovz ecx,r13d
    return c.Finish({&decide});
}

/// InitProjectedShadowVisibility's per-view test of a shadow: add rsi,[rbx+17B8h] (rsi = this view's
/// VisibleLightInfos[light]); the game then tests the frustum bit and jumps to 0x1425F35FA ("not visible in this view,
/// next view") when it is clear. The stub sends a view beyond the cutoff there for a lamp's whole-scene shadow.
/// FProjectedShadowInfo +0x568: bit 4 directional, bit 6 whole-scene [ASSUMPTION: stock 4.18's bit order; bits 8, 9 and 14
/// confirmed by their use in this build].
inline constexpr std::uintptr_t kHideSite       = 0x1425F3418;
inline constexpr std::uintptr_t kHideResume     = 0x1425F341F;
inline constexpr std::uintptr_t kHideNotVisible = 0x1425F35FA;
inline constexpr std::uint8_t   kHideAdd[]      = {0x48, 0x03, 0xB3, 0xB8, 0x17, 0x00, 0x00};
/// At the resume: test byte [rsi+40h],1 and jz 0x1425F35FA (the second exit is where the game's own "not visible" goes).
inline constexpr std::uint8_t kHideResumeBytes[] = {0xF6, 0x46, 0x40, 0x01, 0x0F, 0x84, 0xD1, 0x01, 0x00, 0x00};

inline std::vector<std::uint8_t> HideCode(State& s, std::uintptr_t notVisible)
{
    Code        c;
    Code::Label done;
    c.Put(kHideAdd);                                          // add rsi,[rbx+17B8h]   (replaced)
    c.Put({0xF6, 0x46, 0x40, 0x10});                          // test byte [rsi+40h],10h   this view beyond the cutoff?
    c.Jcc(kJe, done);
    c.Put({0x41, 0xF6, 0x87, 0x68, 0x05, 0x00, 0x00, 0x40});  // test byte [r15+568h],40h  a whole-scene shadow
    c.Jcc(kJe, done);
    c.Put({0x41, 0xF6, 0x87, 0x68, 0x05, 0x00, 0x00, 0x10});  // test byte [r15+568h],10h  not the sun's
    c.Jcc(kJne, done);
    c.Put({0x48, 0x8B, 0x55, 0x30});                          // mov rdx,[rbp+30h]     the renderer
    c.Put({0x83, 0xBA, 0xD8, 0x00, 0x00, 0x00, 0x02});        // cmp dword [rdx+0D8h],2
    c.Jcc(kJl, done);
    c.Put({0x48, 0xBA});  // mov rdx,&state
    c.Value(reinterpret_cast<std::uint64_t>(&s));
    c.Put({0x80, 0xBA});  // cmp byte [rdx+on[kLamps]],0
    c.Disp32(offsetof(State, on) + kLamps);
    c.Put({0x00});
    c.Jcc(kJe, done);
    c.Put({0x48, 0xFF, 0x82});  // inc qword [rdx+hidden]
    c.Disp32(offsetof(State, hidden));
    c.JmpAbsolute(notVisible);
    c.Bind(done);
    return c.Finish({&done});
}

// --- Lever C --------------------------------------------------------------------------------------------------------

/// The moving casters' loop takes Views[0]: mov rax,[r12+0D0h] (r12 = the renderer, r15 = the caster's primitive scene
/// info, bounds centre +0xF0). The game then measures from [rax+4ACh] (the view origin), and its jnz at 0x1425F2E55 reads
/// ZF from cmp byte [r14+18Ch],0 at 0x1425F2DE2, which the stub re-does last.
inline constexpr std::uintptr_t kCasterSite   = 0x1425F2DF2;
inline constexpr std::uintptr_t kCasterResume = 0x1425F2DFA;
inline constexpr std::uint8_t   kCasterLoad[] = {0x49, 0x8B, 0x84, 0x24, 0xD0, 0x00, 0x00, 0x00};
inline constexpr std::uintptr_t kCasterFlagCompare = 0x1425F2DE2;
inline constexpr std::uint8_t   kCasterFlagBytes[] = {0x41, 0x80, 0xBE, 0x8C, 0x01, 0x00, 0x00, 0x00};
/// 0x1425F2DFA through the jnz at 0x1425F2E55: SSE only (no flag write) between the stub's compare and the jnz.
inline constexpr std::uint8_t kCasterTail[] = {
    0xF3, 0x0F, 0x10, 0x88, 0xAC, 0x04, 0x00, 0x00, 0xF3, 0x41, 0x0F, 0x5C, 0x8F, 0xF0, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10,
    0x90, 0xB0, 0x04, 0x00, 0x00, 0xF3, 0x41, 0x0F, 0x5C, 0x97, 0xF4, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x80, 0xB4, 0x04,
    0x00, 0x00, 0xF3, 0x41, 0x0F, 0x5C, 0x87, 0xF8, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x59, 0xC9, 0xF3, 0x0F, 0x59, 0xD2, 0xF3,
    0x0F, 0x59, 0xC0, 0xF3, 0x0F, 0x58, 0xD1, 0x0F, 0x57, 0xC9, 0xF3, 0x0F, 0x58, 0xD0, 0xF3, 0x0F, 0x51, 0xCA, 0xF3, 0x41,
    0x0F, 0x5C, 0x8F, 0x08, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x5F, 0xCF, 0x75, 0x17};
static_assert(sizeof(kCasterTail) == 0x1425F2E57 - kCasterResume);
inline constexpr std::uint32_t  kViewStride        = 0x28B0;
inline constexpr std::size_t    kMaxViews          = 4;

/// The squared distance from the view at `view` (0 rax, 1 rcx) to the caster's centre into `into` (0 xmm0, 1 xmm1);
/// xmm2 is scratch. Only these registers: another base would need another encoding.
inline void Distance(Code& c, int view, int into)
{
    const auto at  = [](std::uint8_t reg, std::uint8_t base) { return static_cast<std::uint8_t>(0x80 | (reg << 3) | base); };
    const auto rr  = [](std::uint8_t a, std::uint8_t b) { return static_cast<std::uint8_t>(0xC0 | (a << 3) | b); };
    const auto x   = static_cast<std::uint8_t>(into);
    const auto v   = static_cast<std::uint8_t>(view);
    c.Put({0xF3, 0x0F, 0x10, at(x, v), 0xAC, 0x04, 0x00, 0x00});        // movss xA,[v+4ACh]
    c.Put({0xF3, 0x41, 0x0F, 0x5C, at(x, 7), 0xF0, 0x00, 0x00, 0x00});  // subss xA,[r15+0F0h]
    c.Put({0xF3, 0x0F, 0x59, rr(x, x)});                                // mulss xA,xA
    c.Put({0xF3, 0x0F, 0x10, at(2, v), 0xB0, 0x04, 0x00, 0x00});        // movss xmm2,[v+4B0h]
    c.Put({0xF3, 0x41, 0x0F, 0x5C, at(2, 7), 0xF4, 0x00, 0x00, 0x00});  // subss xmm2,[r15+0F4h]
    c.Put({0xF3, 0x0F, 0x59, rr(2, 2)});                                // mulss xmm2,xmm2
    c.Put({0xF3, 0x0F, 0x58, rr(x, 2)});                                // addss xA,xmm2
    c.Put({0xF3, 0x0F, 0x10, at(2, v), 0xB4, 0x04, 0x00, 0x00});        // movss xmm2,[v+4B4h]
    c.Put({0xF3, 0x41, 0x0F, 0x5C, at(2, 7), 0xF8, 0x00, 0x00, 0x00});  // subss xmm2,[r15+0F8h]
    c.Put({0xF3, 0x0F, 0x59, rr(2, 2)});                                // mulss xmm2,xmm2
    c.Put({0xF3, 0x0F, 0x58, rr(x, 2)});                                // addss xA,xmm2
}

inline std::vector<std::uint8_t> CasterCode(State& s)
{
    Code        c;
    Code::Label done, chosen;
    c.Put(kCasterLoad);                                             // mov rax,[r12+0D0h]   (replaced) Views[0]
    c.Put({0x41, 0x83, 0xBC, 0x24, 0xD8, 0x00, 0x00, 0x00, 0x02});  // cmp dword [r12+0D8h],2
    c.Jcc(kJl, done);
    MovRcxImm(c, &s);
    c.Put({0x80, 0xB9});  // cmp byte [rcx+on[kCasters]],0
    c.Disp32(offsetof(State, on) + kCasters);
    c.Put({0x00});
    c.Jcc(kJe, done);
    Distance(c, 0, 0);  // xmm0 = view 0's
    c.Put({0x48, 0x8B, 0xC8});  // mov rcx,rax
    for (std::size_t view = 1; view < kMaxViews; ++view)
    {
        Code::Label next;
        c.Put({0x41, 0x83, 0xBC, 0x24, 0xD8, 0x00, 0x00, 0x00, static_cast<std::uint8_t>(view + 1)});  // cmp [r12+0D8h],view+1
        c.Jcc(kJl, chosen);
        c.Put({0x48, 0x81, 0xC1});  // add rcx,28B0h
        c.Value(kViewStride);
        Distance(c, 1, 1);           // xmm1 = this view's
        c.Put({0x0F, 0x2F, 0xC1});   // comiss xmm0,xmm1
        c.Jcc(kJbe, next);           // not nearer (ties: the lower view; a NaN distance is never chosen)
        c.Put({0x0F, 0x28, 0xC1});   // movaps xmm0,xmm1
        c.Put({0x48, 0x8B, 0xC1});   // mov rax,rcx
        c.Bind(next);
    }
    c.Bind(chosen);
    c.Put({0x49, 0x3B, 0x84, 0x24, 0xD0, 0x00, 0x00, 0x00});  // cmp rax,[r12+0D0h]
    c.Jcc(kJe, done);
    MovRcxImm(c, &s);
    c.Put({0x48, 0xFF, 0x81});  // inc qword [rcx+nearCasters]
    c.Disp32(offsetof(State, nearCasters));
    c.Bind(done);
    c.Put(kCasterFlagBytes);  // cmp byte [r14+18Ch],0   (ZF for the game's jnz)
    return c.Finish({&done, &chosen});
}

// --- Lever A -----------------------------------------------------------------------------------------------------------

/// The tag: ULocalPlayer::CalcSceneView calls FSceneView::StartFinalPostprocessSettings (the SE controllers' blend for
/// this view's location, pushed to the scene) right after adding the view to its family; the scene's setter is FScene's
/// vtable entry +0x38; its render-thread half is called from the render command (task path) and inline (rendering not
/// threaded), with {FScene*, the 0x50-byte blend data}.
inline constexpr std::uintptr_t kStartFinalCall     = 0x14301909B;
inline constexpr std::uintptr_t kStartFinal         = 0x14320FBD0;
inline constexpr std::uintptr_t kSceneBlendEntry    = 0x144D63A90;  // FScene vtable 0x144D63A58 + 0x38
inline constexpr std::uintptr_t kSceneBlendSetter   = 0x142583CE0;
inline constexpr std::uintptr_t kSceneBlendRt       = 0x142583F10;
inline constexpr std::uintptr_t kSceneBlendRtTask   = 0x142599436;
inline constexpr std::uintptr_t kSceneBlendRtInline = 0x142583D58;

/// PostVisibilityFrameSetup's light-LOD loop, the view loop inside the light loop: add r13,[r12+0D0h] (r13 = the view's
/// index x 0x28B0, then the view). At the site rcx = the scene, [rbp+10h] = the view's index; rax, rdx, rbx are written
/// before any read after it (rax reloaded at the loop's back edge). Every override read below re-loads the scene.
inline constexpr std::uintptr_t kAreaSite   = 0x1425D25D5;
inline constexpr std::uintptr_t kAreaResume = 0x1425D25DD;
inline constexpr std::uint8_t   kAreaAdd[]  = {0x4D, 0x03, 0xAC, 0x24, 0xD0, 0x00, 0x00, 0x00};
/// 0x1425D25C9..0x1425D25F5 (checked at install: the scene load into rcx, the site, the writes of rbx, r14, rdx, rax).
inline constexpr std::uintptr_t kAreaContextStart = 0x1425D25C9;
inline constexpr std::uint8_t   kAreaContext[]    = {
    0x4C, 0x69, 0xE9, 0xB0, 0x28, 0x00, 0x00, 0x49, 0x8B, 0x4C, 0x24, 0x08, 0x4D, 0x03, 0xAC, 0x24, 0xD0, 0x00, 0x00, 0x00,
    0x49, 0x8B, 0x9D, 0xB8, 0x17, 0x00, 0x00, 0x4D, 0x8D, 0xB5, 0xAC, 0x04, 0x00, 0x00, 0xC6, 0x44, 0x24, 0x52, 0x00, 0x49,
    0x8B, 0xD6, 0x48, 0x8B, 0x01};

/// The record index the A stubs take: the player's place in the frame's split (`familyIndex`, render/family_now.h) while
/// one family per player is drawn, where every view is the renderer's view 0; otherwise the renderer's view index. The
/// record's view-state check after it still decides.
inline std::vector<std::uint8_t> AreaCode(State& s, const std::int32_t* familyIndex)
{
    Code        c;
    Code::Label done, game, copy, have;
    c.Put(kAreaAdd);  // add r13,[r12+0D0h]   (replaced)
    c.Put({0x48, 0xB8});                                      // mov rax,&state
    c.Value(reinterpret_cast<std::uint64_t>(&s));
    c.Put({0x80, 0xB8});  // cmp byte [rax+on[kAreas]],0
    c.Disp32(offsetof(State, on) + kAreas);
    c.Put({0x00});
    c.Jcc(kJe, done);
    c.Put({0x48, 0xBA});              // mov rdx,familyIndex
    c.Value(reinterpret_cast<std::uint64_t>(familyIndex));
    c.Put({0x48, 0x63, 0x12});        // movsxd rdx,dword [rdx]   the player's place in a split frame (-1: none)
    c.Put({0x48, 0x85, 0xD2});        // test rdx,rdx
    c.Jcc(kJns, have);
    c.Put({0x48, 0x8B, 0x55, 0x10});  // mov rdx,[rbp+10h]       the view's index
    c.Bind(have);
    c.Put({0x48, 0x83, 0xFA, static_cast<std::uint8_t>(kRecords)});  // cmp rdx,4
    c.Jcc(kJae, game);
    c.Put({0x48, 0x6B, 0xD2, static_cast<std::uint8_t>(sizeof(ViewRec))});  // imul rdx,rdx,60h
    c.Put({0x48, 0x8D, 0x94, 0x10});                                        // lea rdx,[rax+rdx+records]
    c.Disp32(offsetof(State, records));
    c.Put({0x80, 0x3A, 0x00});        // cmp byte [rdx],0          valid
    c.Jcc(kJe, game);
    c.Put({0x48, 0x3B, 0x4A, 0x08});  // cmp rcx,[rdx+8]            this scene
    c.Jcc(kJne, game);
    c.Put({0x48, 0x8B, 0x5A, 0x10});  // mov rbx,[rdx+10h]
    c.Put({0x49, 0x3B, 0x5D, 0x08});  // cmp rbx,[r13+8]            this view's state (a player's view)
    c.Jcc(kJne, game);
    c.Put({0x48, 0xFF, 0x80});  // inc qword [rax+areaOwn]
    c.Disp32(offsetof(State, areaOwn));
    c.Jmp(copy);
    c.Bind(game);
    c.Put({0x48, 0x8D, 0x90});  // lea rdx,[rax+game]
    c.Disp32(offsetof(State, game));
    c.Put({0x80, 0x3A, 0x00});  // cmp byte [rdx],0
    c.Jcc(kJe, done);
    c.Put({0x48, 0x3B, 0x4A, 0x08});  // cmp rcx,[rdx+8]
    c.Jcc(kJne, done);
    c.Put({0x48, 0xFF, 0x80});  // inc qword [rax+areaGame]
    c.Disp32(offsetof(State, areaGame));
    c.Bind(copy);
    c.Put({0x48, 0x8B, 0x5A, 0x20});                          // mov rbx,[rdx+20h]
    c.Put({0x48, 0x89, 0x99, 0x08, 0x16, 0x00, 0x00});        // mov [rcx+1608h],rbx
    c.Put({0x48, 0x8B, 0x5A, 0x28});                          // mov rbx,[rdx+28h]
    c.Put({0x48, 0x89, 0x99, 0x10, 0x16, 0x00, 0x00});        // mov [rcx+1610h],rbx
    c.Put({0x48, 0x8B, 0x5A, 0x30});                          // mov rbx,[rdx+30h]
    c.Put({0x48, 0x89, 0x99, 0x18, 0x16, 0x00, 0x00});        // mov [rcx+1618h],rbx
    c.Bind(done);
    return c.Finish({&done, &game, &copy, &have});
}

/// The sun's cascade setup, its view loop: add rsi,[rdi+0D0h] (rax = the view's index, rdi = the renderer, rsi = the
/// view after it, the light at [rsp+58h]: r13 saved at 0x1425F4D67). rdx and r9 are live; rcx, r8, r10, r11 are written
/// before any read on both paths after it.
inline constexpr std::uintptr_t kSunSite   = 0x1425F4DA2;
inline constexpr std::uintptr_t kSunResume = 0x1425F4DA9;
inline constexpr std::uint8_t   kSunAdd[]  = {0x48, 0x03, 0xB7, 0xD0, 0x00, 0x00, 0x00};
/// 0x1425F4D90..0x1425F4DBB (checked at install: rax the index, the site, rdx and the paths after it).
inline constexpr std::uintptr_t kSunContextStart = 0x1425F4D90;
inline constexpr std::uint8_t   kSunContext[]    = {
    0x48, 0x69, 0xF0, 0xB0, 0x28, 0x00, 0x00, 0x49, 0x8B, 0xD1, 0x48, 0xC7, 0x45, 0x98, 0x00, 0x00, 0x00, 0x00, 0x48, 0x03,
    0xB7, 0xD0, 0x00, 0x00, 0x00, 0x48, 0x89, 0x74, 0x24, 0x68, 0x48, 0x89, 0x55, 0x90, 0x85, 0xDB, 0x74, 0x64, 0x8B, 0xD3,
    0x48, 0x8D, 0x4D, 0x88};

inline std::vector<std::uint8_t> SunCode(State& s, const std::int32_t* familyIndex)
{
    Code        c;
    Code::Label done, game, copy, have;
    c.Put(kSunAdd);  // add rsi,[rdi+0D0h]   (replaced)
    MovRcxImm(c, &s);
    c.Put({0x80, 0xB9});  // cmp byte [rcx+on[kSun]],0
    c.Disp32(offsetof(State, on) + kSun);
    c.Put({0x00});
    c.Jcc(kJe, done);
    c.Put({0x4C, 0x8B, 0x47, 0x08});                    // mov r8,[rdi+8]          the scene
    c.Put({0x4C, 0x8B, 0x54, 0x24, 0x58});              // mov r10,[rsp+58h]       the light
    c.Put({0x4D, 0x3B, 0x90, 0x20, 0x16, 0x00, 0x00});  // cmp r10,[r8+1620h]      the sun the controllers drive
    c.Jcc(kJne, done);
    c.Put({0x4D, 0x8B, 0x52, 0x30});                    // mov r10,[r10+30h]       its proxy
    c.Put({0x49, 0xBB});              // mov r11,familyIndex    (rax, the view's index, stays as the game left it)
    c.Value(reinterpret_cast<std::uint64_t>(familyIndex));
    c.Put({0x4D, 0x63, 0x1B});        // movsxd r11,dword [r11]  the player's place in a split frame (-1: none)
    c.Put({0x4D, 0x85, 0xDB});        // test r11,r11
    c.Jcc(kJns, have);
    c.Put({0x49, 0x89, 0xC3});        // mov r11,rax             the view's index
    c.Bind(have);
    c.Put({0x49, 0x83, 0xFB, static_cast<std::uint8_t>(kRecords)});  // cmp r11,4
    c.Jcc(kJae, game);
    c.Put({0x4D, 0x6B, 0xDB, static_cast<std::uint8_t>(sizeof(ViewRec))});  // imul r11,r11,60h
    c.Put({0x4E, 0x8D, 0x9C, 0x19});                                        // lea r11,[rcx+r11+records]
    c.Disp32(offsetof(State, records));
    c.Put({0x41, 0x80, 0x3B, 0x00});  // cmp byte [r11],0
    c.Jcc(kJe, game);
    c.Put({0x4D, 0x3B, 0x43, 0x08});  // cmp r8,[r11+8]             this scene
    c.Jcc(kJne, game);
    c.Put({0x4D, 0x3B, 0x53, 0x18});  // cmp r10,[r11+18h]          this proxy
    c.Jcc(kJne, game);
    c.Put({0x4C, 0x8B, 0x46, 0x08});  // mov r8,[rsi+8]
    c.Put({0x4D, 0x3B, 0x43, 0x10});  // cmp r8,[r11+10h]           this view's state
    c.Jcc(kJne, game);
    c.Put({0x48, 0xFF, 0x81});  // inc qword [rcx+sunOwn]
    c.Disp32(offsetof(State, sunOwn));
    c.Jmp(copy);
    c.Bind(game);
    c.Put({0x4C, 0x8D, 0x99});  // lea r11,[rcx+game]
    c.Disp32(offsetof(State, game));
    c.Put({0x41, 0x80, 0x3B, 0x00});  // cmp byte [r11],0
    c.Jcc(kJe, done);
    c.Put({0x4D, 0x3B, 0x53, 0x18});  // cmp r10,[r11+18h]
    c.Jcc(kJne, done);
    c.Put({0x48, 0xFF, 0x81});  // inc qword [rcx+sunGame]
    c.Disp32(offsetof(State, sunGame));
    c.Bind(copy);
    c.Put({0x45, 0x8B, 0x43, 0x38});                    // mov r8d,[r11+38h]
    c.Put({0x45, 0x89, 0x82, 0xA8, 0x01, 0x00, 0x00});  // mov [r10+1A8h],r8d
    c.Put({0x4D, 0x8B, 0x43, 0x3C});                    // mov r8,[r11+3Ch]
    c.Put({0x4D, 0x89, 0x82, 0xC0, 0x01, 0x00, 0x00});  // mov [r10+1C0h],r8
    c.Put({0x4D, 0x8B, 0x43, 0x44});                    // mov r8,[r11+44h]
    c.Put({0x4D, 0x89, 0x82, 0xC8, 0x01, 0x00, 0x00});  // mov [r10+1C8h],r8
    c.Put({0x45, 0x8B, 0x43, 0x4C});                    // mov r8d,[r11+4Ch]
    c.Put({0x45, 0x89, 0x82, 0xD0, 0x01, 0x00, 0x00});  // mov [r10+1D0h],r8d
    c.Put({0x4D, 0x8B, 0x43, 0x50});                    // mov r8,[r11+50h]
    c.Put({0x4D, 0x89, 0x82, 0xDC, 0x00, 0x00, 0x00});  // mov [r10+0DCh],r8
    c.Bind(done);
    return c.Finish({&done, &game, &copy, &have});
}

// --- A-fog --------------------------------------------------------------------------------------------------------------

/// The fog: FScene's vtable entry +0x238 (the game thread's setter, which copies the 0x38-byte blend data whole into its
/// render command); its render-thread half, called from the command's task with {FScene*, the data}.
inline constexpr std::uintptr_t kSceneFogEntry  = 0x144D63C90;  // FScene vtable 0x144D63A58 + 0x238
inline constexpr std::uintptr_t kSceneFogSetter = 0x14258BDE0;
inline constexpr std::uintptr_t kSceneFogRt     = 0x14258C090;
inline constexpr std::uintptr_t kSceneFogRtTask = 0x1425950F6;

/// The one call of InitFogConstants (end of PostVisibilityFrameSetup): wrapped to put the scene's first fog back as the
/// game left it once every view's parameters are made (volumetric fog reads it after, for every view).
inline constexpr std::uintptr_t kFogConstantsCall = 0x1425D3784;
inline constexpr std::uintptr_t kFogConstants     = 0x142205650;

/// InitFogConstants' view loop (r15d = the view's index, rdi = the view, rbx = the scene, its fog count checked > 0):
/// mov rbx,[rbx+1DC0h] (rbx = the first fog), which every field read of the view's fog parameters goes through. rax, rcx
/// and rdx are written before any read after it (the powf call at 0x14220579C).
inline constexpr std::uintptr_t kFogSite   = 0x142205768;
inline constexpr std::uintptr_t kFogResume = 0x14220576F;
inline constexpr std::uint8_t   kFogLoad[] = {0x48, 0x8B, 0x9B, 0xC0, 0x1D, 0x00, 0x00};
/// 0x142205757..0x1422057A0 (checked at install: the scene from the renderer, the count test, the site, the reads after
/// it through the powf call).
inline constexpr std::uintptr_t kFogContextStart = 0x142205757;
inline constexpr std::uint8_t   kFogContext[]    = {
    0x49, 0x8B, 0x5E, 0x08, 0x44, 0x39, 0xAB, 0xC8, 0x1D, 0x00, 0x00, 0x0F, 0x8E, 0xE6, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x9B,
    0xC0, 0x1D, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x4B, 0x08, 0xF3, 0x0F, 0x5C, 0x8F, 0xB4, 0x04, 0x00, 0x00, 0xF3, 0x44, 0x0F,
    0x10, 0x53, 0x10, 0xF3, 0x41, 0x0F, 0x59, 0xCA, 0x41, 0x0F, 0x2F, 0xCB, 0x73, 0x06, 0x41, 0x0F, 0x28, 0xCB, 0xEB, 0x05,
    0xF3, 0x41, 0x0F, 0x5D, 0xCD, 0x41, 0x0F, 0x28, 0xC7, 0xE8, 0x65, 0xAC, 0xE2, 0x01};
static_assert(sizeof(kFogContext) == 0x1422057A1 - kFogContextStart);

inline std::vector<std::uint8_t> FogCode(State& s, const std::int32_t* familyIndex)
{
    Code        c;
    Code::Label done, game, copy, have;
    c.Put(kFogLoad);  // mov rbx,[rbx+1DC0h]   (replaced) the first fog
    c.Put({0x48, 0xB8});  // mov rax,&state
    c.Value(reinterpret_cast<std::uint64_t>(&s));
    c.Put({0x80, 0xB8});  // cmp byte [rax+on[kFog]],0
    c.Disp32(offsetof(State, on) + kFog);
    c.Put({0x00});
    c.Jcc(kJe, done);
    c.Put({0x48, 0xBA});                                             // mov rdx,familyIndex
    c.Value(reinterpret_cast<std::uint64_t>(familyIndex));
    c.Put({0x48, 0x63, 0x12});                                       // movsxd rdx,dword [rdx]   (-1: none)
    c.Put({0x85, 0xD2});                                             // test edx,edx
    c.Jcc(kJns, have);
    c.Put({0x44, 0x89, 0xFA});                                       // mov edx,r15d             the view's index
    c.Bind(have);
    c.Put({0x83, 0xFA, static_cast<std::uint8_t>(kRecords)});        // cmp edx,4
    c.Jcc(kJae, game);
    c.Put({0x48, 0x6B, 0xD2, static_cast<std::uint8_t>(sizeof(FogRec))});  // imul rdx,rdx,60h
    c.Put({0x48, 0x8D, 0x94, 0x10});                                       // lea rdx,[rax+rdx+fogs]
    c.Disp32(offsetof(State, fogs));
    c.Put({0x80, 0x3A, 0x00});        // cmp byte [rdx],0           valid
    c.Jcc(kJe, game);
    c.Put({0x48, 0x3B, 0x5A, 0x08});  // cmp rbx,[rdx+8]            this fog
    c.Jcc(kJne, game);
    c.Put({0x48, 0x8B, 0x0B});        // mov rcx,[rbx]
    c.Put({0x48, 0x3B, 0x4A, 0x58});  // cmp rcx,[rdx+58h]          of this fog component
    c.Jcc(kJne, game);
    c.Put({0x48, 0x8B, 0x4A, 0x10});  // mov rcx,[rdx+10h]
    c.Put({0x48, 0x3B, 0x4F, 0x08});  // cmp rcx,[rdi+8]            this view's state (a player's view)
    c.Jcc(kJne, game);
    c.Put({0x48, 0xFF, 0x80});  // inc qword [rax+fogOwn]
    c.Disp32(offsetof(State, fogOwn));
    c.Jmp(copy);
    c.Bind(game);
    c.Put({0x48, 0x8D, 0x90});  // lea rdx,[rax+fogGame]
    c.Disp32(offsetof(State, fogGame));
    c.Put({0x80, 0x3A, 0x00});  // cmp byte [rdx],0
    c.Jcc(kJe, done);
    c.Put({0x48, 0x3B, 0x5A, 0x08});  // cmp rbx,[rdx+8]
    c.Jcc(kJne, done);
    c.Put({0x48, 0x8B, 0x0B});        // mov rcx,[rbx]
    c.Put({0x48, 0x3B, 0x4A, 0x58});  // cmp rcx,[rdx+58h]
    c.Jcc(kJne, done);
    c.Put({0x48, 0xFF, 0x80});  // inc qword [rax+fogShared]
    c.Disp32(offsetof(State, fogShared));
    c.Bind(copy);
    c.Put({0x48, 0x8B, 0x4A, 0x20});                          // mov rcx,[rdx+20h]
    c.Put({0x48, 0x89, 0x4B, 0x08});                          // mov [rbx+8],rcx       height, density
    c.Put({0x48, 0x8B, 0x4A, 0x28});                          // mov rcx,[rdx+28h]
    c.Put({0x48, 0x89, 0x4B, 0x10});                          // mov [rbx+10h],rcx     height falloff, max opacity
    c.Put({0x48, 0x8B, 0x4A, 0x30});                          // mov rcx,[rdx+30h]
    c.Put({0x48, 0x89, 0x4B, 0x24});                          // mov [rbx+24h],rcx     inscattering colour
    c.Put({0x48, 0x8B, 0x4A, 0x38});                          // mov rcx,[rdx+38h]
    c.Put({0x48, 0x89, 0x4B, 0x2C});                          // mov [rbx+2Ch],rcx
    c.Put({0x48, 0x8B, 0x4A, 0x40});                          // mov rcx,[rdx+40h]
    c.Put({0x48, 0x89, 0x8B, 0x9C, 0x00, 0x00, 0x00});        // mov [rbx+9Ch],rcx     the second colour
    c.Put({0x48, 0x8B, 0x4A, 0x48});                          // mov rcx,[rdx+48h]
    c.Put({0x48, 0x89, 0x8B, 0xA4, 0x00, 0x00, 0x00});        // mov [rbx+0A4h],rcx
    c.Put({0x8B, 0x4A, 0x50});                                // mov ecx,[rdx+50h]
    c.Put({0x89, 0x8B, 0xC0, 0x00, 0x00, 0x00});              // mov [rbx+0C0h],ecx
    c.Bind(done);
    return c.Finish({&done, &game, &copy, &have});
}

}  // namespace lss::render::shadow_code
