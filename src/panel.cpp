#include "panel.h"
#include "appearance.h"
#include "audio.h"
#include "history.h"
#include <shellapi.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

using namespace appearance;
constexpr wchar_t kPanel[] = L"KonturPanel";
constexpr wchar_t kToast[] = L"KonturToast";
constexpr wchar_t kSpeakers[] = L"Колонки";
constexpr wchar_t kHeadphones[] = L"Наушники";
constexpr const wchar_t* kLabels[]{kSpeakers, kHeadphones};
enum Control : UINT { Speakers = 100, Headphones, Configure, Add, Day, Week, Batteries, Extras, VoiceToggle,
                      ReplyToggle, ReplyEdit, ReplySave,
                      BatteryPage = 200, AudioPage, VoicePage };
HWND g_panel, g_owner, g_buttons[6], g_list, g_extraList, g_toast, g_settings, g_voiceToggle, g_listening;
HWND g_replyToggle, g_replyEdit, g_replySave;
HFONT g_replyFont;
constexpr COLORREF kReplyInput = RGB(30, 29, 44);
UINT g_message;
size_t g_module;
std::vector<HWND> g_navigation;
bool IsAudioPage() { return g_module == 1; }
bool IsBatteryPage() { return g_module == 0; }
bool IsVoicePage() { return g_module == 2; }
bool g_voiceEnabled;
bool g_replyEnabled = true;
std::wstring g_replyText = L"Команда выполнена";
std::wstring g_voiceStatus = L"Выключены · микрофон свободен";
bool g_oneDay = true;
HICON g_brand;
std::vector<panel::BatteryRow> g_rows;
std::vector<std::wstring> g_extraIds;

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
    return (std::max)(0, (std::min)(int(r.right) - p(274), p(1240)));
}
int ContentLeft(const RECT& r, Scale p) { return p(214) + (r.right - p(214) - ContentWidth(r, p)) / 2; }
int SectionTop(const RECT& r, Scale p) { return p(24) + (std::max)(0L, (r.bottom - p(650)) / 5); }

bool KeyboardFocus(const DRAWITEMSTRUCT& item) {
    return (item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT) &&
           !(SendMessageW(item.hwndItem, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
}

void FocusAccent(const DRAWITEMSTRUCT& item, RECT rect, Scale p) {
    if (KeyboardFocus(item))
        Rounded(item.hDC, {rect.left + p(9), rect.bottom - p(5), rect.right - p(9), rect.bottom - p(3)}, p(2), kAccent);
}

void LayoutNavigation(Scale p);

void Layout(HWND window) {
    Scale p{GetDpiForWindow(window)};
    RECT r{};
    GetClientRect(window, &r);
    const int left = ContentLeft(r, p), top = SectionTop(r, p);
    const int content = ContentWidth(r, p);
    const int column = (content - p(12)) / 2;
    MoveWindow(g_buttons[2], left + content - p(172), top + p(8), p(172), p(38), TRUE);
    MoveWindow(g_buttons[0], left, top + p(200), column, p(81), TRUE);
    const int split = left + column + p(12), rightWidth = content - column - p(12);
    MoveWindow(g_buttons[1], split, top + p(200), rightWidth, p(81), TRUE);
    LayoutNavigation(p);
    MoveWindow(g_buttons[3], left, top + p(301), (std::min)(column, p(203)), p(39), TRUE);
    MoveWindow(g_buttons[4], left + content - p(159), top + p(231), p(75), p(34), TRUE);
    MoveWindow(g_buttons[5], left + content - p(79), top + p(231), p(79), p(34), TRUE);
    MoveWindow(g_voiceToggle, left + content - p(156), top + p(96), p(134), p(38), TRUE);
    ShowWindow(g_voiceToggle, IsVoicePage() ? SW_SHOW : SW_HIDE);
    MoveWindow(g_replyToggle, left + content - p(156), top + p(186), p(134), p(34), TRUE);
    MoveWindow(g_replyEdit, left + p(34), top + p(245), content - p(199), p(21), TRUE);
    MoveWindow(g_replySave, left + content - p(139), top + p(235), p(117), p(38), TRUE);
    for (HWND control : {g_replyToggle, g_replyEdit, g_replySave})
        ShowWindow(control, IsVoicePage() ? SW_SHOW : SW_HIDE);
    SendMessageW(g_extraList, LB_SETITEMHEIGHT, 0, p(45));
    const int extraHeight = (std::max)(p(45), int(r.bottom) - top - p(318));
    MoveWindow(g_extraList, split, top + p(289), rightWidth,
               (std::min)(extraHeight, (int)g_extraIds.size() * p(45)), TRUE);
    for (int i = 0; i < 3; ++i) ShowWindow(g_buttons[i], IsAudioPage() ? SW_SHOW : SW_HIDE);
    ShowWindow(g_buttons[3], IsAudioPage() ? SW_SHOW : SW_HIDE);
    ShowWindow(g_extraList, IsAudioPage() && !g_extraIds.empty() ? SW_SHOW : SW_HIDE);
    for (int i = 4; i <= 5; ++i) ShowWindow(g_buttons[i], IsBatteryPage() && !g_rows.empty() ? SW_SHOW : SW_HIDE);
    SendMessageW(g_list, LB_SETITEMHEIGHT, 0, p(50));
    MoveWindow(g_list, left, top + p(66), content, p(150), TRUE);
    ShowWindow(g_list, IsBatteryPage() && !g_rows.empty() ? SW_SHOW : SW_HIDE);
}

struct ContentView {
    Scale p;
    long left, right, top, bottom;
};

void PaintAudioView(HWND, HDC dc, const ContentView& view) {
    const auto [p, left, right, top, bottom] = view;
    Font main(p(19), FW_SEMIBOLD, L"Inter");
    Font small(p(13), FW_NORMAL, L"Inter");
    const auto* output = audio::Find(audio::Current());
    Rounded(dc, {left, top + p(66), right, top + p(175)}, p(16), kSurface);
    Text(dc, {left + p(22), top + p(83), right - p(20), top + p(110)}, L"Активный аудиовыход", kQuiet, small.handle);
    const RECT dot{left + p(22), top + p(127), left + p(30), top + p(135)};
    Rounded(dc, dot, p(8), output ? kAccent : kQuiet);
    Text(dc, {left + p(42), top + p(111), right - p(20), top + p(151)},
         output ? output->Label() : L"Нет активного выхода", kInk, main.handle);
}

void PaintBatteryView(HWND window, HDC dc, const ContentView& view) {
    const auto [p, left, right, top, bottom] = view;
    Font main(p(19), FW_SEMIBOLD, L"Inter");
    Font small(p(13), FW_NORMAL, L"Inter");
    if (g_rows.empty()) {
        Text(dc, {left, p(96), right, p(146)}, L"Ожидаем данные от устройств", kQuiet, small.handle);
        return;
    }
    const auto index = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (index < 0 || index >= (int)g_rows.size()) return;
    Text(dc, {left, top + p(229), right - p(170), top + p(267)}, L"История заряда", kInk, main.handle);
    history::Paint(window, dc, {left, top + p(280), right, bottom - p(30)},
                   g_rows[index].device, g_oneDay);
}

void PaintVoiceView(HWND, HDC dc, const ContentView& view) {
    const auto [p, left, right, top, bottom] = view;
    Font title(p(19), FW_SEMIBOLD, L"Inter");
    Font label(p(15), FW_SEMIBOLD, L"Inter");
    Font body(p(14), FW_NORMAL, L"Inter");
    const long x = left + p(22), edge = right - p(22);
    Rounded(dc, {left, top + p(66), right, top + p(161)}, p(16), kSurface);
    Text(dc, {x, top + p(81), edge - p(145), top + p(111)}, L"Голосовые команды", kInk, title.handle);
    Text(dc, {x, top + p(114), edge - p(145), top + p(144)}, g_voiceStatus,
         g_voiceEnabled ? kAccent : kQuiet, body.handle);
    Rounded(dc, {left, top + p(174), right, top + p(290)}, p(16), kSurface);
    Text(dc, {x, top + p(186), edge - p(145), top + p(220)}, L"Голосовой ответ", kInk, title.handle);
    RECT input{x, top + p(235), right - p(150), top + p(273)};
    Rounded(dc, input, p(12), GetFocus() == g_replyEdit ? kAccent : kLine);
    InflateRect(&input, -1, -1);
    Rounded(dc, input, p(11), kReplyInput);
    Text(dc, {left, top + p(303), edge, top + p(335)}, L"Команды для звука", kInk, label.handle);
    Fill(dc, {left, top + p(341), right, top + p(342)}, kLine);
    Text(dc, {left + p(4), top + p(345), edge, top + p(380)}, L"Контур, переключи звук на колонки", kInk, body.handle);
    Fill(dc, {left, top + p(382), right, top + p(383)}, kLine);
    Text(dc, {left + p(4), top + p(386), edge, top + p(421)}, L"Контур, переключи звук на наушники", kInk, body.handle);
    Fill(dc, {left, top + p(423), right, top + p(424)}, kLine);
    Text(dc, {left, top + p(437), right, top + p(462)}, L"Речь обрабатывается на этом ПК. Записи не сохраняются.", kQuiet, body.handle);
}

struct ModuleView {
    UINT command;
    const wchar_t* title;
    const wchar_t* glyph;
    void (*paint)(HWND, HDC, const ContentView&);
};
constexpr ModuleView kModules[]{
    {BatteryPage, L"Зарядка", L"\uE83F", PaintBatteryView},
    {AudioPage, L"Переключение", L"\uE767", PaintAudioView},
    {VoicePage, L"Голос", L"\uE720", PaintVoiceView},
};

void LayoutNavigation(Scale p) {
    for (size_t i = 0; i < g_navigation.size(); ++i)
        MoveWindow(g_navigation[i], p(14), p(102 + (int)i * 49), p(186), p(45), TRUE);
}

void PaintPanel(HWND window, HDC dc) {
    Scale p{GetDpiForWindow(window)};
    RECT r{};
    GetClientRect(window, &r);
    const long left = ContentLeft(r, p), right = left + ContentWidth(r, p), top = SectionTop(r, p);
    Fill(dc, r, kBackground);
    Fill(dc, {0, 0, p(214), r.bottom}, kSidebar);
    Fill(dc, {p(213), 0, p(214), r.bottom}, kLine);
    if (g_brand) DrawIconEx(dc, p(22), p(32), g_brand, p(31), p(31), 0, nullptr, DI_NORMAL);
    Font brand(p(21), FW_SEMIBOLD, L"Inter");
    Font title(p(31), FW_SEMIBOLD, L"Inter");
    Text(dc, {p(64), p(26), p(175), p(69)}, L"Контур", kInk, brand.handle);
    const auto& module = kModules[g_module];
    Text(dc, {left, top, right - (IsAudioPage() ? p(182) : 0), top + p(55)},
         module.title, kInk, title.handle);

    module.paint(window, dc, {p, left, right, top, r.bottom});
}

void DrawButton(const DRAWITEMSTRUCT& item) {
    Scale p{GetDpiForWindow(item.hwndItem)};
    RECT r = item.rcItem;
    Font label(p(15), FW_SEMIBOLD, L"Inter");
    Font sub(p(12), FW_NORMAL, L"Inter");
    const auto navigation = std::find_if(std::begin(kModules), std::end(kModules), [&](const ModuleView& module) {
        return module.command == item.CtlID;
    });
    if (navigation != std::end(kModules)) {
        const auto& module = *navigation;
        const bool selected = module.command == kModules[g_module].command;
        Fill(item.hDC, r, kSidebar);
        if (selected || (item.itemState & ODS_SELECTED)) Rounded(item.hDC, r, p(13), kSurface);
        if (selected) Rounded(item.hDC, {r.left, r.top + p(9), r.left + p(3), r.bottom - p(9)}, p(3), kAccent);
        Font glyph(p(17), FW_NORMAL, L"Segoe MDL2 Assets");
        Text(item.hDC, {r.left + p(13), r.top, r.left + p(36), r.bottom},
             module.glyph, selected ? kAccent : kQuiet, glyph.handle, DT_CENTER);
        Text(item.hDC, {r.left + p(44), r.top, r.right - p(7), r.bottom},
             module.title, selected ? kInk : kQuiet, label.handle);
        FocusAccent(item, r, p);
        return;
    }
    const int slot = item.CtlID == Speakers ? 0 : 1;
    const bool output = item.CtlID == Speakers || item.CtlID == Headphones;
    const auto* endpoint = output ? audio::Find(audio::Configured(slot)) : nullptr;
    const bool unavailable = output && (!endpoint || !endpoint->available);
    const bool selected = (endpoint && endpoint->id == audio::Current()) ||
                          (item.CtlID == Day && g_oneDay) || (item.CtlID == Week && !g_oneDay);
    const bool insideCard = item.CtlID == VoiceToggle || item.CtlID == ReplyToggle || item.CtlID == ReplySave;
    Fill(item.hDC, r, insideCard ? kSurface : kBackground);
    Rounded(item.hDC, r, p(13), selected || item.CtlID == Add ? kAccent : kLine);
    InflateRect(&r, -p(1), -p(1));
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    Rounded(item.hDC, r, p(12), item.CtlID == Add ? kAccent : item.itemState & ODS_SELECTED ? kLine : kSurface);
    if (!output) {
        wchar_t caption[96]{};
        GetWindowTextW(item.hwndItem, caption, ARRAYSIZE(caption));
        Text(item.hDC, r, caption, disabled ? kQuiet : item.CtlID == Add ? kBackground : selected ? kAccent : kInk,
             item.CtlID == Day || item.CtlID == Week ? sub.handle : label.handle, DT_CENTER);
    } else {
        RECT title{r.left + p(16), r.top + p(13), r.right - p(14), r.top + p(40)};
        RECT detail{title.left, r.top + p(39), title.right, r.bottom - p(10)};
        Text(item.hDC, title, kLabels[slot], unavailable ? kQuiet : kInk, label.handle);
        const auto caption = !endpoint ? L"Выбрать устройство" :
                             unavailable ? endpoint->Label() + L" · недоступно" : endpoint->Label();
        Text(item.hDC, detail, caption, selected ? kAccent : kQuiet, sub.handle);
    }
    FocusAccent(item, r, p);
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
    FocusAccent(item, r, p);
}

HWND MakeList(HWND parent, UINT id, const wchar_t* label, HINSTANCE inst) {
    return CreateWindowW(L"LISTBOX", label, WS_CHILD | WS_TABSTOP | WS_VSCROLL |
                         LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                         0, 0, 0, 0, parent, (HMENU)(UINT_PTR)id, inst, nullptr);
}

void DrawExtra(const DRAWITEMSTRUCT& item) {
    if (item.itemID >= g_extraIds.size()) return;
    const auto& id = g_extraIds[item.itemID];
    const auto* device = audio::Find(id);
    const bool active = id == audio::Current();
    const bool available = device && device->available;
    Scale p{GetDpiForWindow(item.hwndItem)};
    const auto& r = item.rcItem;
    Fill(item.hDC, r, kBackground);
    Rounded(item.hDC, {r.left, r.top + p(2), r.right, r.bottom - p(3)}, p(11), active ? kAccent : kLine);
    Rounded(item.hDC, {r.left + 1, r.top + p(2) + 1, r.right - 1, r.bottom - p(3) - 1}, p(10), kSurface);
    Font label(p(14), FW_SEMIBOLD, L"Inter");
    const std::wstring name = device ? device->Label() : L"Устройство не найдено";
    Text(item.hDC, {r.left + p(14), r.top, r.right - p(12), r.bottom},
         device && !available ? name + L" · недоступно" : name, active ? kAccent : available ? kInk : kQuiet, label.handle);
    FocusAccent(item, r, p);
}

void SetReplyFont(HWND window) {
    HFONT previous = g_replyFont;
    const Scale p{GetDpiForWindow(window)};
    g_replyFont = CreateFontW(-p(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(g_replyEdit, WM_SETFONT, (WPARAM)g_replyFont, TRUE);
    if (previous) DeleteObject(previous);
}

LRESULT CALLBACK PanelProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(window, GWLP_HINSTANCE);
            for (const auto& module : kModules) {
                g_navigation.push_back(CreateWindowW(L"BUTTON", module.title,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window,
                    (HMENU)(UINT_PTR)module.command, inst, nullptr));
            }
            const wchar_t* labels[]{kSpeakers, kHeadphones, L"Настроить выходы", L"+ Добавить устройство", L"24 часа", L"7 дней"};
            for (UINT i = 0; i < ARRAYSIZE(g_buttons); ++i)
                g_buttons[i] = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                             0, 0, 0, 0, window, (HMENU)(UINT_PTR)(Speakers + i), inst, nullptr);
            g_list = MakeList(window, Batteries, L"Заряд устройств", inst);
            g_extraList = MakeList(window, Extras, L"Дополнительные аудиовыходы", inst);
            g_voiceToggle = CreateWindowW(L"BUTTON", g_voiceEnabled ? L"Выключить" : L"Включить",
                WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window, (HMENU)(UINT_PTR)VoiceToggle, inst, nullptr);
            g_replyToggle = CreateWindowW(L"BUTTON", g_replyEnabled ? L"Звук включён" : L"Звук выключен",
                WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window, (HMENU)(UINT_PTR)ReplyToggle, inst, nullptr);
            g_replyEdit = CreateWindowExW(0, L"EDIT", g_replyText.c_str(),
                WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, window, (HMENU)(UINT_PTR)ReplyEdit, inst, nullptr);
            SendMessageW(g_replyEdit, EM_LIMITTEXT, 120, 0);
            SendMessageW(g_replyEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            SetReplyFont(window);
            g_replySave = CreateWindowW(L"BUTTON", L"Сохранить", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, window, (HMENU)(UINT_PTR)ReplySave, inst, nullptr);
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
            SetReplyFont(window);
            Layout(window);
            return 0;
        }
        case WM_COMMAND: {
            const auto id = LOWORD(wp);
            if (id == Configure) PostMessageW(g_owner, g_message, panel::ConfigureOutput, 0);
            if (id == Add) PostMessageW(g_owner, g_message, panel::AddOutput, 0);
            if (id == VoiceToggle) PostMessageW(g_owner, g_message, panel::ToggleVoice, 0);
            if (id == ReplyToggle) PostMessageW(g_owner, g_message, panel::ToggleReply, 0);
            if (id == ReplySave) PostMessageW(g_owner, g_message, panel::SaveReply, 0);
            if (id == ReplyEdit && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS))
                InvalidateRect(window, nullptr, FALSE);
            if (id == Extras && HIWORD(wp) == LBN_SELCHANGE) {
                const auto index = SendMessageW(g_extraList, LB_GETCURSEL, 0, 0);
                if (index != LB_ERR) SendMessageW(g_owner, g_message, panel::SelectExtra, index);
            }
            if (id == Speakers || id == Headphones) PostMessageW(g_owner, g_message, panel::SelectOutput, id - Speakers);
            for (size_t i = 0; i < ARRAYSIZE(kModules); ++i) {
                if (id == kModules[i].command) {
                    g_module = i;
                    Layout(window);
                    for (HWND button : g_navigation) InvalidateRect(button, nullptr, FALSE);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
            }
            if (id == Batteries && HIWORD(wp) == LBN_SELCHANGE) InvalidateRect(window, nullptr, FALSE);
            if (id == Day || id == Week) {
                g_oneDay = id == Day;
                InvalidateRect(g_buttons[4], nullptr, FALSE);
                InvalidateRect(g_buttons[5], nullptr, FALSE);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (item.CtlType == ODT_BUTTON) DrawButton(item);
            if (item.CtlID == Batteries) DrawBattery(item);
            if (item.CtlID == Extras) DrawExtra(item);
            return TRUE;
        }
        case WM_CTLCOLOREDIT: {
            static HBRUSH replyBackground = CreateSolidBrush(kReplyInput);
            SetBkColor((HDC)wp, kReplyInput);
            SetTextColor((HDC)wp, kInk);
            return (LRESULT)replyBackground;
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
            g_extraList = nullptr;
            g_voiceToggle = nullptr;
            g_replyToggle = g_replyEdit = g_replySave = nullptr;
            if (g_replyFont) DeleteObject(g_replyFont);
            g_replyFont = nullptr;
            g_extraIds.clear();
            g_navigation.clear();
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

constexpr wchar_t kListeningClass[] = L"KonturListening";
ULONGLONG g_listeningSince;
bool g_listeningMotion;

LRESULT CALLBACK ListeningProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (!g_listeningMotion || GetTickCount64() - g_listeningSince >= 5400) DestroyWindow(window);
            else if (g_listeningMotion) InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC target = BeginPaint(window, &paint);
            RECT r{};
            GetClientRect(window, &r);
            HDC buffer = CreateCompatibleDC(target);
            HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, r.right, r.bottom) : nullptr;
            HGDIOBJ old = bitmap ? SelectObject(buffer, bitmap) : nullptr;
            HDC dc = old ? buffer : target;
            Scale p{GetDpiForWindow(window)};
            Fill(dc, r, kLine);
            RECT inside = r;
            InflateRect(&inside, -1, -1);
            Rounded(dc, inside, p(19), kSurface);
            const auto center = r.right / 2;
            const auto elapsed = GetTickCount64() - g_listeningSince;
            const double phase = g_listeningMotion ? elapsed / 180.0 : 0;
            for (int side : {-1, 1}) {
                for (int bar = 0; bar < 3; ++bar) {
                    const int x = center + side * p(25 + bar * 7);
                    const int amplitude = p(7 + (int)(5 * (1 + std::sin(phase - bar * 0.9 + side * 0.45))));
                    Rounded(dc, {x - p(2), p(29) - amplitude / 2, x + p(2), p(29) + (amplitude + 1) / 2},
                            p(4), kAccent);
                }
            }
            Font microphone(p(26), FW_NORMAL, L"Segoe MDL2 Assets");
            Font caption(p(13), FW_SEMIBOLD, L"Inter");
            Text(dc, {center - p(16), p(10), center + p(16), p(46)}, L"\uE720", kAccent,
                 microphone.handle, DT_CENTER);
            Text(dc, {0, p(47), r.right, p(68)}, L"Слушаю", kInk, caption.handle, DT_CENTER);
            if (old) {
                BitBlt(target, 0, 0, r.right, r.bottom, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, old);
            }
            if (bitmap) DeleteObject(bitmap);
            if (buffer) DeleteDC(buffer);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            g_listening = nullptr;
            return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

enum SettingControl : UINT { OutputList = 400, OutputName, SaveName, SetSpeakers, SetHeadphones, ToggleExtra, ConnectWindows };
HWND g_settingsList, g_settingsName, g_settingsButtons[5];
HFONT g_settingsFont;
bool g_adding;
std::vector<std::wstring> g_optionIds;
std::wstring g_settingsNotice;
void SettingsLayout(HWND window);

int OutputRows() { return (std::clamp)((int)g_optionIds.size(), 1, 4); }

void ResizeAddDialog(HWND window) {
    if (!g_adding || !window) return;
    Scale p{GetDpiForWindow(window)};
    RECT current{};
    GetClientRect(window, &current);
    RECT size{0, 0, current.right, p(131 + OutputRows() * 43)};
    if (current.bottom == size.bottom) return;
    AdjustWindowRectExForDpi(&size, GetWindowLongW(window, GWL_STYLE), FALSE,
                             GetWindowLongW(window, GWL_EXSTYLE), p.dpi);
    SetWindowPos(window, nullptr, 0, 0, size.right - size.left, size.bottom - size.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool IsExtra(const std::wstring& id) {
    const auto& extra = audio::ExtraOutputs();
    return std::find(extra.begin(), extra.end(), id) != extra.end();
}

std::wstring SelectedOutput() {
    const auto index = SendMessageW(g_settingsList, LB_GETCURSEL, 0, 0);
    return index >= 0 && (size_t)index < g_optionIds.size() ? g_optionIds[index] : L"";
}

void ShowSelectedOutput() {
    const auto id = SelectedOutput();
    const auto* endpoint = audio::Find(id);
    const bool primary = !id.empty() && (id == audio::Configured(0) || id == audio::Configured(1));
    SetWindowTextW(g_settingsName, endpoint ? endpoint->alias.c_str() : L"");
    SendMessageW(g_settingsName, EM_SETCUEBANNER, TRUE, (LPARAM)(endpoint ? endpoint->name.c_str() : L"Имя устройства"));
    EnableWindow(g_settingsName, !id.empty());
    EnableWindow(g_settingsButtons[0], !id.empty());
    EnableWindow(g_settingsButtons[1], endpoint && endpoint->available && id != audio::Configured(1));
    EnableWindow(g_settingsButtons[2], endpoint && endpoint->available && id != audio::Configured(0));
    EnableWindow(g_settingsButtons[3], !primary && (IsExtra(id) || (endpoint && endpoint->available)));
    SetWindowTextW(g_settingsButtons[3], IsExtra(id) ? L"Удалить из списка" : L"Добавить");
}

void PopulateOutputSettings(const std::wstring& selected = {}) {
    g_optionIds.clear();
    SendMessageW(g_settingsList, LB_RESETCONTENT, 0, 0);
    auto append = [&](const std::wstring& id) {
        if (id.empty() || std::find(g_optionIds.begin(), g_optionIds.end(), id) != g_optionIds.end()) return;
        const auto* endpoint = audio::Find(id);
        const auto name = endpoint ? endpoint->Label() : L"Устройство не найдено";
        const auto index = SendMessageW(g_settingsList, LB_ADDSTRING, 0, (LPARAM)name.c_str());
        if (index < 0) return;
        g_optionIds.push_back(id);
        if (id == selected) SendMessageW(g_settingsList, LB_SETCURSEL, index, 0);
    };
    if (!g_adding) {
        append(audio::Configured(0));
        append(audio::Configured(1));
        for (const auto& id : audio::ExtraOutputs()) append(id);
    }
    for (const auto& endpoint : audio::Endpoints()) {
        if (!endpoint.available) continue;
        const auto& id = endpoint.id;
        if (!g_adding || (id != audio::Configured(0) && id != audio::Configured(1) && !IsExtra(id))) append(id);
    }
    if (!g_optionIds.empty() && SendMessageW(g_settingsList, LB_GETCURSEL, 0, 0) == LB_ERR)
        SendMessageW(g_settingsList, LB_SETCURSEL, 0, 0);
    ShowWindow(g_settingsList, g_optionIds.empty() ? SW_HIDE : SW_SHOW);
    ShowSelectedOutput();
    HWND window = GetParent(g_settingsList);
    ResizeAddDialog(window);
    SettingsLayout(window);
    InvalidateRect(window, nullptr, FALSE);
    InvalidateRect(g_settingsList, nullptr, TRUE);
}

void SettingsLayout(HWND window) {
    Scale p{GetDpiForWindow(window)};
    RECT r{};
    GetClientRect(window, &r);
    int w = r.right - p(44);
    SendMessageW(g_settingsList, LB_SETITEMHEIGHT, 0, p(43));
    MoveWindow(g_settingsList, p(22), p(63), w, p(g_adding ? OutputRows() * 43 : 215), TRUE);
    if (g_adding) {
        const int footer = r.bottom - p(54);
        MoveWindow(g_settingsButtons[3], r.right - p(153), footer, p(131), p(36), TRUE);
        MoveWindow(g_settingsButtons[4], p(22), footer, p(208), p(36), TRUE);
        ShowWindow(g_settingsName, SW_HIDE);
        for (int i = 0; i < 3; ++i) ShowWindow(g_settingsButtons[i], SW_HIDE);
        return;
    }
    MoveWindow(g_settingsName, p(22), p(311), w - p(120), p(32), TRUE);
    MoveWindow(g_settingsButtons[0], r.right - p(131), p(307), p(109), p(39), TRUE);
    MoveWindow(g_settingsButtons[1], p(22), p(357), p(143), p(38), TRUE);
    MoveWindow(g_settingsButtons[2], p(174), p(357), p(143), p(38), TRUE);
    MoveWindow(g_settingsButtons[3], r.right - p(200), p(357), p(178), p(38), TRUE);
    MoveWindow(g_settingsButtons[4], p(22), p(410), p(208), p(34), TRUE);
    ShowWindow(g_settingsName, SW_SHOW);
    for (int i = 0; i < 3; ++i) ShowWindow(g_settingsButtons[i], SW_SHOW);
}

void PaintSettingsItem(const DRAWITEMSTRUCT& item) {
    if (item.itemID >= g_optionIds.size()) return;
    const auto& id = g_optionIds[item.itemID];
    const auto* endpoint = audio::Find(id);
    const auto& r = item.rcItem;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    Scale p{GetDpiForWindow(item.hwndItem)};
    Fill(item.hDC, r, kBackground);
    Rounded(item.hDC, {r.left, r.top + 1, r.right, r.bottom - p(3)}, p(10), selected ? kLine : kSurface);
    Font label(p(14), FW_SEMIBOLD, L"Inter");
    Font badge(p(12), FW_NORMAL, L"Inter");
    const auto status = id == audio::Configured(0) ? L"Колонки" : id == audio::Configured(1) ? L"Наушники" :
                        IsExtra(id) ? L"Добавлено" : L"";
    const std::wstring name = endpoint ? endpoint->Label() : L"Устройство не найдено";
    Text(item.hDC, {r.left + p(13), r.top, r.right - p(100), r.bottom}, name,
         endpoint && endpoint->available ? kInk : kQuiet, label.handle);
    Text(item.hDC, {r.right - p(96), r.top, r.right - p(14), r.bottom}, status, kAccent, badge.handle, DT_RIGHT);
    FocusAccent(item, r, p);
}

LRESULT CALLBACK SettingsProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(window, GWLP_HINSTANCE);
            g_settingsList = MakeList(window, OutputList, L"Доступные аудиовыходы", inst);
            ShowWindow(g_settingsList, SW_SHOW);
            g_settingsName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                            0, 0, 0, 0, window, (HMENU)(UINT_PTR)OutputName, inst, nullptr);
            g_settingsFont = CreateFontW(-MulDiv(15, GetDpiForWindow(window), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                         DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            SendMessageW(g_settingsName, WM_SETFONT, (WPARAM)g_settingsFont, 0);
            SendMessageW(g_settingsName, EM_SETLIMITTEXT, 127, 0);
            const wchar_t* labels[]{L"Сохранить", L"В колонки", L"В наушники", L"Добавить", L"Подключить в Windows"};
            for (UINT i = 0; i < ARRAYSIZE(g_settingsButtons); ++i)
                g_settingsButtons[i] = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    0, 0, 0, 0, window, (HMENU)(UINT_PTR)(SaveName + i), inst, nullptr);
            SettingsLayout(window);
            PopulateOutputSettings();
            return 0;
        }
        case WM_SIZE:
            SettingsLayout(window);
            return 0;
        case WM_COMMAND: {
            const auto id = LOWORD(wp);
            if (id == OutputList) {
                if (HIWORD(wp) == LBN_SELCHANGE) ShowSelectedOutput();
                if (HIWORD(wp) == LBN_DBLCLK && g_adding && !SelectedOutput().empty())
                    PostMessageW(window, WM_COMMAND, ToggleExtra, 0);
                return 0;
            }
            if (id == ConnectWindows) {
                ShellExecuteW(window, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            const auto selected = SelectedOutput();
            if (selected.empty()) return 0;
            bool ok;
            if (id == SaveName) {
                wchar_t label[128]{};
                GetWindowTextW(g_settingsName, label, ARRAYSIZE(label));
                std::wstring name(label);
                const auto first = name.find_first_not_of(L" \t");
                name = first == std::wstring::npos ? L"" : name.substr(first, name.find_last_not_of(L" \t") - first + 1);
                ok = audio::Rename(selected, name);
            } else if (id == ToggleExtra) ok = audio::SaveExtra(selected, !IsExtra(selected));
            else if (id == SetSpeakers || id == SetHeadphones) ok = audio::Assign(id == SetSpeakers ? 0 : 1, selected);
            else return 0;
            if (ok) PostMessageW(g_owner, g_message, panel::AudioSettingsChanged, 0);
            if (ok && g_adding && id == ToggleExtra) { DestroyWindow(window); return 0; }
            g_settingsNotice = ok ? L"Сохранено" : L"Не удалось сохранить";
            PopulateOutputSettings(selected);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (item.CtlID == OutputList) PaintSettingsItem(item);
            else DrawButton(item);
            return TRUE;
        }
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: {
            static HBRUSH background = CreateSolidBrush(kBackground);
            SetBkColor((HDC)wp, kBackground);
            SetTextColor((HDC)wp, kInk);
            return (LRESULT)background;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT r{};
            GetClientRect(window, &r);
            Scale p{GetDpiForWindow(window)};
            Fill(dc, r, kBackground);
            Font title(p(22), FW_SEMIBOLD, L"Inter");
            Font small(p(13), FW_NORMAL, L"Inter");
            Text(dc, {p(22), p(15), r.right - p(22), p(53)},
                 g_adding ? L"Какой выход добавить?" : L"Аудиовыходы Windows", kInk, title.handle);
            if (g_optionIds.empty())
                Text(dc, {p(28), p(64), r.right - p(22), p(108)}, L"Новых доступных выходов нет", kQuiet, small.handle);
            if (!g_adding) Text(dc, {p(22), p(281), r.right - p(22), p(307)}, L"Имя в Контуре", kQuiet, small.handle);
            if (!g_adding)
                Text(dc, {p(242), p(410), r.right - p(22), p(444)}, g_settingsNotice, kQuiet, small.handle, DT_RIGHT);
            else if (!g_settingsNotice.empty()) {
                Text(dc, {p(22), r.bottom - p(68), r.right - p(22), r.bottom - p(54)},
                     g_settingsNotice, kQuiet, small.handle);
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            g_settings = nullptr;
            DeleteObject(g_settingsFont);
            g_settingsNotice.clear();
            return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
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
        RECT size{0, 0, p(960), p(650)};
        AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, dpi);
        g_panel = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, kPanel, L"Контур", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
                                  owner, nullptr, inst, nullptr);
        if (!g_panel) return;
        appearance::DarkCaption(g_panel);
    }
    ShowWindow(g_panel, SW_RESTORE);
    SetForegroundWindow(g_panel);
    UpdateAudio();
}

void ShowOutputSettings(bool adding) {
    if (!g_panel) return;
    if (g_settings && adding != g_adding) DestroyWindow(g_settings);
    if (!g_settings) {
        g_adding = adding;
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(g_panel, GWLP_HINSTANCE);
        constexpr wchar_t cls[] = L"KonturSoundSettings";
        Register(inst, cls, SettingsProc, g_brand);
        Scale p{GetDpiForWindow(g_panel)};
        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        RECT size{0, 0, p(adding ? 560 : 600), p(adding ? 174 : 460)}, owner{};
        AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_CONTROLPARENT, p.dpi);
        GetWindowRect(g_panel, &owner);
        const int w = size.right - size.left, h = size.bottom - size.top;
        g_settings = CreateWindowExW(WS_EX_CONTROLPARENT, cls, adding ? L"Добавить устройство" : L"Настроить выходы", style,
                                     (owner.left + owner.right - w) / 2, (owner.top + owner.bottom - h) / 2,
                                     w, h, g_panel, nullptr, inst, nullptr);
        if (!g_settings) return;
        RECT actual{};
        GetWindowRect(g_settings, &actual);
        SetWindowPos(g_settings, nullptr, (owner.left + owner.right - (actual.right - actual.left)) / 2,
                     (owner.top + owner.bottom - (actual.bottom - actual.top)) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        DarkCaption(g_settings);
    } else PopulateOutputSettings(SelectedOutput());
    ShowWindow(g_settings, SW_SHOW);
    SetForegroundWindow(g_settings);
    SetFocus(g_settingsList);
}

void UpdateAudio() {
    if (!g_panel) return;
    const auto top = SendMessageW(g_extraList, LB_GETTOPINDEX, 0, 0);
    SendMessageW(g_extraList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_extraList, LB_RESETCONTENT, 0, 0);
    g_extraIds.clear();
    for (const auto& id : audio::ExtraOutputs()) {
        if (id == audio::Configured(0) || id == audio::Configured(1)) continue;
        const auto* device = audio::Find(id);
        const auto label = device ? device->Label() : L"Устройство не найдено";
        const auto index = SendMessageW(g_extraList, LB_ADDSTRING, 0, (LPARAM)label.c_str());
        g_extraIds.push_back(id);
        if (id == audio::Current()) SendMessageW(g_extraList, LB_SETCURSEL, index, 0);
    }
    if (top >= 0 && (size_t)top < g_extraIds.size()) SendMessageW(g_extraList, LB_SETTOPINDEX, top, 0);
    SendMessageW(g_extraList, WM_SETREDRAW, TRUE, 0);
    Layout(g_panel);
    InvalidateRect(g_panel, nullptr, FALSE);
    for (HWND button : g_buttons) InvalidateRect(button, nullptr, FALSE);
    if (g_settings && g_adding) PopulateOutputSettings(SelectedOutput());
}

void UpdateVoice(bool enabled, const std::wstring& status) {
    g_voiceEnabled = enabled;
    g_voiceStatus = status;
    if (!g_panel) return;
    SetWindowTextW(g_voiceToggle, enabled ? L"Выключить" : L"Включить");
    InvalidateRect(g_voiceToggle, nullptr, FALSE);
    if (IsVoicePage()) InvalidateRect(g_panel, nullptr, FALSE);
}

void UpdateReply(bool enabled, const std::wstring& text) {
    const bool changed = text != g_replyText;
    g_replyEnabled = enabled;
    g_replyText = text;
    if (!g_panel) return;
    SetWindowTextW(g_replyToggle, enabled ? L"Звук включён" : L"Звук выключен");
    InvalidateRect(g_replyToggle, nullptr, FALSE);
    if (changed) SetWindowTextW(g_replyEdit, text.c_str());
}

std::wstring ReplyText() {
    wchar_t text[121]{};
    GetWindowTextW(g_replyEdit, text, ARRAYSIZE(text));
    return text;
}

void HideListening() {
    if (g_listening) DestroyWindow(g_listening);
}

void ShowListening(HWND owner) {
    HideListening();
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    Register(inst, kListeningClass, ListeningProc);
    HWND foreground = GetForegroundWindow();
    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO screen{sizeof(screen)};
    if (!GetMonitorInfoW(monitor, &screen)) return;
    const auto& area = screen.rcWork;
    const UINT dpi = foreground ? GetDpiForWindow(foreground) : GetDpiForSystem();
    const Scale p{dpi ? dpi : 96};
    const int width = p(150), height = p(76);
    const int left = area.left + (area.right - area.left - width) / 2;
    const int top = area.top + (area.bottom - area.top) * 58 / 100 - height / 2;
    g_listening = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST |
                                  WS_EX_TOOLWINDOW, kListeningClass, L"Контур слушает команду", WS_POPUP,
                                  left, top, width, height, owner, nullptr, inst, nullptr);
    if (!g_listening) return;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, p(20), p(20));
    if (region && !SetWindowRgn(g_listening, region, FALSE)) DeleteObject(region);
    SetLayeredWindowAttributes(g_listening, 0, 249, LWA_ALPHA);
    BOOL motion = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motion, 0);
    g_listeningMotion = motion != FALSE;
    g_listeningSince = GetTickCount64();
    ShowWindow(g_listening, SW_SHOWNOACTIVATE);
    SetTimer(g_listening, 1, g_listeningMotion ? 38 : 5400, nullptr);
}

std::wstring ExtraId(size_t index) { return index < g_extraIds.size() ? g_extraIds[index] : L""; }

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
    ShowWindow(g_list, IsBatteryPage() && !g_rows.empty() ? SW_SHOW : SW_HIDE);
    for (int i = 4; i <= 5; ++i) ShowWindow(g_buttons[i], IsBatteryPage() && !g_rows.empty() ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_list, nullptr, TRUE);
    InvalidateRect(g_panel, nullptr, FALSE);
}

bool HandleKeyboard(MSG& message) {
    if (g_settings && (message.hwnd == g_settings || IsChild(g_settings, message.hwnd))) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            DestroyWindow(g_settings);
            return true;
        }
        return IsDialogMessageW(g_settings, &message);
    }
    return g_panel && IsDialogMessageW(g_panel, &message);
}

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
