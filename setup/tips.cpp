#include "setup/tips.h"

#include "setup/theme.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <CommCtrl.h>
#include <uxtheme.h>

namespace lss::setup::tips {

namespace {

HWND g_window = nullptr;  // the app's window
HWND g_tips   = nullptr;
UINT g_dpi    = 96;

int Px(int value)
{
    return MulDiv(value, static_cast<int>(g_dpi), 96);
}

}  // namespace

bool Made() noexcept
{
    return g_tips != nullptr;
}

void Style(HFONT font, unsigned dpi)
{
    g_dpi = dpi;
    if (g_tips == nullptr)
    {
        return;
    }
    RECT margin{Px(10), Px(7), Px(10), Px(7)};
    SendMessageW(g_tips, TTM_SETTIPBKCOLOR, theme::kPanel.bottom, 0);
    SendMessageW(g_tips, TTM_SETTIPTEXTCOLOR, theme::kText, 0);
    SendMessageW(g_tips, TTM_SETMARGIN, 0, reinterpret_cast<LPARAM>(&margin));
    SendMessageW(g_tips, TTM_SETMAXTIPWIDTH, 0, Px(380));
    SendMessageW(g_tips, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void Make(HWND window, std::size_t count, HFONT font, unsigned dpi)
{
    g_window = window;
    g_tips   = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_tips == nullptr)
    {
        return;
    }
    SetWindowTheme(g_tips, L"", L"");  // the system's look off: the control then takes the colours it is given
    Style(font, dpi);
    SendMessageW(g_tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 15000);
    for (std::size_t i = 0; i < count; ++i)
    {
        wchar_t     none[] = L"";
        TTTOOLINFOW tool{};
        tool.cbSize   = sizeof(tool);
        tool.uFlags   = TTF_SUBCLASS;
        tool.hwnd     = window;
        tool.uId      = i + 1;
        tool.lpszText = none;
        SendMessageW(g_tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void Place(std::span<const Tip> parts)
{
    if (g_tips == nullptr)
    {
        return;
    }
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        TTTOOLINFOW tool{};
        tool.cbSize   = sizeof(tool);
        tool.hwnd     = g_window;
        tool.uId      = i + 1;
        tool.rect     = {parts[i].at.left, parts[i].at.top, parts[i].at.right, parts[i].at.bottom};
        tool.lpszText = const_cast<wchar_t*>(parts[i].text);  // copied by the control
        SendMessageW(g_tips, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(g_tips, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void Destroy()
{
    if (g_tips != nullptr)
    {
        DestroyWindow(g_tips);
        g_tips = nullptr;
    }
}

}  // namespace lss::setup::tips
