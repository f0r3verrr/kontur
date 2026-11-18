// Kontur host: connects the desktop shell with the audio and battery modules.
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <initguid.h>
#include <hidclass.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "devices.h"
#include "battery_monitor.h"
#include "voice.h"
#include "music_volume.h"
#include "windows_actions.h"
#include "audio.h"
#include "history.h"
#include "panel.h"
#include "widget.h"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RESULTS = WM_APP + 2;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT WM_AUDIO_CHANGED = WM_APP + 4;
constexpr UINT WM_PANEL_ACTION = WM_APP + 5;
constexpr UINT WM_VOICE = WM_APP + 7;
constexpr UINT WM_UPDATE_RESULT = WM_APP + 8;
constexpr UINT WM_SHOW_PANEL = WM_APP + 6;
constexpr UINT WM_MUSIC_RESULT = WM_APP + 9;
constexpr UINT_PTR TIMER_MUSIC = 3;
constexpr UINT_PTR TIMER_INSTALL_UPDATE = 4;
ULONGLONG g_musicDeadline;
constexpr UINT_PTR TIMER_DEBOUNCE = 1;
constexpr UINT_PTR TIMER_WIDGET = 2;
constexpr int LOW_BATTERY = 15;
constexpr UINT PLACEHOLDER_ID = 1;
constexpr UINT AUDIO_ID = 0xFFFF;

enum MenuId : UINT {
    ID_REFRESH = 100, ID_AUTOSTART, ID_HIDE_LAST_KNOWN, ID_WIDGET_BACKGROUND, ID_EXIT, ID_MODE_WIDGET, ID_MODE_TRAY, ID_SUPPORT,
    ID_OPEN_PANEL, ID_CONFIGURE_AUDIO, ID_CANCEL_TIMER,
    ID_DEVICE_FIRST = 1000
};
enum DeviceCmd : UINT { DEV_TOGGLE, DEV_LEFT, DEV_RIGHT, DEV_HISTORY, DEV_STRIDE };

constexpr wchar_t kAppName[] = L"Контур";
constexpr wchar_t kHostWindow[] = L"KonturHost";
constexpr wchar_t kPreviousWindow[] = L"WirelessBatteryWnd";

HWND RunningHost() {
    if (HWND existing = FindWindowW(kHostWindow, nullptr)) return existing;
    return FindWindowW(kPreviousWindow, nullptr);
}
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Kontur";
constexpr wchar_t kLegacyRunValue[] = L"WirelessBattery";
constexpr wchar_t kStartupApproval[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

const wchar_t* kSettingsKey = L"Software\\WirelessBattery";

HINSTANCE g_inst;
HWND g_hwnd;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar;
bool g_wantWidget = true;      // user's choice: taskbar widget (default) or tray icons
bool g_useWidget = false;      // what is actually shown right now
bool g_hideLastKnown = false;  // hide (last known) / asleep devices from taskbar and tray
bool g_widgetBackground = false;  // draw a plate behind the taskbar text (transparent taskbar themes)

struct TrayEntry {
    UINT uid = 0;
    DeviceStatus st;
    int lastPct = -1;        // last known percentage, kept while the device sleeps
    ULONGLONG lastSeen = 0;
    bool added = false;
    bool lowWarned = false;
    std::wstring iconSig;    // what the current icon shows, to avoid needless redraws
    HICON icon = nullptr;
};
std::map<std::wstring, TrayEntry> g_entries;
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;
HICON g_audioIcon = nullptr;
bool g_audioAdded = false;
bool g_audioHeadphones = false;
bool g_audioLight = false;

// ---- helpers ----------------------------------------------------------------------------------

UINT NextTrayId() {
    // NOTIFYICON_VERSION_4 returns only 16 bits of the icon ID in the click event.
    static UINT id = PLACEHOLDER_ID;
    do {
        id = id == AUDIO_ID - 1 ? PLACEHOLDER_ID + 1 : id + 1;
    } while (std::any_of(g_entries.begin(), g_entries.end(), [](const auto& pair) {
        return pair.second.uid == id;
    }));
    return id;
}

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

void ApplyMenuTheme(bool light) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    static auto flush = ux ? (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136)) : nullptr;
    if (setMode) setMode(light ? 3 : 2);
    if (flush) flush();
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Devices the user chose to hide (by key), stored as a REG_MULTI_SZ.
std::set<std::wstring> g_hidden;

void LoadHidden() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_hidden.insert(p);
}

void SaveHidden() {
    std::wstring multi;
    for (const auto& k : g_hidden) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsHidden(const std::wstring& key) { return g_hidden.count(key) != 0; }

// The user's left-to-right order (device keys), stored as a REG_MULTI_SZ. Devices not in it follow
// in the default order (by type, then name).
std::vector<std::wstring> g_order;

void LoadOrder() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_order.push_back(p);
}

void SaveOrder() {
    if (g_order.size() > 64) g_order.resize(64);  // forget long-gone devices
    std::wstring multi;
    for (const auto& k : g_order) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsEntryHidden(const TrayEntry& e) {
    return IsHidden(e.st.key) || (g_hideLastKnown && !e.st.usbOnly &&
           (e.st.asleep || (e.st.percent < 0 && e.lastPct >= 0)));
}

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

void SaveBoolSetting(const wchar_t* name, bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
void SaveWidgetMode(bool on) { SaveBoolSetting(L"TaskbarWidget", on); }
bool LoadHideLastKnown() { return LoadBoolSetting(L"HideLastKnownDevices", false); }
void SaveHideLastKnown(bool on) { SaveBoolSetting(L"HideLastKnownDevices", on); }
bool LoadWidgetBackground() { return LoadBoolSetting(L"WidgetBackground", false); }
void SaveWidgetBackground(bool on) { SaveBoolSetting(L"WidgetBackground", on); }

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    const auto path = L"\"" + ExePath() + L"\"";
    return _wcsicmp(buf, path.c_str()) == 0 || _wcsicmp(buf, (path + L" --background").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\" --background";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

void MigrateAutostart() {
    wchar_t legacy[1024]{};
    DWORD bytes = sizeof(legacy);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kLegacyRunValue, RRF_RT_REG_SZ, nullptr,
                     legacy, &bytes) != ERROR_SUCCESS) return;
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(legacy, &count);
    if (!args) return;
    const std::wstring path = count > 0 ? args[0] : L"";
    LocalFree(args);
    const auto name = path.substr(path.find_last_of(L"\\/") + 1);
    if (_wcsicmp(name.c_str(), L"WirelessBattery.exe") && _wcsicmp(name.c_str(), L"Kontur.exe")) return;

    bytes = 0;
    const auto existing = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ,
                                       nullptr, nullptr, &bytes);
    if (existing == ERROR_FILE_NOT_FOUND) {
        BYTE approval[64]{};
        bytes = sizeof(approval);
        const auto state = RegGetValueW(HKEY_CURRENT_USER, kStartupApproval, kLegacyRunValue,
                                        RRF_RT_REG_BINARY, nullptr, approval, &bytes);
        if (state == ERROR_SUCCESS) {
            if (RegSetKeyValueW(HKEY_CURRENT_USER, kStartupApproval, kRunValue,
                                REG_BINARY, approval, bytes) != ERROR_SUCCESS) return;
        } else if (state != ERROR_FILE_NOT_FOUND) return;
        const auto command = L"\"" + ExePath() + L"\" --background";
        if (RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(),
                            (DWORD)((command.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) return;
    } else if (existing != ERROR_SUCCESS) return;
    if (RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kLegacyRunValue) == ERROR_SUCCESS)
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kStartupApproval, kLegacyRunValue);
}

// ---- icon rendering ---------------------------------------------------------------------------
// Draws the percentage as large digits with a small level bar underneath. GDI text is rendered
// white-on-black into a scratch bitmap and its brightness is used as the alpha channel, which
// gives smooth edges on the transparent icon.

struct Rgb { BYTE r, g, b; };

HICON RenderIcon(const std::wstring& text, int barPct, Rgb textColor, Rgb barColor, bool dim, bool symbol = false) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    const int S = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : GetDpiForSystem());
    const int barH = symbol ? 0 : std::max(2, S / 8);
    const int textH = symbol ? S : S - barH - 1;

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // scratch bitmap for the glyphs
    const int W = S * 3, H = S * 2;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    void* sbits = nullptr;
    HBITMAP scratch = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, scratch);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    auto* sp = static_cast<DWORD*>(sbits);

    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    for (int fh = textH * 2; fh >= 6; fh--) {
        HFONT f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, symbol ? L"Segoe MDL2 Assets" : L"Segoe UI");
        HGDIOBJ oldF = SelectObject(dc, f);
        SetTextCharacterExtra(dc, text.size() >= 3 ? -1 : 0);
        memset(sbits, 0, (size_t)W * H * 4);
        TextOutW(dc, 2, 0, text.c_str(), (int)text.size());
        GdiFlush();
        SelectObject(dc, oldF);
        DeleteObject(f);
        bx0 = W; by0 = H; bx1 = -1; by1 = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (sp[y * W + x] & 0xFF) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }
        if (bx1 < 0) break;
        if (bx1 - bx0 + 1 <= S && by1 - by0 + 1 <= textH) break;
    }

    // final icon bitmap
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    void* ibits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &ibits, nullptr, 0);
    auto* ip = static_cast<DWORD*>(ibits);
    memset(ip, 0, (size_t)S * S * 4);
    const int alphaScale = dim ? 140 : 255;
    auto put = [&](int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= S || y >= S || a <= 0) return;
        a = a * alphaScale / 255;
        ip[y * S + x] = ((DWORD)a << 24) | ((DWORD)c.r << 16) | ((DWORD)c.g << 8) | c.b;  // straight alpha
    };
    if (bx1 >= 0) {
        int gw = bx1 - bx0 + 1, gh = by1 - by0 + 1;
        int ox = (S - gw) / 2, oy = (textH - gh) / 2;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                put(ox + x, oy + y, textColor, sp[(by0 + y) * W + (bx0 + x)] & 0xFF);
    }
    // level bar: faint track + filled part
    int fill = barPct < 0 ? 0 : std::max(barPct > 0 ? 1 : 0, (barPct * S + 50) / 100);
    for (int y = S - barH; y < S; y++)
        for (int x = 0; x < S; x++) {
            if (x < fill) put(x, y, barColor, 255);
            else put(x, y, g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, 70);
        }

    SelectObject(dc, oldBmp);
    DeleteObject(scratch);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    {
        HDC mdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(mdc, mask);
        PatBlt(mdc, 0, 0, S, S, BLACKNESS);
        SelectObject(mdc, o);
        DeleteDC(mdc);
    }
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

Rgb Foreground() { return g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }; }

HICON IconFor(int pct, bool charging, bool dim) {
    Rgb fg = Foreground();
    Rgb bar = charging        ? Rgb{ 0, 170, 255 }
            : pct <= LOW_BATTERY ? Rgb{ 232, 32, 48 }
            : pct <= 30       ? Rgb{ 255, 150, 0 }
                              : Rgb{ 30, 190, 90 };
    Rgb text = (!charging && pct >= 0 && pct <= LOW_BATTERY) ? Rgb{ 240, 50, 60 } : fg;
    std::wstring t = pct < 0 ? L"?" : std::to_wstring(pct);
    return RenderIcon(t, pct, text, bar, dim);
}

// ---- tray management --------------------------------------------------------------------------

void CopyTip(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = 0;
}

std::wstring StatusText(const TrayEntry& e) {
    const auto& s = e.st;
    if (!s.connected) return L"off / not connected";
    if (s.usbOnly) {
        std::wstring t = L"Connected via USB";
        if (e.lastPct >= 0)
            t += L"\nLast read: " + std::to_wstring(e.lastPct) + L"%, " + history::LastReadingAge(s.key);
        return t;
    }
    std::wstring t;
    if (!s.levelText.empty() && s.percent >= 0) t = s.levelText;
    else if (s.percent >= 0) t = std::to_wstring(s.percent) + L"%";
    else if (e.lastPct >= 0) t = std::to_wstring(e.lastPct) + L"% (last known)";
    else t = L"unknown";
    if (!s.detail.empty() && s.percent >= 0) t += L" (" + s.detail + L")";
    if (s.charging) t += L", charging";
    if (s.asleep) t += L", asleep";
    const auto charge = history::ChargeSummary(s);
    if (!s.charging && !charge.empty()) t += L"\n" + charge;
    return t;
}

void TrayCall(DWORD msg, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"") {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    CopyTip(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (infoTitle) {
        nid.uFlags |= NIF_INFO;
        CopyTip(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), infoTitle);
        CopyTip(nid.szInfo, ARRAYSIZE(nid.szInfo), info);
        nid.dwInfoFlags = NIIF_WARNING;
    }
    Shell_NotifyIconW(msg, &nid);
    if (msg == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void TrayDelete(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void UpdatePlaceholder() {
    if (g_useWidget) {
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
        return;
    }
    bool any = false;
    for (auto& [k, e] : g_entries) any |= e.added;
    if (!any && !g_placeholder) {
        if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
        TrayCall(NIM_ADD, PLACEHOLDER_ID, g_placeholderIcon, L"Wireless Battery\nNo devices with battery info found");
        g_placeholder = true;
    } else if (any && g_placeholder) {
        TrayDelete(PLACEHOLDER_ID);
        g_placeholder = false;
    }
}

void ApplyEntry(TrayEntry& e) {
    const auto& s = e.st;
    bool visible = s.connected && !g_useWidget && !IsEntryHidden(e);
    if (!visible) {
        if (e.added) { TrayDelete(e.uid); e.added = false; }
        return;
    }
    int shown = s.percent >= 0 ? s.percent : e.lastPct;
    bool dim = s.percent < 0;
    std::wstring sig = std::to_wstring(shown) + (s.charging ? L"c" : L"") + (dim ? L"d" : L"") + (g_lightTaskbar ? L"l" : L"k");
    if (s.usbOnly) sig += L"u";
    if (sig != e.iconSig || !e.icon) {
        if (e.icon) DestroyIcon(e.icon);
        e.icon = s.usbOnly ? RenderIcon(L"USB", -1, Foreground(), Foreground(), false)
                           : IconFor(shown, s.charging, dim);
        e.iconSig = sig;
    }
    std::wstring tip = s.name + L"\n" + StatusText(e);

    const wchar_t* infoTitle = nullptr;
    std::wstring info;
    if (s.charging || shown > LOW_BATTERY + 10) e.lowWarned = false;
    if (!s.charging && s.percent >= 0 && s.percent <= LOW_BATTERY && !e.lowWarned) {
        e.lowWarned = true;
        infoTitle = L"Low battery";
        info = s.name + L" is at " + std::to_wstring(s.percent) + L"%";
    }
    TrayCall(e.added ? NIM_MODIFY : NIM_ADD, e.uid, e.icon, tip, infoTitle, info);
    e.added = true;
}

int KindOrder(DeviceKind k) {  // default order: headset first, then mouse, ...
    switch (k) {
        case DeviceKind::Headset:  return 0;
        case DeviceKind::Mouse:    return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad:  return 3;
        case DeviceKind::Phone:    return 4;
        default:                   return 5;
    }
}

bool IsShown(const TrayEntry& e) { return e.st.connected && !IsEntryHidden(e); }

// Entries in left-to-right order: the user's order first, then the rest by type and name. Works the
// same for every kind of device (receivers, Bluetooth, controllers, HeadsetControl) as it only uses
// the device key.
std::vector<TrayEntry*> OrderedEntries(bool shownOnly) {
    std::vector<TrayEntry*> list;
    for (auto& [k, e] : g_entries)
        if (!shownOnly || IsShown(e)) list.push_back(&e);
    auto rank = [](const TrayEntry* e) {
        auto it = std::find(g_order.begin(), g_order.end(), e->st.key);
        return it == g_order.end() ? INT_MAX : (int)(it - g_order.begin());
    };
    std::stable_sort(list.begin(), list.end(), [&](const TrayEntry* a, const TrayEntry* b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
        return ka != kb ? ka < kb : a->st.name < b->st.name;
    });
    return list;
}

void UpdateWidget() {
    if (!g_wantWidget || !widget::Alive()) return;
    std::vector<TrayEntry*> list = OrderedEntries(true);
    std::vector<WidgetItem> items;
    std::wstring tip;
    for (auto* e : list) {
        WidgetItem it;
        it.uid = e->uid;
        it.kind = e->st.kind;
        it.percent = e->st.percent >= 0 ? e->st.percent : e->lastPct;
        if (e->st.percent >= 0) it.text = e->st.levelText;
        it.charging = e->st.charging;
        it.dim = e->st.percent < 0;
        it.low = !it.charging && it.percent >= 0 && it.percent <= LOW_BATTERY;
        it.bluetooth = e->st.via == L"Bluetooth";
        if (e->st.usbOnly) {
            it.text = L"USB";
            it.dim = it.low = false;
        }
        items.push_back(it);
        if (!tip.empty()) tip += L"\n";
        tip += e->st.name + L": " + StatusText(*e);
    }
    if (tip.empty()) tip = L"Контур: no devices with battery info found";
    widget::SetItems(items, tip);
}

void UpdatePanelBatteries() {
    if (!panel::IsOpen()) return;
    std::vector<panel::BatteryRow> rows;
    for (const auto* entry : OrderedEntries(false)) {
        const auto& s = entry->st;
        const int percent = s.connected && !s.usbOnly ? s.percent : -1;
        const std::wstring value = s.usbOnly ? L"USB" : percent < 0 ? L"–" : std::to_wstring(percent) + L"%";
        std::wstring detail;
        if (!s.connected) detail = L"Нет связи";
        else if (s.usbOnly) detail = L"Подключена по USB";
        else if (s.charging) detail = L"Заряжается";
        else if (s.asleep) detail = L"Спит";
        else if (s.via == L"2.4 GHz receiver") detail = L"Приёмник 2,4 ГГц";
        else if (s.via == L"USB cable") detail = L"USB";
        else detail = s.via;
        if (s.connected && !s.charging && percent < 0 && entry->lastPct >= 0)
            detail += L"  ·  Последний замер " + std::to_wstring(entry->lastPct) + L"%";
        rows.push_back({s, detail, value, percent, s.charging,
                        !s.charging && percent >= 0 && percent <= LOW_BATTERY});
    }
    panel::SetBatteries(std::move(rows));
}

void OpenPanel() {
    panel::Show(g_hwnd, WM_PANEL_ACTION);
    UpdatePanelBatteries();
}

void UpdateAudioTray() {
    const auto* active = audio::Find(audio::Current());
    const bool headphones = active && (active->id == audio::Configured(1) ||
                            active->form == 3 || active->form == 5);
    if (!g_audioIcon || g_audioHeadphones != headphones || g_audioLight != g_lightTaskbar) {
        if (g_audioIcon) DestroyIcon(g_audioIcon);
        g_audioHeadphones = headphones;
        g_audioLight = g_lightTaskbar;
        g_audioIcon = RenderIcon(headphones ? L"\uE7F6" : L"\uE767", -1, Foreground(), Foreground(), false, true);
    }
    const auto tip = L"Контур · Звук\n" + (active ? active->Label() : L"Нет активного выхода");
    TrayCall(g_audioAdded ? NIM_MODIFY : NIM_ADD, AUDIO_ID, g_audioIcon, tip);
    g_audioAdded = true;
}

void AudioToast(const std::wstring& text) {
    NOTIFYICONIDENTIFIER icon{sizeof(icon), g_hwnd, AUDIO_ID};
    RECT anchor{};
    if (FAILED(Shell_NotifyIconGetRect(&icon, &anchor))) {
        POINT cursor{};
        GetCursorPos(&cursor);
        anchor = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
    }
    panel::Toast(g_hwnd, anchor, text);
}

void SetVoice(bool enabled) {
    if (!enabled) {
        voice::Stop();
        panel::HideListening();
        panel::UpdateVoice(false, L"Выключены · микрофон свободен");
    } else {
        enabled = voice::Start(g_hwnd, WM_VOICE);
        panel::UpdateVoice(enabled, enabled ? L"Запуск распознавания…" : L"Не удалось запустить");
    }
    SaveBoolSetting(L"VoiceEnabled", enabled);
}

bool g_voiceReply;
std::wstring g_voiceReplyText;
int g_voiceReplyVolume = 100;

void LoadVoiceReply() {
    g_voiceReply = LoadBoolSetting(L"VoiceReplyEnabled", true);
    wchar_t text[121]{};
    DWORD size = sizeof(text);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"VoiceReplyText", RRF_RT_REG_SZ, nullptr,
                     text, &size) == ERROR_SUCCESS && *text)
        g_voiceReplyText = text;
    else g_voiceReplyText = L"Команда выполнена";
    panel::UpdateReply(g_voiceReply, g_voiceReplyText);
    DWORD volume = 100, bytes = sizeof(volume);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"VoiceReplyVolume", RRF_RT_REG_DWORD, nullptr,
                     &volume, &bytes) != ERROR_SUCCESS) volume = 100;
    g_voiceReplyVolume = static_cast<int>((std::min)(volume, 100UL));
    panel::UpdateReplyVolume(g_voiceReplyVolume);
}

void SetVoiceReplyVolume(int volume, bool save) {
    g_voiceReplyVolume = std::clamp(volume, 0, 100);
    panel::UpdateReplyVolume(g_voiceReplyVolume);
    if (save) {
        const DWORD value = g_voiceReplyVolume;
        RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"VoiceReplyVolume", REG_DWORD, &value, sizeof(value));
    }
}

void SaveVoiceReply() {
    auto text = panel::ReplyText();
    const auto first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) {
        AudioToast(L"Введи фразу для голосового ответа");
        return;
    }
    text = text.substr(first, text.find_last_not_of(L" \t") - first + 1);
    const auto size = (DWORD)((text.size() + 1) * sizeof(wchar_t));
    if (RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"VoiceReplyText", REG_SZ, text.c_str(), size) != ERROR_SUCCESS) {
        AudioToast(L"Не удалось сохранить фразу");
        return;
    }
    g_voiceReplyText = std::move(text);
    panel::UpdateReply(g_voiceReply, g_voiceReplyText);
    AudioToast(L"Фраза сохранена");
}

bool SetAudioOutput(const std::wstring& id) {
    const auto error = audio::SwitchTo(id);
    UpdateAudioTray();
    panel::UpdateAudio();
    const auto* active = audio::Find(audio::Current());
    AudioToast(error.empty() && active ? L"Звук: " + active->Label() : error);
    return error.empty();
}

void ConfigureAudio(bool adding = false) {
    audio::Refresh();
    if (!panel::IsOpen()) OpenPanel();
    panel::UpdateAudio();
    panel::ShowOutputSettings(adding);
}

bool SwitchAudio(int slot) {
    audio::Refresh();
    if (slot < 0) {
        const auto cycle = audio::Cycle();
        if (cycle.empty() || (cycle.size() == 1 && cycle.front() == audio::Current())) {
            UpdateAudioTray();
            if (audio::Configured(0).empty() || audio::Configured(1).empty()) OpenPanel();
            AudioToast(L"Для переключения нужен ещё один доступный выход");
            return false;
        }
        auto next = std::find(cycle.begin(), cycle.end(), audio::Current());
        if (next == cycle.end() || ++next == cycle.end()) next = cycle.begin();
        return SetAudioOutput(*next);
    }
    if (audio::Configured(slot).empty()) {
        ConfigureAudio();
        return false;
    }
    return SetAudioOutput(audio::Configured(slot));
}

bool OpenYandexMusic(bool playWave) {
    // Yandex Music Desktop handles play-vibe on both cold start and second-instance launch.
    const wchar_t* url = playWave ? L"yandexmusic://play-vibe?screen=landing&seeds=user%3Aonyourwave"
                                  : L"yandexmusic://home";
    if ((INT_PTR)ShellExecuteW(g_hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL) <= 32) {
        AudioToast(L"Не удалось открыть приложение Яндекс Музыка");
        return false;
    }
    AudioToast(playWave ? L"Запуск «Моей волны»" : L"Открываю Яндекс Музыку");
    return true;
}

bool g_updateBusy, g_manualUpdate, g_updateAvailable;
enum UpdatePhase : unsigned { ProbeUpdate = 1, InstallUpdate };

void StartUpdate(UpdatePhase phase) {
    auto path = ExePath();
    for (size_t pos = 0; (pos = path.find(L'\'', pos)) != std::wstring::npos; pos += 2)
        path.insert(pos, 1, L'\'');
    auto setup = L"$app='" + path + L"';$appPid=" + std::to_wstring(GetCurrentProcessId()) +
                 (g_manualUpdate ? L";$manual=$true" : L";$manual=$false");
    setup += phase == ProbeUpdate ? L";$probe=$true\n" : L";$probe=$false\n";
    g_updateBusy = StartWindowsAction(201, setup.c_str(), g_hwnd, WM_UPDATE_RESULT, false, phase);
    if (!g_updateBusy && g_manualUpdate) {
        g_manualUpdate = false;
        panel::ShowUpdateStatus(3);
    }
}

void CheckUpdates(bool manual) {
    g_manualUpdate = g_manualUpdate || manual;
    if (manual) panel::ShowUpdateStatus(g_updateAvailable ? -2 : -1);
    if (g_updateBusy) return;
    if (manual && g_updateAvailable) {
        KillTimer(g_hwnd, TIMER_INSTALL_UPDATE);
        StartUpdate(InstallUpdate);
    } else StartUpdate(ProbeUpdate);
}

void OnUpdateResult(UpdatePhase phase, int result) {
    g_updateBusy = false;
    if (phase == ProbeUpdate && result == 4) {
        g_updateAvailable = true;
        panel::UpdateAvailable(true);
        if (g_manualUpdate) {
            panel::ShowUpdateStatus(-2);
            StartUpdate(InstallUpdate);
        } else if (!SetTimer(g_hwnd, TIMER_INSTALL_UPDATE, 6000, nullptr)) {
            StartUpdate(InstallUpdate);
        }
        return;
    }
    if (result == 0) {
        g_updateAvailable = false;
        panel::UpdateAvailable(false);
    }
    if (g_manualUpdate) panel::ShowUpdateStatus(result);
    g_manualUpdate = false;
}

void OnResults(std::vector<DeviceStatus>& results) {
    std::map<std::wstring, bool> seen;
    for (auto& s : results) {
        auto& e = g_entries[s.key];
        if (!e.uid) e.uid = NextTrayId();
        if (s.usbOnly && !e.st.name.empty()) s.name = e.st.name;
        history::Observe(s);
        e.st = s;
        if (s.percent >= 0) e.lastPct = s.percent;
        else if (s.usbOnly && e.lastPct < 0) e.lastPct = history::LastPercent(s.key);
        e.lastSeen = GetTickCount64();
        seen[s.key] = true;
        ApplyEntry(e);
    }
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        auto& e = it->second;
        // Some receivers drop sleeping devices from the link; keep showing the last value for a while.
        bool inGrace = !seen.count(it->first) && e.st.graceSec > 0 && e.st.connected &&
                       GetTickCount64() - e.lastSeen < (ULONGLONG)e.st.graceSec * 1000;
        if (inGrace) {
            e.st.percent = -1;
            e.st.asleep = true;
            ApplyEntry(e);
        }
        if (!seen.count(it->first) && !inGrace) {
            if (e.st.usbOnly) history::UsbDisconnected(it->first);
            if (it->second.added) TrayDelete(it->second.uid);
            if (it->second.icon) DestroyIcon(it->second.icon);
            it = g_entries.erase(it);
        } else {
            ++it;
        }
    }
    UpdatePlaceholder();
    UpdateWidget();
    UpdatePanelBatteries();
}

// Makes the actual display match the chosen mode; falls back to tray icons if the taskbar
// widget cannot be created (e.g. Explorer not ready yet). Called once a second.
void Reconcile() {
    if (!g_wantWidget) widget::Destroy();
    else if (!widget::Alive() && widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar))
        UpdateWidget();
    const bool visible = g_wantWidget && widget::Reposition();
    if (visible != g_useWidget) {
        g_useWidget = visible;
        for (auto& [k, e] : g_entries) ApplyEntry(e);
        UpdatePlaceholder();
    }
}

void SetMode(bool useWidget) {
    g_wantWidget = useWidget;
    SaveWidgetMode(useWidget);
    Reconcile();
}

void SetWidgetBackground(bool on) {
    g_widgetBackground = on;
    SaveWidgetBackground(on);
    widget::SetBackground(on);
}

void SetHideLastKnown(bool on) {
    g_hideLastKnown = on;
    SaveHideLastKnown(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void ReAddAll() {
    g_placeholder = false;
    for (auto& [k, e] : g_entries) { e.added = false; ApplyEntry(e); }
    UpdatePlaceholder();
}

void RedrawAll() {
    if (g_placeholderIcon) {
        DestroyIcon(g_placeholderIcon);
        g_placeholderIcon = nullptr;
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
    }
    for (auto& [k, e] : g_entries) { e.iconSig.clear(); ApplyEntry(e); }
    UpdatePlaceholder();
}

void ToggleHidden(const std::wstring& key) {
    if (!g_hidden.erase(key)) g_hidden.insert(key);
    SaveHidden();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdatePlaceholder();
    UpdateWidget();
}

// Swaps a shown device with its neighbour (dir -1 = left, +1 = right) and remembers the new order.
void MoveEntry(const std::wstring& key, int dir) {
    std::vector<TrayEntry*> list = OrderedEntries(true);
    auto pos = std::find_if(list.begin(), list.end(), [&](const TrayEntry* e) { return e->st.key == key; });
    if (pos == list.end()) return;
    ptrdiff_t i = pos - list.begin(), j = i + dir;
    if (j < 0 || j >= (ptrdiff_t)list.size()) return;
    std::swap(list[i], list[j]);
    std::vector<std::wstring> order;
    for (auto* e : list) order.push_back(e->st.key);
    for (auto& k : g_order)  // keep the places of devices that are not shown right now
        if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
    g_order = order;
    SaveOrder();
    UpdateWidget();
}

void MusicReply(const std::wstring& text) {
    AudioToast(text);
    if (g_voiceReply && voice::Running()) voice::Reply(text, g_voiceReplyVolume);
}

bool g_likePending;

void MusicAction(voice::Event event) {
    const bool changesLike = event == voice::LikeTrack || event == voice::RemoveLike;
    if (changesLike && g_likePending) return;
    const wchar_t* setup = event == voice::NextTrack ? L"$mode='next';" :
        event == voice::PreviousTrack ? L"$mode='previous';" :
        event == voice::CurrentTrack ? L"$mode='track';" :
        event == voice::LikeTrack ? L"$mode='like';" :
        event == voice::RemoveLike ? L"$mode='unlike';" :
        event == voice::ResumeMusic ? L"$mode='play';" : L"$mode='pause';";
    if (!StartWindowsAction(202, setup, g_hwnd, WM_MUSIC_RESULT, event == voice::CurrentTrack, (unsigned)event))
        MusicReply(L"Не удалось выполнить команду Яндекс Музыки");
    else if (changesLike)
        g_likePending = true;
}

void MusicVolumeAction(int percent, bool relative, voice::Event event) {
    struct Request { HWND window; int percent; bool relative; voice::Event event; };
    auto* request = new Request{g_hwnd, percent, relative, event};
    const auto run = [](void* data) -> DWORD {
        std::unique_ptr<Request> request(static_cast<Request*>(data));
        const DWORD result = music_volume::Set(request->percent, request->relative) ? 0 : 5;
        PostMessageW(request->window, WM_MUSIC_RESULT, result | (WPARAM(request->event) << 32), 0);
        return 0;
    };
    if (!QueueUserWorkItem(run, request, WT_EXECUTELONGFUNCTION)) {
        delete request;
        MusicReply(L"Не удалось изменить громкость Яндекс Музыки");
    }
}

void CancelMusicTimer() {
    KillTimer(g_hwnd, TIMER_MUSIC);
    g_musicDeadline = 0;
}

void CheckMusicTimer() {
    if (!g_musicDeadline || GetTickCount64() < g_musicDeadline) return;
    CancelMusicTimer();
    MusicAction(voice::PauseMusic);
}

void ShowMenu() {
    ApplyMenuTheme(g_lightTaskbar);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN_PANEL, L"Открыть Контур");
    AppendMenuW(m, MF_STRING, ID_CONFIGURE_AUDIO, L"Настроить звук");
    if (g_musicDeadline) {
        const auto now = GetTickCount64();
        const auto seconds = g_musicDeadline > now ? (g_musicDeadline - now + 999) / 1000 : 0;
        const auto remainder = seconds % 60;
        const auto label = L"Таймер музыки · " + std::to_wstring(seconds / 60) + L":" +
            (remainder < 10 ? L"0" : L"") + std::to_wstring(remainder);
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, label.c_str());
        AppendMenuW(m, MF_STRING, ID_CANCEL_TIMER, L"Отменить таймер музыки");
    }
    SetMenuDefaultItem(m, ID_OPEN_PANEL, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::vector<std::wstring> keys;  // device index -> device key; menu id ID_DEVICE_FIRST + index * DEV_STRIDE + command
    std::vector<TrayEntry*> all = OrderedEntries(false), shown = OrderedEntries(true);
    if (all.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No devices with battery info found");
    else AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Point at a device to show, hide or move it");
    for (TrayEntry* e : all) {
        const std::wstring& k = e->st.key;
        UINT base = ID_DEVICE_FIRST + (UINT)keys.size() * DEV_STRIDE;
        auto pos = std::find(shown.begin(), shown.end(), e);
        bool canLeft = pos != shown.end() && pos != shown.begin();
        bool canRight = pos != shown.end() && pos + 1 != shown.end();
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, base + DEV_HISTORY, L"Battery history");
        AppendMenuW(sub, MF_STRING | (IsHidden(k) ? 0 : MF_CHECKED), base + DEV_TOGGLE,
                    g_wantWidget ? L"Show on the taskbar" : L"Show in the tray");
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING | (canLeft ? 0 : MF_GRAYED), base + DEV_LEFT, g_wantWidget ? L"Move left" : L"Move up");
        AppendMenuW(sub, MF_STRING | (canRight ? 0 : MF_GRAYED), base + DEV_RIGHT, g_wantWidget ? L"Move right" : L"Move down");
        std::wstring line = e->st.name + L":  " + StatusText(*e) + L"   [" + e->st.via + L"]";
        AppendMenuW(m, MF_POPUP | (IsHidden(k) ? 0 : MF_CHECKED), (UINT_PTR)sub, line.c_str());
        keys.push_back(k);
    }
    AppendMenuW(m, MF_STRING | (g_hideLastKnown ? MF_CHECKED : 0), ID_HIDE_LAST_KNOWN, L"Hide last known / asleep devices");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_wantWidget ? MF_CHECKED : 0), ID_MODE_WIDGET, L"Show on the taskbar (icon + percentage)");
    AppendMenuW(m, MF_STRING | (!g_wantWidget ? MF_CHECKED : 0), ID_MODE_TRAY, L"Show as tray icons");
    CheckMenuRadioItem(m, ID_MODE_WIDGET, ID_MODE_TRAY, g_wantWidget ? ID_MODE_WIDGET : ID_MODE_TRAY, MF_BYCOMMAND);
    UINT bgFlags = MF_STRING | (g_widgetBackground ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED);
    AppendMenuW(m, bgFlags, ID_WIDGET_BACKGROUND, L"Background behind taskbar text (for transparent taskbars)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_REFRESH, L"Refresh now");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING, ID_SUPPORT, L"Support the project...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case ID_CANCEL_TIMER: CancelMusicTimer(); AudioToast(L"Таймер музыки отменён"); break;
        case ID_OPEN_PANEL: OpenPanel(); break;
        case ID_CONFIGURE_AUDIO: ConfigureAudio(); break;
        case ID_REFRESH: battery_monitor::Refresh(); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_HIDE_LAST_KNOWN: SetHideLastKnown(!g_hideLastKnown); break;
        case ID_WIDGET_BACKGROUND: SetWidgetBackground(!g_widgetBackground); break;
        case ID_MODE_WIDGET: SetMode(true); break;
        case ID_MODE_TRAY: SetMode(false); break;
        case ID_SUPPORT: ShellExecuteW(nullptr, L"open", L"https://github.com/sponsors/krisgfx9", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        default:
            if (cmd >= ID_DEVICE_FIRST && cmd < ID_DEVICE_FIRST + keys.size() * DEV_STRIDE) {
                std::wstring key = keys[(cmd - ID_DEVICE_FIRST) / DEV_STRIDE];
                switch ((cmd - ID_DEVICE_FIRST) % DEV_STRIDE) {
                    case DEV_TOGGLE: ToggleHidden(key); break;
                    case DEV_LEFT:   MoveEntry(key, -1); break;
                    case DEV_RIGHT:  MoveEntry(key, +1); break;
                    case DEV_HISTORY:
                        if (auto it = g_entries.find(key); it != g_entries.end()) history::Show(g_hwnd, it->second.st);
                        break;
                }
            }
    }
}

void OpenHistoryOrMenu(UINT uid) {
    for (const auto& [key, entry] : g_entries) {
        if (entry.uid == uid) {
            history::Show(g_hwnd, entry.st);
            return;
        }
    }
    ShowMenu();
}

// ---- worker thread ----------------------------------------------------------------------------

void RefreshSoon(UINT ms) { SetTimer(g_hwnd, TIMER_DEBOUNCE, ms, nullptr); }

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {  // Explorer restarted
        widget::Destroy();
        g_useWidget = false;
        ReAddAll();
        g_audioAdded = false;
        UpdateAudioTray();
        Reconcile();
        return 0;
    }
    switch (msg) {
        case WM_RESULTS: {
            auto* v = reinterpret_cast<std::vector<DeviceStatus>*>(lp);
            OnResults(*v);
            delete v;
            return 0;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU: ShowMenu(); break;
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (HIWORD(lp) == AUDIO_ID) SwitchAudio(-1);
                    else OpenHistoryOrMenu(HIWORD(lp));
                    break;
            }
            return 0;
        case WM_AUDIO_CHANGED:
            audio::Refresh();
            UpdateAudioTray();
            panel::UpdateAudio();
            return 0;
        case WM_SHOW_PANEL:
            OpenPanel();
            return 0;
        case WM_PANEL_ACTION:
            if (wp == panel::SelectOutput) SwitchAudio((int)lp);
            if (wp == panel::ConfigureOutput) ConfigureAudio();
            if (wp == panel::AddOutput) ConfigureAudio(true);
            if (wp == panel::SelectExtra) {
                const auto id = panel::ExtraId((size_t)lp);
                if (!id.empty()) SetAudioOutput(id);
            }
            if (wp == panel::AudioSettingsChanged) {
                UpdateAudioTray();
                panel::UpdateAudio();
                if (voice::Running()) {
                    voice::Stop();
                    SetVoice(true);
                }
            }
            if (wp == panel::ToggleVoice) SetVoice(!voice::Running());
            if (wp == panel::ToggleReply) {
                g_voiceReply = !g_voiceReply;
                SaveBoolSetting(L"VoiceReplyEnabled", g_voiceReply);
                panel::UpdateReply(g_voiceReply, g_voiceReplyText);
            }
            if (wp == panel::SaveReply) SaveVoiceReply();
            if (wp == panel::ReplyVolume) SetVoiceReplyVolume(LOWORD(lp), HIWORD(lp) != 0);
            if (wp == panel::CheckUpdates) CheckUpdates(true);
            return 0;
        case WM_UPDATE_RESULT:
            OnUpdateResult(static_cast<UpdatePhase>(wp >> 32), static_cast<int>(LOWORD(wp)));
            return 0;
        case WM_MUSIC_RESULT: {
            std::unique_ptr<std::wstring> title(reinterpret_cast<std::wstring*>(lp));
            const auto action = static_cast<voice::Event>(wp >> 32);
            const auto result = static_cast<DWORD>(wp);
            if (action == voice::LikeTrack || action == voice::RemoveLike) g_likePending = false;
            const bool volume = action == voice::MusicLouder || action == voice::MusicQuieter ||
                (action >= voice::MusicVolume && action <= voice::MusicVolume + 100);
            if (result == 4) MusicReply(L"Яндекс Музыка сейчас не играет");
            else if (result == 5) MusicReply(volume ? L"Ползунок громкости Яндекс Музыки недоступен. Открой окно приложения" :
                L"Кнопка лайка недоступна. Открой окно Яндекс Музыки");
            else if (result == 6) MusicReply(volume ? L"Яндекс Музыка не подтвердила громкость" :
                action == voice::RemoveLike ? L"Яндекс Музыка не подтвердила снятие лайка" :
                L"Яндекс Музыка не подтвердила лайк");
            else if (result) MusicReply(L"Яндекс Музыка не выполнила команду");
            else if (title && !title->empty()) MusicReply(*title);
            else if (action >= voice::MusicVolume && action <= voice::MusicVolume + 100)
                MusicReply(L"Громкость Яндекс Музыки " + std::to_wstring(action - voice::MusicVolume) + L" процентов");
            else if (action == voice::MusicLouder || action == voice::MusicQuieter)
                MusicReply(action == voice::MusicLouder ? L"Сделала музыку громче" : L"Сделала музыку тише");
            else if (action == voice::LikeTrack) MusicReply(L"Трек в коллекции");
            else if (action == voice::RemoveLike) MusicReply(L"У трека нет лайка");
            else if (action == voice::ResumeMusic && g_voiceReply && voice::Running())
                voice::Reply(g_voiceReplyText, g_voiceReplyVolume);
            else if ((action == voice::NextTrack || action == voice::PreviousTrack) && g_voiceReply && voice::Running())
                voice::Reply(g_voiceReplyText, g_voiceReplyVolume);
            return 0;
        }
        case WM_VOICE:
            if (!voice::CurrentEvent(lp)) return 0;
            if (wp >= voice::MusicVolume && wp <= voice::MusicVolume + 100) {
                panel::HideListening();
                if (!voice::Running()) return 0;
                const int percent = int(wp - voice::MusicVolume);
                MusicVolumeAction(percent, false, static_cast<voice::Event>(wp));
                return 0;
            }
            if (wp >= voice::NamedOutput) {
                panel::HideListening();
                if (voice::Running()) {
                    const auto id = voice::OutputId(unsigned(wp - voice::NamedOutput));
                    if (!id.empty()) SetAudioOutput(id);
                }
                return 0;
            }
            if (wp >= voice::DiscordVolume && wp <= voice::DiscordVolume + 100) {
                panel::HideListening();
                if (voice::Running()) {
                    const int percent = int(wp - voice::DiscordVolume);
                    if (audio::AppVolume(false, percent))
                        MusicReply(L"Громкость Discord " + std::to_wstring(percent) + L" процентов");
                    else MusicReply(L"В микшере Windows нет Discord");
                }
                return 0;
            }
            if (wp > voice::MusicTimer && wp <= voice::MusicTimer + 100) {
                panel::HideListening();
                if (!voice::Running()) return 0;
                const auto minutes = UINT(wp - voice::MusicTimer);
                const auto duration = minutes * 60000;
                if (SetTimer(h, TIMER_MUSIC, duration, nullptr)) {
                    g_musicDeadline = GetTickCount64() + duration;
                    MusicReply(L"Таймер музыки установлен");
                } else MusicReply(L"Не удалось установить таймер");
                return 0;
            }
            if (wp >= voice::Scenario && wp < voice::Scenario + voice::ScenarioCount) {
                panel::HideListening();
                if (!voice::Running()) return 0;
                const unsigned actions = voice::LoadProfiles()[wp - voice::Scenario].actions;
                if (!actions) return 0;
                bool ok = true;
                if (actions & voice::Headset) ok = SwitchAudio(1);
                if (actions & voice::Speaker) ok = SwitchAudio(0);
                if (actions & voice::Pause) MusicAction(voice::PauseMusic);
                if (actions & voice::Discord) {
                    const auto result = ShellExecuteW(h, L"open", L"discord://-/channels/@me", nullptr, nullptr, SW_SHOWNORMAL);
                    ok = reinterpret_cast<INT_PTR>(result) > 32 && ok;
                }
                if (!ok) MusicReply(L"Часть действий сценария не выполнена");
                else if (g_voiceReply) voice::Reply(g_voiceReplyText, g_voiceReplyVolume);
                return 0;
            }
            switch (wp) {
                case voice::Ready: panel::UpdateVoice(true, L"Включены · жду «Контур»"); break;
                case voice::Wake: panel::ShowListening(g_hwnd); break;
                case voice::Finished: panel::HideListening(); break;
                case voice::Speakers:
                case voice::Headphones:
                    panel::HideListening();
                    if (voice::Running() && SwitchAudio(wp == voice::Speakers ? 0 : 1) && g_voiceReply)
                        voice::Reply(g_voiceReplyText, g_voiceReplyVolume);
                    break;
                case voice::OpenMusic:
                case voice::PlayWave:
                    panel::HideListening();
                    if (voice::Running() && OpenYandexMusic(wp == voice::PlayWave) && g_voiceReply)
                        voice::Reply(g_voiceReplyText, g_voiceReplyVolume);
                    break;
                case voice::PauseMusic:
                case voice::ResumeMusic:
                case voice::NextTrack:
                case voice::PreviousTrack:
                case voice::LikeTrack:
                case voice::RemoveLike:
                case voice::CurrentTrack:
                    panel::HideListening();
                    if (voice::Running()) MusicAction(static_cast<voice::Event>(wp));
                    break;
                case voice::DiscordQuieter:
                case voice::DiscordLouder:
                case voice::MusicQuieter:
                case voice::MusicLouder: {
                    panel::HideListening();
                    if (!voice::Running()) break;
                    const bool music = wp == voice::MusicQuieter || wp == voice::MusicLouder;
                    const bool louder = wp == voice::MusicLouder || wp == voice::DiscordLouder;
                    if (music) {
                        MusicVolumeAction(louder ? 10 : -10, true, static_cast<voice::Event>(wp));
                        break;
                    }
                    if (audio::AppVolume(false, louder ? 10 : -10, true))
                        MusicReply(louder ? L"Сделала громче" : L"Сделала тише");
                    else MusicReply(music ? L"В микшере Windows нет Яндекс Музыки" : L"В микшере Windows нет Discord");
                    break;
                }
                case voice::Unrecognized:
                    MusicReply(L"Не разобрала команду. Повтори, пожалуйста");
                    break;
                case voice::CancelTimer:
                    panel::HideListening();
                    if (voice::Running()) {
                        if (g_musicDeadline) {
                            CancelMusicTimer();
                            MusicReply(L"Таймер музыки отменён");
                        } else MusicReply(L"Таймер музыки выключен");
                    }
                    break;
                case voice::ReplyError:
                    AudioToast(L"Не удалось озвучить ответ");
                    break;
                case voice::MissingFiles:
                case voice::MicrophoneError:
                case voice::EngineError:
                    SaveBoolSetting(L"VoiceEnabled", false);
                    panel::HideListening();
                    panel::UpdateVoice(false, wp == voice::MicrophoneError ? L"Микрофон недоступен" :
                                             wp == voice::MissingFiles ? L"Голосовой пакет не найден" :
                                                                        L"Ошибка распознавания");
                    AudioToast(wp == voice::MissingFiles ? L"Папка voice должна лежать рядом с Kontur.exe" :
                               wp == voice::MicrophoneError ? L"Не удалось открыть микрофон Windows" :
                                                             L"Не удалось загрузить распознавание речи");
                    break;
            }
            return 0;
        case WM_WIDGET_CLICK:
            OpenHistoryOrMenu((UINT)wp);
            return 0;
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) RefreshSoon(2500);
            return TRUE;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) { RefreshSoon(5000); CheckMusicTimer(); }
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_DEBOUNCE) { KillTimer(h, TIMER_DEBOUNCE); battery_monitor::Refresh(); }
            if (wp == TIMER_WIDGET) Reconcile();
            if (wp == TIMER_INSTALL_UPDATE) {
                KillTimer(h, TIMER_INSTALL_UPDATE);
                if (g_updateAvailable && !g_updateBusy) StartUpdate(InstallUpdate);
            }
            if (wp == TIMER_MUSIC) {
                CheckMusicTimer();
                if (g_musicDeadline) {
                    const auto now = GetTickCount64();
                    SetTimer(h, TIMER_MUSIC, UINT(g_musicDeadline > now ? g_musicDeadline - now : 1), nullptr);
                }
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
                bool light = ReadLightTaskbar();
                if (light != g_lightTaskbar) {
                    g_lightTaskbar = light;
                    ApplyMenuTheme(light);
                    RedrawAll();
                    UpdateAudioTray();
                    widget::SetLightTheme(light);
                }
            }
            return 0;
        case WM_DESTROY:
            panel::HideListening();
            voice::Stop();
            audio::Stop();
            if (g_audioAdded) TrayDelete(AUDIO_ID);
            if (g_audioIcon) DestroyIcon(g_audioIcon);
            KillTimer(h, TIMER_WIDGET);
            widget::Destroy();
            battery_monitor::Stop();
            for (auto& [k, e] : g_entries) {
                if (e.added) TrayDelete(e.uid);
                if (e.icon) DestroyIcon(e.icon);
            }
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `Kontur.exe --list` prints what it finds, `--dump` every HID collection.
int ListToConsole(bool dump) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [&](const std::wstring& s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    if (dump) {
        write(DumpHidCollections());
        return 0;
    }
    auto list = PollAllDevices();
    if (list.empty()) write(L"No devices with battery info found.\n");
    for (auto& s : list) {
        std::wstring line = s.name + L"  [" + s.via + L"]  ";
        if (!s.connected) line += L"off / not connected";
        else if (s.asleep) line += L"asleep";
        else if (!s.levelText.empty()) line += s.levelText;
        else if (s.percent >= 0) line += std::to_wstring(s.percent) + L"%";
        else line += L"unknown";
        if (!s.detail.empty()) line += L" (" + s.detail + L")";
        if (s.charging) line += L", charging";
        if (s.usbOnly) line += L", connected via USB";
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        write(line + L"   (" + s.key + L")\n");
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--list")) return ListToConsole(false);
    if (cmdLine && wcsstr(cmdLine, L"--dump")) return ListToConsole(true);
    if (cmdLine && wcscmp(cmdLine, L"--enable-autostart") == 0) {
        MigrateAutostart();
        SetAutostart(true);
        return AutostartEnabled() ? 0 : 1;
    }
    if (cmdLine && wcsstr(cmdLine, L"--quit")) {  // used by the installer before upgrading/uninstalling
        if (HWND w = RunningHost()) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            for (int i = 0; i < 50 && RunningHost(); i++) Sleep(100);
        }
        return 0;
    }
    g_inst = inst;

    const bool background = cmdLine && wcsstr(cmdLine, L"--background");
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Kontur");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!background) {
            if (HWND existing = FindWindowW(kHostWindow, nullptr))
                PostMessageW(existing, WM_SHOW_PANEL, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }
    // Prevent the previous portable build from competing for the same receivers.
    HANDLE legacyGuard = CreateMutexW(nullptr, FALSE, L"Local\\WirelessBatteryTray");
    if (!legacyGuard || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (legacyGuard) CloseHandle(legacyGuard);
        CloseHandle(mutex);
        if (!background) MessageBoxW(nullptr, L"Закрой предыдущую версию через Exit в трее и снова запусти Контур.",
                                    kAppName, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    g_lightTaskbar = ReadLightTaskbar();
    ApplyMenuTheme(g_lightTaskbar);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kHostWindow;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) {
        CloseHandle(legacyGuard);
        CloseHandle(mutex);
        return 1;
    }

    DEV_BROADCAST_DEVICEINTERFACE_W filter{ sizeof(filter) };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    HDEVNOTIFY notify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_wantWidget = LoadWidgetMode();
    g_hideLastKnown = LoadHideLastKnown();
    g_widgetBackground = LoadWidgetBackground();
    widget::SetBackground(g_widgetBackground);  // remembered by the widget across re-creation
    LoadHidden();
    LoadOrder();
    MigrateAutostart();
    Reconcile();
    UpdatePlaceholder();  // something to click on until the first poll finishes
    audio::Start(g_hwnd, WM_AUDIO_CHANGED);
    UpdateAudioTray();
    voice::LoadProfiles();
    LoadVoiceReply();
    if (LoadBoolSetting(L"VoiceEnabled", false)) SetVoice(true);
    if (!background) OpenPanel();
    CheckUpdates(false);
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    battery_monitor::Start(g_hwnd, WM_RESULTS);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (panel::HandleKeyboard(m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // drain results posted after shutdown started
    while (PeekMessageW(&m, nullptr, WM_RESULTS, WM_RESULTS, PM_REMOVE)) delete reinterpret_cast<std::vector<DeviceStatus>*>(m.lParam);
    if (notify) UnregisterDeviceNotification(notify);
    CloseHandle(legacyGuard);
    CloseHandle(mutex);
    return 0;
}
