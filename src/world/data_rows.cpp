#include "world/data_rows.h"

#include "engine_facts.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/game_globals.h"

#include <array>

namespace lss::world::data_rows {

namespace {

using FindRowFn = void* (*)(void* table, std::int64_t name);

std::array<std::uintptr_t, 4> g_tables{};  // by Table; 0: not found

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto triggers = image.Rebase(facts::kStateTriggerTablesGlobal, 3 * sizeof(void*));
    const auto states   = image.Rebase(facts::kStateChangeTablesGlobal, 3 * sizeof(void*));
    if (!triggers || !states)
    {
        platform::Log(L"data rows: the state tables are not where the facts say; nothing reads the level's state data");
        return false;
    }
    g_tables[static_cast<std::size_t>(Table::Trigger)]     = *triggers;
    g_tables[static_cast<std::size_t>(Table::StateChange)] = *states;
    const auto acts   = players::game_globals::Address(players::game_globals::Global::TableAction);
    const auto groups = image.Rebase(facts::kActionGroupTablesGlobal, 3 * sizeof(void*));
    if (acts != 0 && groups)
    {
        g_tables[static_cast<std::size_t>(Table::Action)]      = acts;
        g_tables[static_cast<std::size_t>(Table::ActionGroup)] = *groups;
    }
    else
    {
        platform::Log(L"data rows: the act tables are out of the image; a level's acts are not read");
    }
    return true;
}

bool Installed(Table table) noexcept
{
    return Tables(table) != 0;
}

std::uintptr_t Tables(Table table) noexcept
{
    return g_tables[static_cast<std::size_t>(table)];
}

const std::uint8_t* Row(Table table, std::int64_t name) noexcept
{
    const std::uintptr_t tables = Tables(table);
    if (tables == 0 || name == 0)
    {
        return nullptr;
    }
    for (std::uint32_t k = 0; k < 3; ++k)
    {
        void* one = nullptr;
        if (!platform::Read(reinterpret_cast<const void*>(tables + 8 * k), one) || one == nullptr)
        {
            continue;
        }
        void* vtable = platform::ReadPointer(one, 0);
        auto  find   = reinterpret_cast<FindRowFn>(platform::ReadPointer(vtable, facts::kDataTableFindRowSlot));
        void* row    = nullptr;
        if (find != nullptr && platform::CallGuarded(find, row, one, name) && row != nullptr)
        {
            return static_cast<const std::uint8_t*>(row);
        }
    }
    return nullptr;
}

}  // namespace lss::world::data_rows
