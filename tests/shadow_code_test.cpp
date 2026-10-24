// Host-side tests for render/shadow_code.h, run without the game: each stub installed through platform::CodeRedirect
// over a stand-in that loads the game's registers from a context, holds the replaced instruction at the game's block
// offset, and stores the registers (and ZF) back where the game would resume. EXECUTED.

#include "platform/code_redirect.h"
#include "render/shadow_code.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

namespace code = lss::render::shadow_code;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

/// The registers a stub reads or leaves, and the byte at [rsp+42h].
struct Context
{
    std::uint64_t rax = 0, rcx = 0, rdx = 0, rbx = 0, rbp = 0, rsi = 0, rdi = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;
    std::uint8_t  var = 0, zf = 0, other = 0;  // other: the stand-in's second exit was taken
    std::uint64_t stack58 = 0;                  // [rsp+58h] at the site
};

struct Builder
{
    std::vector<std::uint8_t> bytes;
    void        Put(std::initializer_list<std::uint8_t> b) { bytes.insert(bytes.end(), b); }
    void        Put(const std::uint8_t* b, std::size_t n) { bytes.insert(bytes.end(), b, b + n); }
    std::size_t At() const { return bytes.size(); }
    void        PadTo(std::size_t blockOffset)
    {
        while ((bytes.size() & 0xF) != blockOffset)
        {
            bytes.push_back(0x90);
        }
    }
};

constexpr std::uint8_t D(std::size_t field) { return static_cast<std::uint8_t>(field); }

/// rax = context: mov reg,[rax+field] (`load`) or mov [rax+field],reg.
void Move(Builder& b, bool load, int reg, std::size_t field)
{
    const std::uint8_t rex = static_cast<std::uint8_t>(0x48 | (reg >= 8 ? 0x04 : 0));
    b.Put({rex, static_cast<std::uint8_t>(load ? 0x8B : 0x89), static_cast<std::uint8_t>(0x40 | ((reg & 7) << 3)), D(field)});
}

enum Reg
{
    kRax = 0, kRcx = 1, kRdx = 2, kRbx = 3, kRbp = 5, kRsi = 6, kRdi = 7, kR12 = 12, kR13 = 13, kR14 = 14, kR15 = 15
};
struct Field
{
    int         reg;
    std::size_t at;
};
const Field kFields[] = {{kRcx, offsetof(Context, rcx)}, {kRdx, offsetof(Context, rdx)}, {kRbx, offsetof(Context, rbx)},
                         {kRbp, offsetof(Context, rbp)}, {kRsi, offsetof(Context, rsi)}, {kRdi, offsetof(Context, rdi)},
                         {kR12, offsetof(Context, r12)},
                         {kR13, offsetof(Context, r13)}, {kR14, offsetof(Context, r14)}, {kR15, offsetof(Context, r15)}};

/// void Run(Context*): the site at `blockOffset` holding `replaced`; returns the site, the resume and the second exit.
struct StandIn
{
    using Fn = void (*)(Context*);
    Fn            fn = nullptr;
    std::uint8_t* page = nullptr;
    std::size_t   site = 0, resume = 0, other = 0;

    /// `runThrough`: the game's bytes run as they are and fall into the stores (the reference for a differential).
    StandIn(std::size_t blockOffset, const std::uint8_t* replaced, std::size_t length, bool runThrough = false)
    {
        Builder b;
        b.Put({0x53, 0x56, 0x57, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});  // push rbx..r15
        b.Put({0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00});                                // sub rsp,80h
        b.Put({0x48, 0x89, 0x4C, 0x24, 0x78});                                            // mov [rsp+78h],rcx
        b.Put({0x48, 0x8B, 0xC1});                                                        // mov rax,rcx
        b.Put({0x8A, 0x50, D(offsetof(Context, var))});                                  // mov dl,[rax+var]
        b.Put({0x88, 0x54, 0x24, 0x42});                                                  // mov [rsp+42h],dl
        b.Put({0x48, 0x8B, 0x50, D(offsetof(Context, stack58))});                         // mov rdx,[rax+stack58]
        b.Put({0x48, 0x89, 0x54, 0x24, 0x58});                                            // mov [rsp+58h],rdx
        for (const Field& f : kFields)
        {
            Move(b, true, f.reg, f.at);
        }
        Move(b, true, kRax, offsetof(Context, rax));
        b.PadTo(blockOffset);
        site = b.At();
        b.Put(replaced, length);
        if (!runThrough)
        {
            b.Put({0xCC});  // never reached: the stub resumes below
        }
        resume = b.At();
        Store(b, false);
        other = b.At();
        Store(b, true);
        page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        std::memcpy(page, b.bytes.data(), b.bytes.size());
        fn = reinterpret_cast<Fn>(page);
    }
    ~StandIn() { VirtualFree(page, 0, MEM_RELEASE); }
    StandIn(const StandIn&) = delete;
    StandIn& operator=(const StandIn&) = delete;

    std::uintptr_t At(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(page + offset); }

private:
    static void Store(Builder& b, bool otherExit)
    {
        b.Put({0x48, 0x89, 0x44, 0x24, 0x70});  // mov [rsp+70h],rax
        b.Put({0x48, 0x8B, 0x44, 0x24, 0x78});  // mov rax,[rsp+78h]   (moves keep the flags)
        b.Put({0x0F, 0x94, 0x40, D(offsetof(Context, zf))});  // sete [rax+zf]
        for (const Field& f : kFields)
        {
            Move(b, false, f.reg, f.at);
        }
        b.Put({0x8A, 0x54, 0x24, 0x42});                      // mov dl,[rsp+42h]
        b.Put({0x88, 0x50, D(offsetof(Context, var))});       // mov [rax+var],dl
        b.Put({0xC6, 0x40, D(offsetof(Context, other)), static_cast<std::uint8_t>(otherExit ? 1 : 0)});  // mov byte [rax+other]
        b.Put({0x48, 0x8B, 0x4C, 0x24, 0x70});                // mov rcx,[rsp+70h]
        b.Put({0x48, 0x89, 0x48, D(offsetof(Context, rax))}); // mov [rax+rax_],rcx
        b.Put({0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00});    // add rsp,80h
        b.Put({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5F, 0x5E, 0x5B, 0xC3});  // pop r15..rbx; ret
    }
};

template <class T>
std::uint64_t Address(T* p)
{
    return reinterpret_cast<std::uint64_t>(p);
}

// --- B: the gather ----------------------------------------------------------------------------------------------------

struct GatherWorld
{
    std::vector<std::uint8_t> views    = std::vector<std::uint8_t>(0x2000, 0);  // r14: [r14+17B8h] = &light infos
    std::vector<std::uint8_t> infos    = std::vector<std::uint8_t>(0x100, 0);   // the view's VisibleLightInfos
    std::vector<std::uint8_t> renderer = std::vector<std::uint8_t>(0x100, 0);   // r12: +0xD8 the view count
    std::vector<std::uint8_t> light    = std::vector<std::uint8_t>(0x40, 0);    // r15: +0x30 the proxy
    std::uint64_t             proxy[1] = {0x1234};                              // [proxy] = its vtable

    GatherWorld()
    {
        const auto at = Address(infos.data());
        std::memcpy(views.data() + 0x17B8, &at, 8);
        const auto p = Address(proxy);
        std::memcpy(light.data() + 0x30, &p, 8);
    }
    Context Run(StandIn& s, std::uint32_t viewCount, std::uint32_t flags, std::uint8_t before)
    {
        std::memcpy(renderer.data() + 0xD8, &viewCount, 4);
        std::memcpy(infos.data() + 0x40, &flags, 4);
        Context c;
        c.rcx = 0;  // light index × 0x78
        c.rsi = 0;
        c.r14 = Address(views.data());
        c.r15 = Address(light.data());
        c.r12 = Address(renderer.data());
        c.rdx = before;
        c.var = 0xEE;
        s.fn(&c);
        return c;
    }
};

void TestGather()
{
    std::puts("lamps (B), the gather: the game's bit with one view or the switch off; per view 1 beyond, 2 seen within, "
              "nothing for a lamp outside the view; the re-done instructions' results");
    StandIn                 s(code::kGatherSite & 0xF, code::kGatherAdd, sizeof(code::kGatherAdd));
    alignas(16) code::State state;
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(s.At(s.site), code::kGatherAdd, code::GatherCode(state), s.At(s.resume)));
    GatherWorld w;
    const auto  check = [&](std::uint32_t views, std::uint32_t flags, std::uint8_t before, std::uint8_t expected) {
        const Context c = w.Run(s, views, flags, before);
        EXPECT((c.rdx & 0xFF) == expected && c.var == expected);
        EXPECT(c.rcx == Address(w.proxy) && c.rax == 0x1234 && !c.other);
    };
    // Differential: with the switch off or one view, every register and the byte as the game's own seven instructions leave them.
    StandIn reference(code::kGatherSite & 0xF, code::kGatherBytes, sizeof(code::kGatherBytes), true);
    const auto same = [&](std::uint32_t views) {
        for (std::uint32_t flags : {0x00u, 0x01u, 0x10u, 0x11u, 0x15u})
        {
            for (std::uint8_t before : {0, 1})
            {
                const auto prepare = [&](Context& c) {
                    std::memcpy(w.renderer.data() + 0xD8, &views, 4);
                    std::memcpy(w.infos.data() + 0x40, &flags, 4);
                    c.rcx = 0;
                    c.r14 = Address(w.views.data());
                    c.r15 = Address(w.light.data());
                    c.r12 = Address(w.renderer.data());
                    c.rdx = 0xAAAAAAAAAAAAAA00ull | before;
                    c.rbx = 0x1111;
                    c.r13 = 0x2222;
                };
                Context ours, game;
                prepare(ours);
                prepare(game);
                s.fn(&ours);
                reference.fn(&game);
                EXPECT(ours.rcx == game.rcx && ours.rdx == game.rdx && ours.rbx == game.rbx && ours.rsi == game.rsi &&
                       ours.r12 == game.r12 && ours.r13 == game.r13 && ours.r14 == game.r14 && ours.r15 == game.r15 &&
                       ours.var == game.var && ours.zf == game.zf && (ours.rax == game.rax));
            }
        }
    };
    same(2);  // the switch off
    // Switch off: the game's (bit 4 of the flags, or-ed).
    check(2, 0x11, 0, 1);
    check(2, 0x01, 0, 0);
    check(2, 0x00, 1, 1);
    EXPECT(state.beyondViews == 0 && state.withinViews == 0);
    state.on[code::kLamps] = 1;
    same(1);  // one view
    // One view: still the game's.
    check(1, 0x11, 0, 1);
    check(1, 0x01, 0, 0);
    EXPECT(state.beyondViews == 0 && state.withinViews == 0);
    // Two views: beyond 1, within 2, outside the frustum nothing.
    check(2, 0x11, 0, 1);
    check(2, 0x01, 0, 2);
    check(2, 0x01, 1, 3);
    check(2, 0x00, 2, 2);
    check(3, 0x15, 2, 3);  // other flag bits ignored
    EXPECT(state.beyondViews == 2 && state.withinViews == 2);
    EXPECT(redirect.Restore());
}

// --- B: the decision --------------------------------------------------------------------------------------------------

void TestDecide()
{
    std::puts("lamps (B), the decision: no shadow only when the byte is 1 (beyond and nobody within); 3 counted as kept");
    StandIn                     s(code::kDecideSite & 0xF, code::kDecideCompare, sizeof(code::kDecideCompare));
    alignas(16) code::State     state;
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(s.At(s.site), code::kDecideCompare, code::DecideCode(state), s.At(s.resume)));
    for (std::uint8_t var : {0, 1, 2, 3})
    {
        Context c;
        c.rcx = 5;  // shadows to build
        c.r13 = 0;
        // The stand-in writes the context's var byte to [rsp+42h].
        c.var = var;
        // Run with var preloaded (the stand-in reads it from the context).
        Context in = c;
        s.fn(&in);
        EXPECT((in.rcx & 0xFFFFFFFF) == (var == 1 ? 0u : 5u));
        EXPECT(in.var == var);
    }
    EXPECT(state.kept == 1);
    EXPECT(redirect.Restore());
}

// --- B: the per-view hide ---------------------------------------------------------------------------------------------

void TestHide()
{
    std::puts("lamps (B), the hide: a view beyond the cutoff skips a lamp's whole-scene shadow with two views and the switch "
              "on; never the sun's, a per-object shadow, one view, or the switch off");
    StandIn                     s(code::kHideSite & 0xF, code::kHideAdd, sizeof(code::kHideAdd));
    alignas(16) code::State     state;
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(s.At(s.site), code::kHideAdd, code::HideCode(state, s.At(s.other)), s.At(s.resume)));
    std::vector<std::uint8_t> view(0x2000, 0), infos(0x100, 0), shadow(0x600, 0), renderer(0x100, 0), frame(0x40, 0);
    const auto                infosAt = Address(infos.data());
    std::memcpy(view.data() + 0x17B8, &infosAt, 8);
    const auto rendererAt = Address(renderer.data());
    std::memcpy(frame.data() + 0x30, &rendererAt, 8);
    const auto run = [&](std::uint32_t views, std::uint8_t flags, std::uint8_t kind) {
        std::memcpy(renderer.data() + 0xD8, &views, 4);
        infos[0x40]   = flags;
        shadow[0x568] = kind;
        Context c;
        c.rsi = 0;
        c.rbx = Address(view.data());
        c.r15 = Address(shadow.data());
        c.rbp = Address(frame.data());
        s.fn(&c);
        EXPECT(c.rsi == infosAt);
        return c.other != 0;
    };
    EXPECT(!run(2, 0x11, 0x40));  // off
    state.on[code::kLamps] = 1;
    EXPECT(run(2, 0x11, 0x40));    // beyond, whole-scene, a lamp: skipped in this view
    EXPECT(run(3, 0x11, 0x41));
    EXPECT(!run(1, 0x11, 0x40));   // one view
    EXPECT(!run(2, 0x01, 0x40));   // within
    EXPECT(!run(2, 0x11, 0x50));   // the sun's
    EXPECT(!run(2, 0x11, 0x00));   // a per-object shadow
    EXPECT(state.hidden == 2);
    EXPECT(redirect.Restore());
}

// --- C: the moving casters --------------------------------------------------------------------------------------------

void TestCasters()
{
    std::puts("casters (C): Views[0] with one view or the switch off; else the view nearest the caster (ties: the lower "
              "view); ZF from the light's flag byte");
    StandIn                     s(code::kCasterSite & 0xF, code::kCasterLoad, sizeof(code::kCasterLoad));
    alignas(16) code::State     state;
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(s.At(s.site), code::kCasterLoad, code::CasterCode(state), s.At(s.resume)));
    std::vector<std::uint8_t> views(code::kViewStride * code::kMaxViews, 0), renderer(0x100, 0), caster(0x200, 0),
        proxy(0x200, 0);
    const auto viewsAt = Address(views.data());
    std::memcpy(renderer.data() + 0xD0, &viewsAt, 8);
    const auto place = [&](std::size_t view, float x, float y, float z) {
        const float o[3] = {x, y, z};
        std::memcpy(views.data() + view * code::kViewStride + 0x4AC, o, sizeof(o));
    };
    const float centre[3] = {1000.0f, 0.0f, 50.0f};
    std::memcpy(caster.data() + 0xF0, centre, sizeof(centre));
    place(0, -5000.0f, 0.0f, 0.0f);
    place(1, 900.0f, 100.0f, 0.0f);
    place(2, 1100.0f, 0.0f, 300.0f);
    place(3, 1000.0f, 0.0f, 50.0f);
    const auto run = [&](std::uint32_t count, std::uint8_t flag) {
        std::memcpy(renderer.data() + 0xD8, &count, 4);
        proxy[0x18C] = flag;
        Context c;
        c.r12 = Address(renderer.data());
        c.r15 = Address(caster.data());
        c.r14 = Address(proxy.data());
        s.fn(&c);
        EXPECT(c.zf == (flag == 0 ? 1 : 0));
        return (c.rax - viewsAt) / code::kViewStride;
    };
    EXPECT(run(3, 0) == 0);  // off
    state.on[code::kCasters] = 1;
    EXPECT(run(1, 1) == 0);  // one view
    EXPECT(run(2, 0) == 1);
    EXPECT(run(3, 1) == 1);  // view 2 is farther (300 up)
    EXPECT(run(4, 0) == 3);  // view 3 stands on the caster
    place(2, 900.0f, 100.0f, 0.0f);  // a tie between views 1 and 2: the lower
    EXPECT(run(3, 0) == 1);
    EXPECT(state.nearCasters == 4);
    place(0, 1000.0f, 0.0f, 40.0f);  // view 0 the nearest of three: Views[0], not counted
    EXPECT(run(3, 0) == 0);
    EXPECT(state.nearCasters == 4);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    place(0, -5000.0f, 0.0f, 0.0f);
    place(1, nan, 0.0f, 0.0f);  // a view origin not a number: never chosen
    EXPECT(run(3, 0) == 2);
    EXPECT(redirect.Restore());
}

// --- A: each view's set at the light-LOD loop, the sun's at its cascade setup ----------------------------------------------

code::ViewRec Record(const void* scene, const void* state, const void* sun, std::uint8_t fill)
{
    code::ViewRec r;
    r.valid = 1;
    r.scene = scene;
    r.state = state;
    r.sun   = sun;
    std::memset(r.sceneFields, fill, sizeof(r.sceneFields));
    r.sunDistance = 0x10000000u * fill;
    for (std::uint32_t& c : r.cascades)
    {
        c = 0x01010101u * fill;
    }
    r.biasSharpen[0] = r.biasSharpen[1] = 0x02020202u * fill;
    return r;
}

void TestArea()
{
    std::puts("areas (A): the view's own record when its scene and state match, else the game's, else nothing; off: nothing");
    StandIn                     s(code::kAreaSite & 0xF, code::kAreaAdd, sizeof(code::kAreaAdd));
    alignas(16) static code::State state;
    lss::platform::CodeRedirect redirect;
    static std::int32_t         family = -1;  // render/family_now.h: no split frame
    EXPECT(redirect.Install(s.At(s.site), code::kAreaAdd, code::AreaCode(state, &family), s.At(s.resume)));
    std::vector<std::uint8_t> views(code::kViewStride * 2, 0), renderer(0x100, 0), scene(0x2000, 0), frame(0x40, 0);
    const auto                viewsAt = Address(views.data());
    std::memcpy(renderer.data() + 0xD0, &viewsAt, 8);
    const std::uint64_t states[2] = {0xA0, 0xA1};
    std::memcpy(views.data() + code::kViewState, &states[0], 8);
    std::memcpy(views.data() + code::kViewStride + code::kViewState, &states[1], 8);
    const auto run = [&](std::uint64_t index) {
        std::memset(scene.data() + code::kSceneFields, 0xEE, 24);
        std::memcpy(frame.data() + 0x10, &index, 8);
        Context c;
        c.rcx = Address(scene.data());
        c.r12 = Address(renderer.data());
        c.r13 = index * code::kViewStride;
        c.rbp = Address(frame.data());
        s.fn(&c);
        EXPECT(c.r13 == viewsAt + index * code::kViewStride && c.rcx == Address(scene.data()));
        return scene[code::kSceneFields];
    };
    state.records[1] = Record(scene.data(), reinterpret_cast<const void*>(states[1]), nullptr, 0x11);
    state.game       = Record(scene.data(), nullptr, nullptr, 0x22);
    EXPECT(run(1) == 0xEE);  // off
    state.on[code::kAreas] = 1;
    EXPECT(run(1) == 0x11);  // its own
    EXPECT(run(0) == 0x22);  // no record: the game's
    state.records[1].state = reinterpret_cast<const void*>(0xBB);
    EXPECT(run(1) == 0x22);  // another view's state (a capture): the game's
    state.records[1].state = reinterpret_cast<const void*>(states[1]);
    state.records[1].scene = nullptr;
    EXPECT(run(1) == 0x22);  // another scene
    state.game.scene = nullptr;
    EXPECT(run(1) == 0xEE);  // nothing of this scene: untouched
    EXPECT(state.areaOwn == 1 && state.areaGame == 3);
    // One family per player: every view is the renderer's view 0, and the family index picks the record.
    state.records[1].scene = scene.data();
    state.game.scene       = scene.data();
    std::memcpy(views.data() + code::kViewState, &states[1], 8);  // the renderer's view 0 is player 2's
    family = 1;
    EXPECT(run(0) == 0x11);  // its own, by the family index
    family = 0;
    EXPECT(run(0) == 0x22);  // record 0 is empty: the game's
    family = 1;
    std::memcpy(views.data() + code::kViewState, &states[0], 8);
    EXPECT(run(0) == 0x22);  // the family index names a record of another view state: the game's
    family = -1;
    EXPECT(state.areaOwn == 2 && state.areaGame == 5);
    EXPECT(redirect.Restore());
}

void TestSun()
{
    std::puts("sun (A): the view's own cascade fields on the sun the controllers drive; another light untouched; rdx kept");
    StandIn                     s(code::kSunSite & 0xF, code::kSunAdd, sizeof(code::kSunAdd));
    alignas(16) static code::State state;
    lss::platform::CodeRedirect redirect;
    static std::int32_t         family = -1;
    EXPECT(redirect.Install(s.At(s.site), code::kSunAdd, code::SunCode(state, &family), s.At(s.resume)));
    std::vector<std::uint8_t> views(code::kViewStride * 2, 0), renderer(0x100, 0), scene(0x2000, 0), light(0x40, 0),
        proxy(0x200, 0);
    const auto viewsAt = Address(views.data()), sceneAt = Address(scene.data()), lightAt = Address(light.data()),
               proxyAt = Address(proxy.data());
    std::memcpy(renderer.data() + 0x08, &sceneAt, 8);
    std::memcpy(renderer.data() + 0xD0, &viewsAt, 8);
    std::memcpy(scene.data() + code::kSceneSunLight, &lightAt, 8);
    std::memcpy(light.data() + code::kLightProxy, &proxyAt, 8);
    const std::uint64_t viewState = 0xC1;
    std::memcpy(views.data() + code::kViewStride + code::kViewState, &viewState, 8);
    const auto run = [&](std::uint64_t index, std::uint64_t theLight) {
        std::memset(proxy.data(), 0xEE, proxy.size());
        Context c;
        c.rax     = index;
        c.rsi     = index * code::kViewStride;
        c.rdi     = Address(renderer.data());
        c.rdx     = 0x5555;
        c.stack58 = theLight;
        s.fn(&c);
        EXPECT(c.rdx == 0x5555 && c.rax == index && c.rsi == viewsAt + index * code::kViewStride);
        std::uint32_t cascade0 = 0, distance = 0, bias = 0;
        std::memcpy(&cascade0, proxy.data() + code::kSunCascades, 4);
        std::memcpy(&distance, proxy.data() + code::kSunDistance, 4);
        std::memcpy(&bias, proxy.data() + code::kSunBiasSharpen, 4);
        EXPECT(proxy[code::kSunCascades - 1] == 0xEE && proxy[code::kSunCascades + 20] == 0xEE);  // nothing around them
        return std::array<std::uint32_t, 3>{cascade0, distance, bias};
    };
    const auto mine = Record(scene.data(), reinterpret_cast<const void*>(viewState), proxy.data(), 0x03);
    state.records[1] = mine;
    state.game       = Record(scene.data(), nullptr, proxy.data(), 0x04);
    EXPECT(run(1, lightAt)[0] == 0xEEEEEEEEu);  // off
    state.on[code::kSun] = 1;
    const auto own = run(1, lightAt);
    EXPECT(own[0] == mine.cascades[0] && own[1] == mine.sunDistance && own[2] == mine.biasSharpen[0]);
    EXPECT(run(0, lightAt)[0] == state.game.cascades[0]);        // view 0 has no record: the game's
    EXPECT(run(1, lightAt + 8)[0] == 0xEEEEEEEEu);               // another directional light: untouched
    state.records[1].sun = nullptr;
    EXPECT(run(1, lightAt)[0] == state.game.cascades[0]);        // its record is of another proxy: the game's
    EXPECT(state.sunOwn == 1 && state.sunGame == 2);
    // One family per player: the renderer's view 0 with player 2's state; rax (the view index) is kept (run checks).
    state.records[1] = mine;
    std::memcpy(views.data() + code::kViewState, &viewState, 8);
    family = 1;
    EXPECT(run(0, lightAt)[0] == mine.cascades[0]);
    family = 2;
    EXPECT(run(0, lightAt)[0] == state.game.cascades[0]);  // no record 2: the game's
    family = -1;
    EXPECT(state.sunOwn == 2 && state.sunGame == 3);
    EXPECT(redirect.Restore());
}

void TestFog()
{
    std::puts("fog (A): the view's own fog when its fog, component and state match, else the game's, else nothing; off: nothing");
    StandIn                     s(code::kFogSite & 0xF, code::kFogLoad, sizeof(code::kFogLoad));
    alignas(16) static code::State state;
    lss::platform::CodeRedirect redirect;
    static std::int32_t         family = -1;
    EXPECT(redirect.Install(s.At(s.site), code::kFogLoad, code::FogCode(state, &family), s.At(s.resume)));
    std::vector<std::uint8_t> scene(0x2000, 0), fog(0xC8, 0), view(0x10, 0);
    const auto fogAt = Address(fog.data()), component = std::uint64_t{0xC0C0};
    std::memcpy(scene.data() + code::kSceneFogs, &fogAt, 8);
    std::memcpy(fog.data(), &component, 8);
    const std::uint64_t viewState = 0xD1;
    std::memcpy(view.data() + code::kViewState, &viewState, 8);
    const auto record = [&](std::uint8_t fill, std::uint64_t state, std::uint64_t comp) {
        code::FogRec r;
        r.valid     = 1;
        r.fog       = fog.data();
        r.component = reinterpret_cast<const void*>(comp);
        r.state     = reinterpret_cast<const void*>(state);
        std::memset(r.heightToOpacity, fill, sizeof(r.heightToOpacity));
        std::memset(r.inscattering, fill + 1, sizeof(r.inscattering));
        std::memset(r.appendix, fill + 2, sizeof(r.appendix));
        r.lastField = fill * 0x01010101u;
        return r;
    };
    // Returns the fog's first field byte; checks the other spans moved with it and the bytes around them did not.
    const auto run = [&](std::uint64_t index) {
        std::memset(fog.data() + 8, 0xEE, fog.size() - 8);
        Context c;
        c.rbx = Address(scene.data());
        c.r15 = index;
        c.rdi = Address(view.data());
        c.rax = c.rcx = c.rdx = 0x1234;
        s.fn(&c);
        EXPECT(c.rbx == fogAt && c.r15 == index && c.rdi == Address(view.data()));
        const std::uint8_t first = fog[0x08];
        EXPECT(fog[0x17] == first && fog[0x18] == 0xEE && fog[0x23] == 0xEE && fog[0x33] == fog[0x24] && fog[0x34] == 0xEE);
        EXPECT(fog[0xAB] == fog[0x9C] && fog[0xAC] == 0xEE && fog[0x9B] == 0xEE && fog[0xC4] == 0xEE);
        if (first != 0xEE)
        {
            EXPECT(fog[0x24] == first + 1 && fog[0x9C] == first + 2 && fog[0xC0] == first && fog[0xC3] == first);
        }
        return first;
    };
    state.fogs[1] = record(0x11, viewState, component);
    state.fogGame = record(0x22, 0, component);
    EXPECT(run(1) == 0xEE);  // off
    state.on[code::kFog] = 1;
    EXPECT(run(1) == 0x11);  // its own
    EXPECT(run(0) == 0x22);  // no record: the game's
    EXPECT(run(5) == 0x22);  // past the records: the game's
    state.fogs[1].state = reinterpret_cast<const void*>(0xBB);
    EXPECT(run(1) == 0x22);  // another view's state (a capture): the game's
    state.fogs[1].state     = reinterpret_cast<const void*>(viewState);
    state.fogs[1].component = reinterpret_cast<const void*>(0xC1C1);
    EXPECT(run(1) == 0x22);  // another fog component at that address: the game's
    state.fogs[1].component = reinterpret_cast<const void*>(component);
    state.fogs[1].fog       = nullptr;
    EXPECT(run(1) == 0x22);  // another fog
    state.fogGame.component = reinterpret_cast<const void*>(0xC1C1);
    EXPECT(run(1) == 0xEE);  // nothing of this fog: untouched
    EXPECT(state.fogOwn == 1 && state.fogShared == 5);
    state.fogGame.component = reinterpret_cast<const void*>(component);
    state.fogs[1]           = record(0x11, viewState, component);
    state.fogs[1].valid     = 0;
    EXPECT(run(1) == 0x22);  // its record not valid, all else matching: the game's
    state.fogGame.fog = nullptr;
    EXPECT(run(1) == 0xEE);  // the game's record of another fog: untouched
    state.fogGame.fog   = fog.data();
    state.fogGame.valid = 0;
    EXPECT(run(1) == 0xEE);  // no game's record: untouched
    std::uint64_t kept = 0;
    std::memcpy(&kept, fog.data(), 8);
    EXPECT(kept == component);  // the fog's component is never written
    // One family per player: the view is the renderer's view 0 (r15 = 0), the family index picks the record.
    state.fogGame = record(0x22, 0, component);
    state.fogs[1] = record(0x11, viewState, component);
    family        = 1;
    EXPECT(run(0) == 0x11);
    family = 3;
    EXPECT(run(0) == 0x22);  // no record 3: the game's
    family = 7;
    EXPECT(run(0) == 0x22);  // past the records: the game's
    family = -1;
    EXPECT(run(0) == 0x22);  // no split frame: the renderer's index 0, no record

    // A round trip through FogField (the mapping the mod's own copies use): a record taken from the fog's bytes, the
    // fog changed, the stub puts the same bytes back.
    std::vector<std::uint8_t> before(fog.size());
    for (std::size_t i = 8; i < fog.size(); ++i)
    {
        fog[i] = static_cast<std::uint8_t>(i * 7 + 1);
    }
    before = fog;
    code::FogRec taken;
    for (std::size_t i = 0; i < 4; ++i)
    {
        std::memcpy(code::FogField(taken, i), fog.data() + code::kFogFieldSpans[i][0], code::kFogFieldSpans[i][1]);
    }
    taken.valid      = 1;
    taken.fog        = fog.data();
    taken.component  = reinterpret_cast<const void*>(component);
    taken.state      = reinterpret_cast<const void*>(viewState);
    state.fogs[1]    = taken;
    state.fogGame    = taken;
    Context c;
    for (std::size_t i = 0; i < 4; ++i)
    {
        std::memset(fog.data() + code::kFogFieldSpans[i][0], 0, code::kFogFieldSpans[i][1]);
    }
    c.rbx = Address(scene.data());
    c.r15 = 1;
    c.rdi = Address(view.data());
    s.fn(&c);
    EXPECT(fog == before);
    EXPECT(redirect.Restore());
}

}  // namespace

int main()
{
    TestGather();
    TestDecide();
    TestHide();
    TestCasters();
    TestArea();
    TestSun();
    TestFog();
    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all passed");
    return 0;
}
