#include "panel.h"
#include "appearance.h"
#include "audio.h"
#include "history.h"

#include <algorithm>
#include <utility>

namespace {

using namespace appearance;
constexpr wchar_t kPanel[] = L"KonturPanel";
constexpr wchar_t kToast[] = L"KonturToast";
constexpr wchar_t kSpeakers[] = L"Колонки";
constexpr wchar_t kHeadphones[] = L"Наушники";
constexpr const wchar_t* kLabels[]{kSpeakers, kHeadphones};
enum Control : UINT { Speakers = 100, Headphones, Configure, BatteryPage, AudioPage, Add, Extras, Day, Week, Batteries };
HWND g_panel, g_owner, g_buttons[9], g_list, g_toast;
UINT g_message;
bool g_audioPage;
bool g_oneDay = true;
HICON g_brand;
std::vector<panel::BatteryRow> g_rows;

struct Font {
    HFONT handle;
    Font(int size, int weight, const wchar_t* family)
        : handle(CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, family)) {}
    ~Font() { DeleteObject(handle); }
};

void Fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, (HBRUSH)GetStockObject(DC_BRUSH));
}

void Rounded(HDC dc, const RECT& rect, int radius, COLORREF color) {
    const auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    SetDCBrushColor(dc, color);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

void Text(HDC dc, RECT rect, const std::wstring& text, COLORREF color, HFONT font, UINT align = DT_LEFT) {
    const auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), (int)text.size(), &rect, align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

struct Scale {
    UINT dpi;
    int operator()(int dip) const { return MulDiv(dip, dpi, 96); }
};

int ContentWidth(const RECT& r, Scale p) {
    return (std::min)(int(r.right) - p(248), p(980));
}

void Layout(HWND window) {
    Scale p{GetDpiForWindow(window)};
    RECT r{};
    GetClientRect(window, &r);
    const int left = p(218);
    const int content = ContentWidth(r, p);
    const int column = (content - p(12)) / 2;
    MoveWindow(g_buttons[2], left + content - p(172), p(32), p(172), p(38), TRUE);
    MoveWindow(g_buttons[0], left, p(225), column, p(82), TRUE);
    MoveWindow(g_buttons[1], left + column + p(12), p(225), content - column - p(12), p(82), TRUE);
    MoveWindow(g_buttons[3], p(14), p(102), p(161), p(45), TRUE);
    MoveWindow(g_buttons[4], p(14), p(151), p(161), p(45), TRUE);
    MoveWindow(g_buttons[5], left, p(331), p(203), p(39), TRUE);
    MoveWindow(g_buttons[6], left + p(215), p(331), p(190), p(39), TRUE);
    MoveWindow(g_buttons[7], left + content - p(159), p(266), p(75), p(34), TRUE);
    MoveWindow(g_buttons[8], left + content - p(79), p(266), p(79), p(34), TRUE);
    for (int i = 0; i < 3; ++i) ShowWindow(g_buttons[i], g_audioPage ? SW_SHOW : SW_HIDE);
    ShowWindow(g_buttons[5], g_audioPage ? SW_SHOW : SW_HIDE);
    ShowWindow(g_buttons[6], g_audioPage && !audio::ExtraOutputs().empty() ? SW_SHOW : SW_HIDE);
    for (int i = 7; i <= 8; ++i) ShowWindow(g_buttons[i], !g_audioPage && !g_rows.empty() ? SW_SHOW : SW_HIDE);
    SendMessageW(g_list, LB_SETITEMHEIGHT, 0, p(50));
    MoveWindow(g_list, left, p(96), content, p(150), TRUE);
    ShowWindow(g_list, !g_audioPage && !g_rows.empty() ? SW_SHOW : SW_HIDE);
}

void PaintPanel(HWND window, HDC dc) {
    Scale p{GetDpiForWindow(window)};
    RECT r{};
    GetClientRect(window, &r);
    const long left = p(218), right = left + ContentWidth(r, p);
    Fill(dc, r, kBackground);
    Fill(dc, {0, 0, p(189), r.bottom}, kSidebar);
    Fill(dc, {p(188), 0, p(189), r.bottom}, kLine);
    if (g_brand) DrawIconEx(dc, p(22), p(32), g_brand, p(31), p(31), 0, nullptr, DI_NORMAL);
    Font brand(p(21), FW_SEMIBOLD, L"Inter");
    Font title(p(31), FW_SEMIBOLD, L"Inter");
    Font main(p(19), FW_SEMIBOLD, L"Inter");
    Font small(p(13), FW_NORMAL, L"Inter");
    Text(dc, {p(64), p(26), p(175), p(69)}, L"Контур", kInk, brand.handle);
    Text(dc, {left, p(23), right - (g_audioPage ? p(185) : 0), p(79)},
         g_audioPage ? L"Переключение" : L"Зарядка", kInk, title.handle);

    if (g_audioPage) {
        const auto* output = audio::Find(audio::Current());
        Rounded(dc, {left, p(96), right, p(197)}, p(14), kSurface);
        Text(dc, {left + p(20), p(113), right - p(20), p(139)}, L"Активный аудиовыход", kQuiet, small.handle);
        const RECT dot{left + p(20), p(156), left + p(28), p(164)};
        Rounded(dc, dot, p(8), output ? kAccent : kQuiet);
        Text(dc, {left + p(40), p(141), right - p(20), p(179)},
             output ? output->name : L"Нет активного выхода", kInk, main.handle);
        Text(dc, {left, p(387), right, p(410)},
             L"Значок рядом с часами переключает доступные выходы по кругу.", kQuiet, small.handle);
        return;
    }

    if (g_rows.empty()) {
        Text(dc, {left, p(96), right, p(146)}, L"Ожидаем данные от устройств", kQuiet, small.handle);
        return;
    }
    const auto index = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (index < 0 || index >= (int)g_rows.size()) return;
    Text(dc, {left, p(265), right - p(170), p(301)}, L"История заряда", kInk, main.handle);
    history::Paint(window, dc, {left, p(312), right, (std::min)(r.bottom - p(26), (long)p(596))},
                   g_rows[index].device, g_oneDay);
}

void DrawButton(const DRAWITEMSTRUCT& item) {
    Scale p{GetDpiForWindow(item.hwndItem)};
    RECT r = item.rcItem;
    Font label(p(15), FW_SEMIBOLD, L"Inter");
    Font sub(p(12), FW_NORMAL, L"Inter");
    if (item.CtlID == BatteryPage || item.CtlID == AudioPage) {
        const bool sound = item.CtlID == AudioPage;
        const bool selected = g_audioPage == sound;
        Fill(item.hDC, r, kSidebar);
        if (selected || (item.itemState & ODS_SELECTED)) Rounded(item.hDC, r, p(13), kSurface);
        if (selected) Rounded(item.hDC, {r.left, r.top + p(9), r.left + p(3), r.bottom - p(9)}, p(3), kAccent);
        Font glyph(p(17), FW_NORMAL, L"Segoe MDL2 Assets");
        Text(item.hDC, {r.left + p(13), r.top, r.left + p(36), r.bottom},
             sound ? L"\uE767" : L"\uE83F", selected ? kAccent : kQuiet, glyph.handle, DT_CENTER);
        Text(item.hDC, {r.left + p(44), r.top, r.right - p(7), r.bottom},
             sound ? L"Переключение" : L"Зарядка", selected ? kInk : kQuiet, label.handle);
        if (item.itemState & ODS_FOCUS) { InflateRect(&r, -p(3), -p(3)); DrawFocusRect(item.hDC, &r); }
        return;
    }
    const int slot = item.CtlID == Speakers ? 0 : 1;
    const bool output = item.CtlID == Speakers || item.CtlID == Headphones;
    const auto* endpoint = output ? audio::Find(audio::Configured(slot)) : nullptr;
    const bool unavailable = output && (!endpoint || !endpoint->available);
    const bool selected = (endpoint && endpoint->id == audio::Current()) ||
                          (item.CtlID == Day && g_oneDay) || (item.CtlID == Week && !g_oneDay);
    Fill(item.hDC, r, kBackground);
    Rounded(item.hDC, r, p(13), selected || item.CtlID == Add ? kAccent : kLine);
    InflateRect(&r, -p(1), -p(1));
    Rounded(item.hDC, r, p(12), item.CtlID == Add ? kAccent : item.itemState & ODS_SELECTED ? kLine : kSurface);
    if (!output) {
        wchar_t caption[96]{};
        GetWindowTextW(item.hwndItem, caption, ARRAYSIZE(caption));
        Text(item.hDC, r, caption, item.CtlID == Add ? kBackground : selected ? kAccent : kInk,
             item.CtlID == Day || item.CtlID == Week ? sub.handle : label.handle, DT_CENTER);
    } else {
        RECT title{r.left + p(16), r.top + p(13), r.right - p(14), r.top + p(40)};
        RECT detail{title.left, r.top + p(39), title.right, r.bottom - p(10)};
        Text(item.hDC, title, kLabels[slot], unavailable ? kQuiet : kInk, label.handle);
        const auto caption = !endpoint ? L"Выбрать устройство" :
                             unavailable ? L"Устройство недоступно" : endpoint->name;
        Text(item.hDC, detail, caption, selected ? kAccent : kQuiet, sub.handle);
    }
    if (item.itemState & ODS_FOCUS) {
        InflateRect(&r, -p(3), -p(3));
        DrawFocusRect(item.hDC, &r);
    }
}

void DrawBattery(const DRAWITEMSTRUCT& item) {
    if (item.itemID >= g_rows.size()) return;
    const auto& row = g_rows[item.itemID];
    Scale p{GetDpiForWindow(g_list)};
    const auto r = item.rcItem;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    Fill(item.hDC, r, selected ? kSurface : kBackground);
    Fill(item.hDC, {r.left, r.bottom - p(1), r.right, r.bottom}, kLine);
    Font main(p(14), FW_SEMIBOLD, L"Inter");
    Font small(p(12), FW_NORMAL, L"Inter");
    Font number(p(19), FW_NORMAL, L"Consolas");
    const int meter = r.right - p(178);
    if (selected) Fill(item.hDC, {r.left, r.top + p(9), r.left + p(2), r.bottom - p(9)}, kAccent);
    Text(item.hDC, {r.left + p(13), r.top + p(4), meter - p(12), r.top + p(26)}, row.device.name, kInk, main.handle);
    Text(item.hDC, {r.left + p(13), r.top + p(24), meter - p(12), r.bottom - p(5)}, row.detail, kQuiet, small.handle);
    COLORREF color = row.low ? kAmber : row.charging ? kAccent : kInk;
    RECT bar{meter, r.top + p(24), r.right - p(67), r.top + p(29)};
    Rounded(item.hDC, bar, p(5), kLine);
    if (row.percent >= 0) {
        bar.right = bar.left + (bar.right - bar.left) * row.percent / 100;
        Rounded(item.hDC, bar, p(5), row.low ? kAmber : kAccent);
    }
    Text(item.hDC, {r.right - p(62), r.top, r.right - p(10), r.bottom}, row.value, color, number.handle, DT_RIGHT);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = r;
        InflateRect(&focus, -p(1), -p(1));
        DrawFocusRect(item.hDC, &focus);
    }
}

LRESULT CALLBACK PanelProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(window, GWLP_HINSTANCE);
            const wchar_t* labels[]{kSpeakers, kHeadphones, L"Настроить выходы", L"Зарядка", L"Переключение",
                                    L"+ Добавить устройство", L"Другие выходы ▾", L"24 часа", L"7 дней"};
            for (UINT i = 0; i < ARRAYSIZE(g_buttons); ++i)
                g_buttons[i] = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_TABSTOP | BS_OWNERDRAW |
                                             (i == 3 || i == 4 ? WS_VISIBLE : 0),
                                             0, 0, 0, 0, window, (HMENU)(UINT_PTR)(Speakers + i), inst, nullptr);
            g_list = CreateWindowW(L"LISTBOX", L"Заряд устройств", WS_CHILD | WS_TABSTOP | WS_VSCROLL |
                                   LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                   0, 0, 0, 0, window, (HMENU)(UINT_PTR)Batteries, inst, nullptr);
            Layout(window);
            return 0;
        }
        case WM_SIZE:
            Layout(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            Scale p{GetDpiForWindow(window)};
            auto& info = *reinterpret_cast<MINMAXINFO*>(lp);
            info.ptMinTrackSize = {p(710), p(540)};
            return 0;
        }
        case WM_DPICHANGED: {
            const auto& r = *reinterpret_cast<RECT*>(lp);
            SetWindowPos(window, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(window);
            return 0;
        }
        case WM_COMMAND: {
            const auto id = LOWORD(wp);
            if (id == Configure) PostMessageW(g_owner, g_message, panel::ConfigureOutput, 0);
            if (id == Add || id == Extras) PostMessageW(g_owner, g_message,
                                                        id == Add ? panel::AddOutput : panel::ShowExtras, 0);
            if (id == Speakers || id == Headphones) PostMessageW(g_owner, g_message, panel::SelectOutput, id - Speakers);
            if (id == BatteryPage || id == AudioPage) {
                g_audioPage = id == AudioPage;
                Layout(window);
                InvalidateRect(g_buttons[3], nullptr, FALSE);
                InvalidateRect(g_buttons[4], nullptr, FALSE);
            }
            if (id == BatteryPage || id == AudioPage || (id == Batteries && HIWORD(wp) == LBN_SELCHANGE))
                InvalidateRect(window, nullptr, FALSE);
            if (id == Day || id == Week) {
                g_oneDay = id == Day;
                InvalidateRect(g_buttons[7], nullptr, FALSE);
                InvalidateRect(g_buttons[8], nullptr, FALSE);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (item.CtlType == ODT_BUTTON) DrawButton(item);
            if (item.CtlType == ODT_LISTBOX) DrawBattery(item);
            return TRUE;
        }
        case WM_CTLCOLORLISTBOX: {
            static HBRUSH brush = CreateSolidBrush(kBackground);
            SetBkColor((HDC)wp, kBackground);
            SetTextColor((HDC)wp, kInk);
            return (LRESULT)brush;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintPanel(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            g_panel = nullptr;
            g_list = nullptr;
            g_rows.clear();
            return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

LRESULT CALLBACK ToastProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER) {
        DestroyWindow(window);
        return 0;
    }
    if (msg == WM_DESTROY) g_toast = nullptr;
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        Scale p{GetDpiForWindow(window)};
        RECT r{};
        GetClientRect(window, &r);
        Fill(dc, r, kLine);
        InflateRect(&r, -p(1), -p(1));
        Fill(dc, r, kSurface);
        Fill(dc, {r.left, r.top, r.left + p(3), r.bottom}, kAccent);
        Font font(p(14), FW_SEMIBOLD, L"Inter");
        wchar_t text[512]{};
        GetWindowTextW(window, text, ARRAYSIZE(text));
        InflateRect(&r, -p(15), 0);
        Text(dc, r, text, kInk, font.handle);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

void Register(HINSTANCE inst, const wchar_t* name, WNDPROC procedure, HICON icon = nullptr) {
    WNDCLASSW cls{};
    cls.hInstance = inst;
    cls.lpszClassName = name;
    cls.lpfnWndProc = procedure;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = icon;
    RegisterClassW(&cls);
}

}  // namespace

namespace panel {

bool IsOpen() { return g_panel != nullptr; }

void Show(HWND owner, UINT message) {
    if (!g_panel) {
        g_owner = owner;
        g_message = message;
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
        g_brand = LoadIconW(inst, MAKEINTRESOURCEW(1));
        Register(inst, kPanel, PanelProc, g_brand);
        const UINT dpi = GetDpiForSystem();
        Scale p{dpi};
        constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        RECT size{0, 0, p(850), p(572)};
        AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, dpi);
        g_panel = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, kPanel, L"Контур", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
                                  owner, nullptr, inst, nullptr);
        if (!g_panel) return;
        appearance::DarkCaption(g_panel);
    }
    ShowWindow(g_panel, SW_RESTORE);
    SetForegroundWindow(g_panel);
}

void UpdateAudio() {
    if (!g_panel) return;
    Layout(g_panel);
    InvalidateRect(g_panel, nullptr, FALSE);
    for (HWND button : g_buttons) InvalidateRect(button, nullptr, FALSE);
}

void SetBatteries(std::vector<BatteryRow> rows) {
    if (!g_list) return;
    const int selected = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    const std::wstring key = selected >= 0 && selected < (int)g_rows.size() ? g_rows[selected].device.key : L"";
    const int top = (int)SendMessageW(g_list, LB_GETTOPINDEX, 0, 0);
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    g_rows = std::move(rows);
    size_t newSelected = 0;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const auto& row = g_rows[i];
        const std::wstring accessible = row.device.name + L", " + row.value + L", " + row.detail;
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)accessible.c_str());
        if (row.device.key == key) newSelected = i;
    }
    if (!g_rows.empty()) SendMessageW(g_list, LB_SETCURSEL, newSelected, 0);
    if (top >= 0 && top < (int)g_rows.size()) SendMessageW(g_list, LB_SETTOPINDEX, top, 0);
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    ShowWindow(g_list, g_audioPage || g_rows.empty() ? SW_HIDE : SW_SHOW);
    for (int i = 7; i <= 8; ++i) ShowWindow(g_buttons[i], !g_audioPage && !g_rows.empty() ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_list, nullptr, TRUE);
    InvalidateRect(g_panel, nullptr, FALSE);
}

bool HandleKeyboard(MSG& message) { return g_panel && IsDialogMessageW(g_panel, &message); }

void Toast(HWND owner, const RECT& anchor, const std::wstring& text) {
    if (g_toast) DestroyWindow(g_toast);
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    Register(inst, kToast, ToastProc);
    MONITORINFO screen{sizeof(screen)};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTOPRIMARY), &screen);
    Scale p{GetDpiForSystem()};
    const int width = p(382), height = p(44);
    const int x = screen.rcWork.right - width - p(14);
    const int y = screen.rcWork.bottom - height - p(14);
    g_toast = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                              kToast, text.c_str(), WS_POPUP, x, y, width, height,
                              owner, nullptr, inst, nullptr);
    if (g_toast) {
        ShowWindow(g_toast, SW_SHOWNOACTIVATE);
        SetTimer(g_toast, 1, 1550, nullptr);
    }
}

}  // namespace panel
