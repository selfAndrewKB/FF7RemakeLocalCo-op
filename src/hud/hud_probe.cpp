#include "hud/hud_probe.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/overlay_pass.h"
#include "hud/user_widgets.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <format>
#include <string>
#include <string_view>

namespace lss::hud::hud_probe {

namespace {

constexpr int kColumns = overlay_pass::kMaxColumns;

using RC::Unreal::UClass;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

const std::uint8_t* const* g_overlay        = nullptr;  // &g_SEndRootOverlay
UClass*                    g_endWidgetClass = nullptr;  // /Script/EndGame.EndUserWidget
bool                       g_listKept       = false;

template <class T>
bool ReadAt(const void* base, std::uint32_t offset, T& out) noexcept
{
    return base != nullptr && platform::CopyGuarded(&out, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
}

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

const wchar_t* VisibilityName(std::uint8_t visibility) noexcept
{
    switch (visibility)
    {
    case 0: return L"visible";
    case 1: return L"collapsed";
    case 2: return L"hidden";
    case 3: return L"hit-invisible";
    case 4: return L"self-hit-invisible";
    default: return L"?";
    }
}

/// The engine list's local players' GUObjectArray indexes (-1 where absent), for naming a widget's PlayerContext: the
/// list's place is the column (players/seats.h).
void LocalPlayerIndexes(std::int32_t (&indexes)[kColumns]) noexcept
try
{
    std::fill(std::begin(indexes), std::end(indexes), -1);
    UObject* instance = UObjectGlobals::FindFirstOf(STR("GameInstance"));
    auto*    players  = instance == nullptr
                            ? nullptr
                            : instance->GetValuePtrByPropertyNameInChain<RC::Unreal::TArray<UObject*>>(STR("LocalPlayers"));
    for (std::int32_t i = 0; players != nullptr && i < players->Num() && i < kColumns; ++i)
    {
        if ((*players)[i] != nullptr)
        {
            indexes[i] = static_cast<std::int32_t>((*players)[i]->GetInternalIndex());
        }
    }
}
catch (...)
{
    std::fill(std::begin(indexes), std::end(indexes), -1);
}

/// Whose a widget's PlayerContext is: "p1", a seat's "p2"/"p3", "mirror" for a mirror's column, else "none"/"other".
const wchar_t* ContextName(std::int32_t context, const std::int32_t (&players)[kColumns]) noexcept
{
    if (context < 0)
    {
        return L"none";
    }
    constexpr const wchar_t* kSeatTags[] = {L"p1", L"p2", L"p3", L"p4"};
    for (int column = 0; column < kColumns; ++column)
    {
        if (players[column] == context)
        {
            const int seat = column == 0 ? 0 : overlay_pass::SeatOf(column);
            return seat >= 0 && seat < static_cast<int>(std::size(kSeatTags)) ? kSeatTags[seat] : L"mirror";
        }
    }
    return L"other";
}

std::wstring Names(UObject* object)
{
    try
    {
        return std::format(L"{} {}", object->GetClassPrivate()->GetName(), object->GetName());
    }
    catch (...)
    {
        return L"(unnamed)";
    }
}

enum class Mode
{
    Roots,
    Shown,
    All,
};

/// One line per widget that passes the mode and the filter. Returns how many were logged.
int Dump(Mode mode, std::wstring_view filter)
{
    std::int32_t players[kColumns];
    LocalPlayerIndexes(players);
    int listed = 0, seen = 0;
    for (const auto& widget : user_widgets::Live())
    {
        UObject* object = widget.object;
        ++seen;
        const bool endWidget = g_endWidgetClass != nullptr && object->IsA(g_endWidgetClass);
        void*        canvas  = nullptr;
        std::int32_t zOrder  = 0;
        const bool   root    = endWidget && ReadAt(object, facts::kEndWidgetRootCanvas, canvas) && canvas != nullptr &&
                          ReadAt(object, facts::kEndWidgetZOrder, zOrder);
        std::uint8_t visibility = 0xFF;
        float        color[4]{};
        Vec2         position, size, alignment;
        float        anchors[4]{};
        std::int32_t context = -1;
        if (!ReadAt(object, facts::kWidgetVisibility, visibility) || !ReadAt(object, facts::kUserWidgetColor, color) ||
            !ReadAt(object, facts::kUserWidgetOffset, position) || !ReadAt(object, facts::kUserWidgetOffset + 8, size) ||
            !ReadAt(object, facts::kUserWidgetAnchors, anchors) || !ReadAt(object, facts::kUserWidgetAlignment, alignment) ||
            !ReadAt(object, facts::kUserWidgetPlayerWeak, context))
        {
            continue;
        }
        const bool shown = visibility != 1 && visibility != 2 && color[3] > 0.0f;
        if ((mode != Mode::All && !root) || (mode == Mode::Shown && !shown))
        {
            continue;
        }
        const std::wstring names = Names(object);
        if (!filter.empty() && names.find(filter) == std::wstring::npos)
        {
            continue;
        }
        const wchar_t* owner = ContextName(context, players);
        platform::Log(L"hud: {} {} z={} ctx={} {} a={:.2f} pos=({:.0f},{:.0f}) size=({:.0f},{:.0f}) anchors=({:.2f},{:.2f},{:.2f},{:.2f}) "
                      L"align=({:.2f},{:.2f})",
                      root ? L"root " : L"child", names, root ? zOrder : 0, owner, VisibilityName(visibility), color[3],
                      position.x, position.y, size.x, size.y, anchors[0], anchors[1], anchors[2], anchors[3], alignment.x,
                      alignment.y);
        ++listed;
    }
    platform::Log(L"hud: {} of {} user widgets listed", listed, seen);
    return listed;
}

std::wstring OverlayText() noexcept
{
    const std::uint8_t* overlay = nullptr;
    if (g_overlay == nullptr || !platform::CopyGuarded(&overlay, g_overlay, sizeof(overlay)) || overlay == nullptr)
    {
        return L"no overlay";
    }
    Vec2         min, origin, uiSize, hostSize;
    float        scale = 0.0f;
    std::int32_t stamp = 0;
    float        fade[5]{};  // alpha, from, to, elapsed, duration (SEndRootOverlay__vf1_AdvanceFade_WorldDelta)
    if (!ReadAt(overlay, facts::kOverlayLayoutMin, min) || !ReadAt(overlay, facts::kOverlayHostOrigin, origin) ||
        !ReadAt(overlay, facts::kOverlayUiSize, uiSize) || !ReadAt(overlay, facts::kOverlayHostSize, hostSize) ||
        !ReadAt(overlay, facts::kOverlayHostScale, scale) || !ReadAt(overlay, facts::kOverlayRenderStamp, stamp) ||
        !ReadAt(overlay, facts::kOverlayFade, fade))
    {
        return L"overlay unreadable";
    }
    return std::format(L"min ({:.0f},{:.0f}) ui ({:.0f}x{:.0f}) host ({:.0f},{:.0f}) {:.0f}x{:.0f} scale {:.3f} frame {} fade {:.2f} "
                       L"({:.2f}->{:.2f}, {:.2f}/{:.2f} s)",
                       min.x, min.y, uiSize.x, uiSize.y, origin.x, origin.y, hostSize.x, hostSize.y, scale, stamp, fade[0],
                       fade[1], fade[2], fade[3], fade[4]);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (const auto overlay = image.Rebase(facts::kRootOverlayGlobal, sizeof(void*)))
    {
        g_overlay = reinterpret_cast<const std::uint8_t* const*>(*overlay);
    }
    try
    {
        g_endWidgetClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/EndGame.EndUserWidget"));
    }
    catch (...)
    {
        g_endWidgetClass = nullptr;
    }
    g_listKept = user_widgets::Install();

    using control::Outcome;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"hud.dump",
              .shape = Shape::Text,
              .help  = L"log the HUD's user widgets: roots (default) | shown | all, then optional text the class or name holds",
              .set   = [](const Value& value) {
                  const std::wstring& text  = std::get<std::wstring>(value);
                  const auto          space = text.find(L' ');
                  const std::wstring  word  = text.substr(0, space);
                  const std::wstring  rest  = space == std::wstring::npos ? L"" : text.substr(space + 1);
                  const bool          named = word == L"roots" || word == L"shown" || word == L"all";
                  const Mode          mode  = word == L"shown" ? Mode::Shown : word == L"all" ? Mode::All : Mode::Roots;
                  const std::wstring  filter = named ? rest : text;  // no mode word: roots, the whole line the filter
                  try
                  {
                      return Outcome{true, std::format(L"{} listed (see the log)", Dump(mode, filter))};
                  }
                  catch (...)
                  {
                      return Outcome{false, L"the dump failed (an exception)"};
                  }
              }});
    Register({.name  = L"hud.overlay",
              .shape = Shape::Text,
              .help  = L"the HUD overlay's layout fields: UI rect min and size, marker host origin, size and scale, last render frame",
              .get   = [] { return Value{OverlayText()}; }});
    platform::Log(L"hud probe: hud.dump and hud.overlay ready{}{}", g_overlay ? L"" : L" (no overlay global)",
                  g_listKept ? L"" : L" (no widget list: each dump walks every object)");
    return g_listKept;
}

void Remove() noexcept
{
    user_widgets::Remove();
}

}  // namespace lss::hud::hud_probe
