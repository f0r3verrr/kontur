#pragma once
#include <windows.h>

namespace appearance {
constexpr COLORREF kBackground = RGB(14, 15, 26);
constexpr COLORREF kSidebar = RGB(18, 18, 33);
constexpr COLORREF kSurface = RGB(24, 26, 43);
constexpr COLORREF kLine = RGB(53, 51, 80);
constexpr COLORREF kInk = RGB(237, 241, 250);
constexpr COLORREF kQuiet = RGB(170, 170, 195);
constexpr COLORREF kAccent = RGB(192, 168, 255);
constexpr COLORREF kAmber = RGB(255, 192, 128);

inline void DarkCaption(HWND window) {
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    using SetAttribute = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto set = reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    BOOL dark = TRUE;
    if (set) {
        set(window, 20, &dark, sizeof(dark));
        set(window, 35, &kBackground, sizeof(kBackground)); // DWMWA_CAPTION_COLOR (Windows 11)
        set(window, 34, &kLine, sizeof(kLine));             // DWMWA_BORDER_COLOR
        set(window, 36, &kInk, sizeof(kInk));               // DWMWA_TEXT_COLOR
    }
    FreeLibrary(dwm);
}
}
