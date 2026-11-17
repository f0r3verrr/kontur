#pragma once
#include <windows.h>

namespace appearance {
constexpr COLORREF kBackground = RGB(24, 24, 35);
constexpr COLORREF kSidebar = RGB(31, 30, 45);
constexpr COLORREF kSurface = RGB(37, 36, 53);
constexpr COLORREF kLine = RGB(60, 57, 80);
constexpr COLORREF kInk = RGB(241, 239, 250);
constexpr COLORREF kQuiet = RGB(174, 168, 194);
constexpr COLORREF kAccent = RGB(184, 167, 255);
constexpr COLORREF kAmber = RGB(255, 192, 128);

inline void DarkCaption(HWND window) {
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    using SetAttribute = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto set = reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    BOOL dark = TRUE;
    if (set) set(window, 20, &dark, sizeof(dark));
    FreeLibrary(dwm);
}
}
