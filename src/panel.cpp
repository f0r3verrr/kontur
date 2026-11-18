#include "panel.h"
#include "appearance.h"
#include "audio.h"
#include "voice.h"
#include "history.h"
#include <shellapi.h>
#include <commctrl.h>
#include <gdiplus.h>

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
void ShowScenarios(HWND owner);
void ShowCommandCatalog(HWND owner);
HWND g_scenarios, g_commandCatalog, g_catalogWindow;
constexpr UINT CatalogButton = 571;
constexpr UINT ScenarioButton = 570;
enum Control : UINT { Speakers = 100, Headphones, Configure, Add, Day, Week, Batteries, Extras, VoiceToggle,
                      ReplyToggle, ReplyEdit, ReplySave,
                      BatteryPage = 200, AudioPage, VoicePage };
HWND g_panel, g_owner, g_buttons[6], g_list, g_extraList, g_toast, g_settings, g_voiceToggle, g_listening;
HWND g_replyToggle, g_replyEdit, g_replySave;
HWND g_volumeSlider;
int g_replyVolume = 100;
HFONT g_replyFont;
constexpr COLORREF kReplyInput = RGB(22, 26, 38);
UINT g_message;
size_t g_module;
std::vector<HWND> g_navigation;
HWND g_checkUpdates;
bool g_updateAvailable;
constexpr UINT UpdateButton = 300;
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

constexpr int kSidebarWidth = 196;
constexpr int kBatteryRowsTop = 166;
constexpr int kBatteryRowHeight = 56;
constexpr int kBatteryPlotTop = 353;
constexpr int kPeriodWidth = 69;

int RingWidth(Scale p, int content) { return content >= p(650) ? p(215) : 0; }
int PlotRight(Scale p, int left, int right) {
    const int ring = RingWidth(p, right - left);
    return right - (ring ? ring + p(14) : 0);
}

int ContentWidth(const RECT& r, Scale p) {
    return (std::max)(0, (std::min)(int(r.right) - p(kSidebarWidth + 54), p(1320)));
}
int ContentLeft(const RECT& r, Scale p) { return p(kSidebarWidth) + (r.right - p(kSidebarWidth) - ContentWidth(r, p)) / 2; }
int SectionTop(Scale p) { return p(19); }

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
    const int left = ContentLeft(r, p), top = SectionTop(p);
    const int content = ContentWidth(r, p);
    const int column = (content - p(12)) / 2;
    MoveWindow(g_buttons[2], left + content - p(172), top + p(8), p(172), p(38), TRUE);
    MoveWindow(g_buttons[0], left, top + p(150), column, p(62), TRUE);
    const int split = left + column + p(12), rightWidth = content - column - p(12);
    MoveWindow(g_buttons[1], split, top + p(150), rightWidth, p(62), TRUE);
    LayoutNavigation(p);
    MoveWindow(g_buttons[3], left, top + p(226), (std::min)(column, p(203)), p(36), TRUE);
    const int plotRight = PlotRight(p, left, left + content);
    MoveWindow(g_buttons[4], plotRight - p(156), top + p(kBatteryPlotTop + 15), p(kPeriodWidth), p(29), TRUE);
    MoveWindow(g_buttons[5], plotRight - p(84), top + p(kBatteryPlotTop + 15), p(kPeriodWidth), p(29), TRUE);
    MoveWindow(g_commandCatalog, left, top + p(262), p(142), p(34), TRUE);
    MoveWindow(g_scenarios, left + p(152), top + p(262), p(142), p(34), TRUE);
    for (HWND control : {g_commandCatalog, g_scenarios})
        ShowWindow(control, IsVoicePage() ? SW_SHOW : SW_HIDE);
    MoveWindow(g_voiceToggle, left + content - p(156), top + p(77), p(134), p(34), TRUE);
    ShowWindow(g_voiceToggle, IsVoicePage() ? SW_SHOW : SW_HIDE);
    MoveWindow(g_replyToggle, left + content - p(156), top + p(157), p(134), p(31), TRUE);
    MoveWindow(g_replyEdit, left + p(34), top + p(204), content - p(199), p(21), TRUE);
    MoveWindow(g_replySave, left + content - p(139), top + p(197), p(117), p(35), TRUE);
    for (HWND control : {g_replyToggle, g_replyEdit, g_replySave})
        ShowWindow(control, IsVoicePage() ? SW_SHOW : SW_HIDE);
    SendMessageW(g_extraList, LB_SETITEMHEIGHT, 0, p(45));
    const int extraHeight = (std::max)(p(45), int(r.bottom) - top - p(251));
    MoveWindow(g_extraList, split, top + p(220), rightWidth,
               (std::min)(extraHeight, (int)g_extraIds.size() * p(45)), TRUE);
    for (int i = 0; i < 3; ++i) ShowWindow(g_buttons[i], IsAudioPage() ? SW_SHOW : SW_HIDE);
    ShowWindow(g_buttons[3], IsAudioPage() ? SW_SHOW : SW_HIDE);
    ShowWindow(g_extraList, IsAudioPage() && !g_extraIds.empty() ? SW_SHOW : SW_HIDE);
    for (int i = 4; i <= 5; ++i) ShowWindow(g_buttons[i], IsBatteryPage() && !g_rows.empty() ? SW_SHOW : SW_HIDE);
    SendMessageW(g_list, LB_SETITEMHEIGHT, 0, p(kBatteryRowHeight));
    MoveWindow(g_list, left, top + p(kBatteryRowsTop), content, 3 * p(kBatteryRowHeight), TRUE);
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
    Rounded(dc, {left, top + p(62), right, top + p(138)}, p(14), kSurface);
    Text(dc, {left + p(18), top + p(72), right - p(18), top + p(95)}, L"Активный аудиовыход", kQuiet, small.handle);
    const RECT dot{left + p(19), top + p(110), left + p(25), top + p(116)};
    Rounded(dc, dot, p(6), output ? kAccent : kQuiet);
    Text(dc, {left + p(34), top + p(97), right - p(18), top + p(128)},
         output ? output->Label() : L"Нет активного выхода", kInk, main.handle);
}

bool GraphicsReady() {
    struct Runtime {
        ULONG_PTR token = 0;
        Runtime() {
            Gdiplus::GdiplusStartupInput input;
            Gdiplus::GdiplusStartup(&token, &input, nullptr);
        }
        ~Runtime() { if (token) Gdiplus::GdiplusShutdown(token); }
    };
    static Runtime runtime;
    return runtime.token != 0;
}

void DashboardCard(HDC dc, RECT box, Scale p, bool selected = false) {
    Rounded(dc, box, p(18), selected ? RGB(131, 96, 209) : kLine);
    InflateRect(&box, -1, -1);
    Rounded(dc, box, p(17), selected ? RGB(34, 28, 56) : kSurface);
}

void DashboardAtmosphere(HDC dc, const RECT& client, Scale p) {
    if (!GraphicsReady()) return;
    using namespace Gdiplus;
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const float right = float(client.right), split = float(p(kSidebarWidth));
    auto ribbon = [&](Rect clip, PointF start, PointF a, PointF b, PointF end, bool above) {
        const auto state = graphics.Save();
        graphics.SetClip(clip);
        GraphicsPath area, edge;
        edge.AddBezier(start, a, b, end);
        area.AddBezier(start, a, b, end);
        const float bound = above ? -1.f : float(client.bottom);
        area.AddLine(end.X, end.Y, end.X, bound);
        area.AddLine(end.X, bound, start.X, bound);
        area.CloseFigure();
        const PointF shadeStart = above ? PointF(start.X, start.Y) : PointF(0, start.Y - p(40));
        const PointF shadeEnd = above ? PointF(end.X, end.Y) : PointF(0, float(client.bottom));
        LinearGradientBrush shade(shadeStart, shadeEnd,
                                  above ? Color(3, 80, 52, 161) : Color(106, 103, 52, 214),
                                  above ? Color(112, 100, 50, 206) : Color(0, 60, 38, 126));
        graphics.FillPath(&shade, &area);
        Pen outer(Color(19, 149, 88, 250), float(p(12))), line(Color(105, 155, 101, 252), 1.f);
        graphics.DrawPath(&outer, &edge);
        graphics.DrawPath(&line, &edge);
        graphics.Restore(state);
    };
    ribbon(Rect(int(split), 0, int(right-split), p(167)),
           PointF(split+p(60),float(p(154))), PointF(right*.58f,float(p(161))),
           PointF(right*.57f,float(-p(15))), PointF(right*.81f,float(-p(9))), true);
    ribbon(Rect(0, p(218), int(split), (std::max)(1L, client.bottom-p(218))),
           PointF(-4.f,float(p(370))), PointF(split*.44f,float(p(315))),
           PointF(split*.74f,float(p(443))), PointF(split+4,float(p(398))), false);
}

struct BatteryOverview {
    int online = 0;
    int charging = 0;
    int average = -1;
    std::int64_t last = 0;
};

BatteryOverview BatteryStats() {
    BatteryOverview stats;
    int sum = 0, samples = 0;
    for (const auto& row : g_rows) {
        if (row.device.connected && !row.device.asleep) {
            ++stats.online;
            if (row.charging) ++stats.charging;
            if (row.percent >= 0 && row.percent <= 100) {
                sum += row.percent;
                ++samples;
            }
        }
        stats.last = (std::max)(stats.last, history::LastCharging(row.device.key));
    }
    if (samples) stats.average = (sum + samples / 2) / samples;
    return stats;
}

void PaintOverallCharge(HDC dc, RECT box, Scale p, int average) {
    DashboardCard(dc, box, p);
    const int centerX = (box.left + box.right) / 2;
    const int centerY = (box.top + box.bottom) / 2;
    const int diameter = (std::min)({p(150), int(box.right - box.left) - p(40), int(box.bottom - box.top) - p(36)});
    if (GraphicsReady() && diameter > p(50)) {
        using namespace Gdiplus;
        Graphics graphics(dc);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        const float x = float(centerX - diameter / 2), y = float(centerY - diameter / 2);
        const float d = float(diameter);
        Pen track(Color(255, 58, 44, 86), float(p(10)));
        graphics.DrawEllipse(&track, x, y, d, d);
        if (average > 0) {
            LinearGradientBrush light(PointF(x,y+d), PointF(x+d,y), Color(127,68,235), Color(220,194,255));
            Pen glow(Color(34, 162, 99, 255), float(p(18))), arc(&light, float(p(10)));
            arc.SetStartCap(LineCapRound);
            arc.SetEndCap(LineCapRound);
            graphics.DrawArc(&glow, x,y,d,d,-90.f,average*3.6f);
            graphics.DrawArc(&arc, x,y,d,d,-90.f,average*3.6f);
        }
    }
    Font glyph(p(20), FW_NORMAL, L"Segoe MDL2 Assets");
    Font value(p(32), FW_SEMIBOLD, L"Segoe UI");
    Font label(p(13), FW_NORMAL, L"Segoe UI");
    Text(dc, {box.left, centerY-p(51), box.right, centerY-p(25)}, L"\uE945", kAccent, glyph.handle, DT_CENTER);
    Text(dc, {box.left, centerY-p(22), box.right, centerY+p(20)},
         average >= 0 ? std::to_wstring(average)+L"%" : L"—", kInk, value.handle, DT_CENTER);
    Text(dc, {box.left, centerY+p(19), box.right, centerY+p(44)}, L"Средний заряд", kQuiet, label.handle, DT_CENTER);
}

void PaintBatteryView(HWND window, HDC dc, const ContentView& view) {
    const auto [p, left, right, top, bottom] = view;
    Font main(p(18), FW_SEMIBOLD, L"Segoe UI");
    Font metric(p(22), FW_SEMIBOLD, L"Segoe UI");
    Font label(p(12), FW_NORMAL, L"Segoe UI");
    Font glyph(p(19), FW_NORMAL, L"Segoe MDL2 Assets");
    Text(dc, {left, top+p(45), right, top+p(70)}, L"Заряд и состояние беспроводных устройств", kQuiet, label.handle);
    const auto stats = BatteryStats();
    const std::wstring values[]{
        std::to_wstring(stats.charging) + L" из " + std::to_wstring(g_rows.size()),
        stats.average >= 0 ? std::to_wstring(stats.average) + L"%" : L"—",
        stats.charging ? L"Сейчас" : history::ChargingAge(stats.last),
    };
    const wchar_t* labels[]{L"Устройства на зарядке", L"Средний заряд", L"Последняя активная зарядка"};
    const wchar_t* icons[]{L"\uE945", L"\uE9D9", L"\uE823"};
    const int width = (right-left-2*p(12))/3;
    for (int i=0; i<3; ++i) {
        const long x = left+i*(width+p(12));
        const long end = i==2 ? right : x+width;
        DashboardCard(dc, {x,top+p(83),end,top+p(151)}, p);
        const bool withIcon = width >= p(190);
        if (withIcon) {
            RECT tile{x+p(12),top+p(100),x+p(48),top+p(136)};
            Rounded(dc, tile, p(12), RGB(50,38,82));
            Text(dc, tile, icons[i], kAccent, glyph.handle, DT_CENTER);
        }
        const long tx = x + p(withIcon ? 58 : 12);
        Text(dc, {tx,top+p(90),end-p(8),top+p(123)}, values[i], kInk, metric.handle);
        const auto caption = i == 2 && !withIcon ? L"Последняя зарядка" : labels[i];
        Text(dc, {tx,top+p(122),end-p(8),top+p(142)}, caption, kQuiet, label.handle);
    }
    if (g_rows.empty()) {
        Text(dc, {left, top+p(171), right, top+p(217)}, L"Ожидаем данные от устройств", kQuiet, label.handle);
        return;
    }
    const auto index = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (index < 0 || index >= (int)g_rows.size()) return;
    const long chartRight = PlotRight(p, left, right);
    const long chartTop = top+p(kBatteryPlotTop), cardBottom = bottom-p(26);
    DashboardCard(dc, {left, chartTop, chartRight, cardBottom}, p);
    RECT tile{left+p(15),chartTop+p(17),left+p(47),chartTop+p(49)};
    Rounded(dc, tile, p(10), RGB(50,38,82));
    Text(dc, tile, icons[1], kAccent, glyph.handle, DT_CENTER);
    Text(dc, {left+p(58),chartTop+p(9),chartRight-p(161),chartTop+p(36)}, L"История заряда", kInk,main.handle);
    Text(dc, {left+p(58),chartTop+p(35),chartRight-p(161),chartTop+p(56)}, g_rows[index].device.name,kQuiet,label.handle);
    const auto charge = history::ChargeSummary(g_rows[index].device, true);
    if (!charge.empty())
        Text(dc, {left+p(58),chartTop+p(54),chartRight-p(15),chartTop+p(72)}, charge, kQuiet, label.handle);
    history::Paint(window, dc, {left+p(9),chartTop+p(charge.empty() ? 64 : 77),chartRight-p(9),cardBottom-p(9)},
                   g_rows[index].device, g_oneDay, false);
    if (chartRight != right) PaintOverallCharge(dc, {chartRight+p(14),chartTop,right,cardBottom},p,stats.average);
}

void PaintVoiceView(HWND, HDC dc, const ContentView& view) {
    const auto [p, left, right, top, bottom] = view;
    Font title(p(17), FW_SEMIBOLD, L"Inter");
    Font body(p(13), FW_NORMAL, L"Inter");
    const long x = left + p(18), edge = right - p(18);
    Rounded(dc, {left, top + p(62), right, top + p(129)}, p(14), kSurface);
    Text(dc, {x, top + p(70), edge - p(150), top + p(96)}, L"Голосовые команды", kInk, title.handle);
    Text(dc, {x, top + p(97), edge - p(150), top + p(120)}, g_voiceStatus,
         g_voiceEnabled ? kAccent : kQuiet, body.handle);
    Rounded(dc, {left, top + p(141), right, top + p(247)}, p(14), kSurface);
    Text(dc, {x, top + p(150), edge - p(150), top + p(188)}, L"Голосовой ответ", kInk, title.handle);
    RECT input{left + p(22), top + p(197), right - p(150), top + p(232)};
    Rounded(dc, input, p(10), GetFocus() == g_replyEdit ? kAccent : kLine);
    InflateRect(&input, -1, -1);
    Rounded(dc, input, p(9), kReplyInput);
    Text(dc, {left, top + p(313), right, top + p(337)},
         L"Речь обрабатывается на этом ПК. Записи не сохраняются.", kQuiet, body.handle);
}

struct ModuleView {
    UINT command;
    const wchar_t* title;
    const wchar_t* glyph;
    void (*paint)(HWND, HDC, const ContentView&);
};
constexpr ModuleView kModules[]{
    {BatteryPage, L"Зарядка", L"\uE945", PaintBatteryView},
    {AudioPage, L"Звук", L"\uE767", PaintAudioView},
    {VoicePage, L"Голос", L"\uE720", PaintVoiceView},
};

void LayoutNavigation(Scale p) {
    for (size_t i = 0; i < g_navigation.size(); ++i)
        MoveWindow(g_navigation[i], p(10), p(79 + (int)i * 47), p(kSidebarWidth-20), p(42), TRUE);
    if (g_checkUpdates) {
        RECT bounds{};
        GetClientRect(GetParent(g_checkUpdates), &bounds);
        MoveWindow(g_checkUpdates, p(10), bounds.bottom - p(49), p(kSidebarWidth-20), p(39), TRUE);
    }
}

void PaintPanel(HWND window, HDC dc) {
    Scale p{GetDpiForWindow(window)};
    RECT r{};
    GetClientRect(window, &r);
    const long left = ContentLeft(r, p), right = left + ContentWidth(r, p), top = SectionTop(p);
    Fill(dc, r, kBackground);
    Fill(dc, {0, 0, p(kSidebarWidth), r.bottom}, kSidebar);
    DashboardAtmosphere(dc, r, p);
    Fill(dc, {p(kSidebarWidth - 1), 0, p(kSidebarWidth), r.bottom}, kLine);
    const auto stats = BatteryStats();
    RECT status{p(12),r.bottom-p(121),p(kSidebarWidth-12),r.bottom-p(64)};
    DashboardCard(dc, status, p);
    const auto color = stats.online ? RGB(100,220,166) : kQuiet;
    Rounded(dc,{p(25),status.top+p(16),p(32),status.top+p(23)},p(7),color);
    Font statusLabel(p(13), FW_SEMIBOLD, L"Segoe UI");
    Font statusDetail(p(11), FW_NORMAL, L"Segoe UI");
    Text(dc,{p(43),status.top+p(6),status.right-p(10),status.top+p(31)},
         L"Устройств: "+std::to_wstring(g_rows.size()),kInk,statusLabel.handle);
    Text(dc,{p(43),status.top+p(30),status.right-p(10),status.bottom-p(6)},
         L"На связи: "+std::to_wstring(stats.online),kQuiet,statusDetail.handle);
    if (g_brand) DrawIconEx(dc, p(17), p(22), g_brand, p(25), p(25), 0, nullptr, DI_NORMAL);
    Font brand(p(19), FW_SEMIBOLD, L"Inter");
    Font title(p(32), FW_SEMIBOLD, L"Inter");
    Text(dc, {p(51), p(17), p(166), p(52)}, L"Контур", kInk, brand.handle);
    const auto& module = kModules[g_module];
    Text(dc, {left, top, right - (IsAudioPage() ? p(182) : 0), top + p(55)},
         module.title, kInk, title.handle);

    if (!IsBatteryPage()) Fill(dc, {left, top + p(53), right, top + p(54)}, kLine);
    module.paint(window, dc, {p, left, right, top, r.bottom});
}

void RoundedPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& box, float radius) {
    const float d = radius * 2;
    path.AddArc(box.X,box.Y,d,d,180,90);
    path.AddArc(box.GetRight()-d,box.Y,d,d,270,90);
    path.AddArc(box.GetRight()-d,box.GetBottom()-d,d,d,0,90);
    path.AddArc(box.X,box.GetBottom()-d,d,d,90,90);
    path.CloseFigure();
}

void SidebarHighlight(HDC dc, const RECT& rect, Scale p, bool accent) {
    if (!GraphicsReady()) {
        DashboardCard(dc, rect, p, accent);
        return;
    }
    using namespace Gdiplus;
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const RectF box(float(rect.left)+.75f,float(rect.top)+.75f,
                    float(rect.right-rect.left)-1.5f,float(rect.bottom-rect.top)-1.5f);
    GraphicsPath path;
    RoundedPath(path,box,float(p(12)));
    LinearGradientBrush shade(box, accent ? Color(62,39,112) : Color(42,38,68),
                              accent ? Color(30,24,54) : Color(21,22,36), 22.f);
    graphics.FillPath(&shade,&path);
    const auto clip = graphics.Save();
    graphics.SetClip(&path);
    GraphicsPath halo;
    halo.AddEllipse(box.X-p(28.f),box.Y-p(50.f),float(p(118)),float(p(127)));
    PathGradientBrush sheen(&halo);
    sheen.SetCenterColor(accent ? Color(191,154,98,255) : Color(105,145,126,221));
    Color transparent(0,126,80,230);
    int count=1;
    sheen.SetSurroundColors(&transparent,&count);
    graphics.FillPath(&sheen,&halo);
    if (accent) {
        SolidBrush light(Color(192,148,230));
        graphics.FillEllipse(&light,box.X-float(p(9)),box.Y+float(p(7)),float(p(13)),box.Height-float(p(14)));
    }
    graphics.Restore(clip);
    LinearGradientBrush edge(box, accent ? Color(171,123,255) : Color(111,100,157),
                              accent ? Color(78,60,134) : Color(47,47,73), 12.f);
    Pen border(&edge,1.f);
    graphics.DrawPath(&border,&path);
}

void SidebarIcon(HDC dc, UINT id, int x, int y, Scale p, COLORREF color) {
    if (!GraphicsReady()) return;
    using namespace Gdiplus;
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.TranslateTransform(float(x),float(y));
    const float scale = p.dpi / 96.f;
    graphics.ScaleTransform(scale,scale);
    const Color ink(GetRValue(color), GetGValue(color), GetBValue(color));
    Pen pen(ink,1.55f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    if (id == BatteryPage) {
        PointF points[]{{13,1},{5,12},{10.7f,12},{9,22},{20,9.5f},{13.6f,9.5f},{15,1}};
        SolidBrush brush(ink);
        graphics.FillPolygon(&brush,points,ARRAYSIZE(points));
    } else if (id == AudioPage) {
        PointF points[]{{3,9},{7,9},{12,5},{12,19},{7,15},{3,15},{3,9}};
        graphics.DrawLines(&pen,points,ARRAYSIZE(points));
        graphics.DrawArc(&pen,9.5f,6.5f,8.f,11.f,-60.f,120.f);
        graphics.DrawArc(&pen,9.5f,3.f,13.f,18.f,-56.f,112.f);
    } else if (id == VoicePage) {
        GraphicsPath capsule;
        RoundedPath(capsule,RectF(8.3f,2.f,7.4f,13.f),3.6f);
        graphics.DrawPath(&pen,&capsule);
        graphics.DrawArc(&pen,4.5f,5.5f,15.f,14.f,0.f,180.f);
        graphics.DrawLine(&pen,12.f,19.5f,12.f,23.f);
        graphics.DrawLine(&pen,8.3f,23.f,15.7f,23.f);
    } else {
        graphics.DrawLine(&pen,12.f,3.f,12.f,16.f);
        PointF arrow[]{{7,11},{12,16},{17,11}};
        graphics.DrawLines(&pen,arrow,ARRAYSIZE(arrow));
        graphics.DrawLine(&pen,5.f,20.f,19.f,20.f);
    }
}

void PaintDialogHeader(HDC dc, const RECT& bounds, Scale p, int height) {
    Fill(dc,bounds,kBackground);
    if (GraphicsReady()) {
        using namespace Gdiplus;
        Graphics graphics(dc);
        const RectF header(0,0,float(bounds.right),float(p(height)));
        LinearGradientBrush wash(PointF(0,0),PointF(0,float(p(height))),
                                 Color(37,29,61),Color(14,15,26));
        graphics.FillRectangle(&wash,header);
    }
    Fill(dc,{p(22),p(height)-1,bounds.right-p(22),p(height)},kLine);
}

void DrawButton(const DRAWITEMSTRUCT& item) {
    Scale p{GetDpiForWindow(item.hwndItem)};
    RECT r = item.rcItem;
    Font label(p(14), FW_SEMIBOLD, L"Inter");
    Font sub(p(12), FW_NORMAL, L"Inter");
    const auto navigation = std::find_if(std::begin(kModules), std::end(kModules), [&](const ModuleView& module) {
        return module.command == item.CtlID;
    });
    if (navigation != std::end(kModules)) {
        const auto& module = *navigation;
        const bool selected = module.command == kModules[g_module].command;
        Fill(item.hDC, r, kSidebar);
        if (selected || (item.itemState & ODS_SELECTED)) SidebarHighlight(item.hDC, r, p, selected);
        const int centerY = (r.top + r.bottom - p(24))/2;
        SidebarIcon(item.hDC,module.command,r.left+p(15),centerY,p,selected ? kAccent : kQuiet);
        Font navLabel(p(16),selected ? FW_SEMIBOLD : FW_NORMAL,L"Segoe UI");
        Text(item.hDC, {r.left+p(52),r.top,r.right-p(8),r.bottom}, module.title,
             selected ? kInk : kQuiet,navLabel.handle);
        FocusAccent(item, r, p);
        return;
    }
    if (item.CtlID == UpdateButton) {
        Fill(item.hDC,r,kSidebar);
        SidebarHighlight(item.hDC,r,p,false);
        SidebarIcon(item.hDC,UpdateButton,r.left+p(13),(r.top+r.bottom-p(24))/2,p,kInk);
        Font caption(p(15),FW_SEMIBOLD,L"Segoe UI");
        Text(item.hDC,{r.left+p(47),r.top,r.right-p(29),r.bottom},L"Обновления",kInk,caption.handle);
        const int x=r.right-p(18), y=r.top+p(13);
        if (g_updateAvailable && GraphicsReady()) {
            using namespace Gdiplus;
            Graphics graphics(item.hDC);
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            SolidBrush mark(Color(37,84,69));
            graphics.FillEllipse(&mark,x-p(8),y-p(8),p(16),p(16));
            Pen check(Color(109,237,169),float(p(2)));
            check.SetLineJoin(LineJoinRound);
            check.SetStartCap(LineCapRound);
            check.SetEndCap(LineCapRound);
            Point points[]{{x-p(4),y},{x-p(1),y+p(3)},{x+p(4),y-p(3)}};
            graphics.DrawLines(&check,points,ARRAYSIZE(points));
        }
        FocusAccent(item,r,p);
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
    const bool catalog = item.CtlID == CatalogButton;
    Rounded(item.hDC, r, p(10), selected || item.CtlID == Add ? kAccent : kLine);
    InflateRect(&r, -p(1), -p(1));
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    Rounded(item.hDC, r, p(9), item.CtlID == Add ? kAccent : item.itemState & ODS_SELECTED ? kLine :
            catalog ? RGB(39, 42, 63) : kSurface);
    if (!output) {
        wchar_t caption[96]{};
        GetWindowTextW(item.hwndItem, caption, ARRAYSIZE(caption));
        Text(item.hDC, r, caption, disabled ? kQuiet : item.CtlID == Add ? kBackground : selected || catalog ? kAccent : kInk,
             item.CtlID == Day || item.CtlID == Week ? sub.handle : label.handle, DT_CENTER);
    } else {
        RECT title{r.left + p(16), r.top + p(7), r.right - p(14), r.top + p(32)};
        RECT detail{title.left, r.top + p(30), title.right, r.bottom - p(6)};
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
    Fill(item.hDC, r, kBackground);
    RECT box{r.left, r.top + 1, r.right, r.bottom - p(6)};
    DashboardCard(item.hDC, box, p, selected);
    Font main(p(15), FW_SEMIBOLD, L"Segoe UI");
    Font small(p(12), FW_NORMAL, L"Segoe UI");
    Font number(p(18), FW_SEMIBOLD, L"Segoe UI");
    Font icon(p(19), FW_NORMAL, L"Segoe MDL2 Assets");
    const wchar_t* symbol = L"\uE83F";
    switch (row.device.kind) {
        case DeviceKind::Headset: symbol = L"\uE7F6"; break;
        case DeviceKind::Keyboard: symbol = L"\uE765"; break;
        case DeviceKind::Mouse: symbol = L"\uE962"; break;
        case DeviceKind::Gamepad: symbol = L"\uE7FC"; break;
        default: break;
    }
    const RECT tile{r.left+p(12), r.top+p(8), r.left+p(53), r.bottom-p(13)};
    Rounded(item.hDC, tile, p(12), RGB(48,37,75));
    Text(item.hDC, tile, symbol, kAccent, icon.handle, DT_CENTER);
    const int meter = r.right - std::clamp(int((r.right-r.left)*.32), p(156), p(330));
    Text(item.hDC, {r.left+p(68),r.top+p(5),meter-p(12),r.top+p(28)},row.device.name,kInk,main.handle);
    Text(item.hDC, {r.left+p(68),r.top+p(26),meter-p(12),r.bottom-p(8)},row.detail,
         row.charging ? kAccent : kQuiet,small.handle);
    const COLORREF color = row.low ? kAmber : row.charging ? kAccent : kInk;
    RECT bar{meter,r.top+p(22),r.right-p(72),r.top+p(30)};
    Rounded(item.hDC, bar, p(8), RGB(47,43,69));
    if (row.percent > 0) {
        bar.right = bar.left+MulDiv(bar.right-bar.left,(std::min)(100,row.percent),100);
        if (GraphicsReady() && !row.low) {
            using namespace Gdiplus;
            Graphics graphics(item.hDC);
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            const float x=float(bar.left), y=float(bar.top), width=float(bar.right-bar.left), height=float(bar.bottom-bar.top);
            const float radius=(std::min)(width,height);
            if (radius > 1.f) {
                GraphicsPath track;
                track.AddArc(x,y,radius,radius,90.f,180.f);
                track.AddArc(x+width-radius,y,radius,radius,270.f,180.f);
                track.CloseFigure();
                LinearGradientBrush shade(PointF(x,y),PointF(x+width,y),Color(115,68,231),Color(212,181,255));
                graphics.FillPath(&shade,&track);
            }
        } else Rounded(item.hDC, bar, p(8), row.low ? kAmber : kAccent);
    }
    Text(item.hDC,{r.right-p(66),r.top,r.right-p(12),r.bottom-p(6)},row.value,color,number.handle,DT_RIGHT);
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
            g_checkUpdates = CreateWindowW(L"BUTTON", L"Обновления",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window,
                (HMENU)(UINT_PTR)UpdateButton, inst, nullptr);
            panel::UpdateAvailable(g_updateAvailable);
            const wchar_t* labels[]{kSpeakers, kHeadphones, L"Настроить выходы", L"+ Добавить устройство", L"24 часа", L"7 дней"};
            for (UINT i = 0; i < ARRAYSIZE(g_buttons); ++i)
                g_buttons[i] = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                             0, 0, 0, 0, window, (HMENU)(UINT_PTR)(Speakers + i), inst, nullptr);
            g_list = MakeList(window, Batteries, L"Заряд устройств", inst);
            g_extraList = MakeList(window, Extras, L"Дополнительные аудиовыходы", inst);
            g_scenarios = CreateWindowW(L"BUTTON", L"Мои сценарии", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, window, (HMENU)(UINT_PTR)ScenarioButton, inst, nullptr);
            g_commandCatalog = CreateWindowW(L"BUTTON", L"Все команды", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, window, (HMENU)(UINT_PTR)CatalogButton, inst, nullptr);
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
            info.ptMinTrackSize = {p(760), p(650)};
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
            if (id == UpdateButton) PostMessageW(g_owner, g_message, panel::CheckUpdates, 0);
            if (id == Configure) PostMessageW(g_owner, g_message, panel::ConfigureOutput, 0);
            if (id == Add) PostMessageW(g_owner, g_message, panel::AddOutput, 0);
            if (id == ScenarioButton) ShowScenarios(window);
            if (id == CatalogButton) ShowCommandCatalog(window);
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
            g_checkUpdates = nullptr;
            g_scenarios = g_commandCatalog = nullptr;
            g_rows.clear();
            return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

template<class Draw>
void PaintLayered(HWND window, Draw draw) {
    if (!GraphicsReady()) return;
    using namespace Gdiplus;
    RECT rect{};
    GetWindowRect(window, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) return;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) { DeleteObject(bitmap); return; }
    const auto previous = SelectObject(dc, bitmap);
    {
        Bitmap image(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
        Graphics graphics(&image);
        graphics.Clear(Color(0,0,0,0));
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        draw(graphics, width, height);
    }
    POINT origin{}, position{rect.left,rect.top};
    SIZE size{width,height};
    BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UpdateLayeredWindow(window,nullptr,&position,&size,dc,&origin,0,&blend,ULW_ALPHA);
    SelectObject(dc,previous);
    DeleteDC(dc);
    DeleteObject(bitmap);
}

void PaintToast(HWND window, const std::wstring& text, Scale p) {
    using namespace Gdiplus;
    PaintLayered(window, [&](Graphics& graphics,int width,int height) {
        RectF box(1,1,float(width-2),float(height-2));
        GraphicsPath path;
        RoundedPath(path,box,float(p(14)));
        LinearGradientBrush surface(box,Color(43,32,72),Color(22,22,39),18.f);
        graphics.FillPath(&surface,&path);
        Pen border(Color(165,151,118,222),float(p(1)));
        graphics.DrawPath(&border,&path);

        const float y = height / 2.f, center = float(p(25));
        SolidBrush tile(Color(62,44,101)), accent(Color(203,179,255));
        const float radius = float(p(12));
        graphics.FillEllipse(&tile,center-radius,y-radius,2*radius,2*radius);
        Pen mark(Color(203,179,255),float(p(1)));
        graphics.DrawLine(&mark,center,y-p(1),center,y+p(5));
        graphics.FillEllipse(&accent,center-p(1),y-p(6),float(p(2)),float(p(2)));

        Gdiplus::Font label(L"Segoe UI",float(p(14)),FontStyleRegular,UnitPixel);
        SolidBrush ink(Color(241,238,255));
        StringFormat layout;
        layout.SetLineAlignment(StringAlignmentCenter);
        graphics.DrawString(text.c_str(),int(text.size()),&label,
            RectF(float(p(47)),float(p(7)),float(width-p(62)),float(height-p(14))),&layout,&ink);
    });
}

LRESULT CALLBACK ToastProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER) {
        DestroyWindow(window);
        return 0;
    }
    if (msg == WM_DESTROY) g_toast = nullptr;
    return DefWindowProcW(window,msg,wp,lp);
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

HWND g_updateDialog;
int g_updateStatus;

LRESULT CALLBACK UpdateDialogProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    const Scale p{GetDpiForWindow(window)};
    switch (msg) {
        case WM_CREATE:
            CreateWindowW(L"BUTTON", L"Закрыть", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                          p(274), p(124), p(110), p(37), window, (HMENU)(UINT_PTR)IDCANCEL,
                          (HINSTANCE)GetWindowLongPtrW(window, GWLP_HINSTANCE), nullptr);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) DestroyWindow(window);
            return 0;
        case WM_DRAWITEM:
            DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(window, &paint);
            RECT bounds{};
            GetClientRect(window, &bounds);
            PaintDialogHeader(dc,bounds,p,108);
            DashboardCard(dc,{p(22),p(23),p(62),p(63)},p,true);
            Font icon(p(20), FW_NORMAL, L"Segoe MDL2 Assets");
            Font title(p(19), FW_SEMIBOLD, L"Inter");
            Font body(p(13), FW_NORMAL, L"Inter");
            const bool latest = g_updateStatus == 0;
            const bool failed = g_updateStatus > 0 && g_updateStatus != 2;
            const bool busy = g_updateStatus == 2;
            const bool installing = g_updateStatus == -2;
            Text(dc, {p(22), p(23), p(62), p(63)}, latest ? L"\uE73E" : failed ? L"\uE7BA" : L"\uE72C",
                 failed ? kAmber : kAccent, icon.handle, DT_CENTER);
            Text(dc, {p(75), p(21), p(384), p(64)}, latest ? L"Последняя версия установлена" :
                 failed ? L"Обновление не удалось" : busy ? L"Проверка уже запущена" : installing ? L"Устанавливаем обновление" : L"Проверяем обновления", kInk, title.handle);
            Text(dc, {p(22), p(73), p(384), p(102)}, latest ? L"Контур уже обновлён." :
                 failed || busy ? L"Попробуй ещё раз позже." : installing ? L"Контур перезапустится после установки." : L"Ищем свежую версию на GitHub…", kQuiet, body.handle);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY: g_updateDialog = nullptr; return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

constexpr wchar_t kListeningClass[] = L"KonturListening";
ULONGLONG g_listeningSince;
bool g_listeningMotion;

void PaintListening(HWND window) {
    using namespace Gdiplus;
    PaintLayered(window, [](Graphics& graphics,int width,int height) {
        graphics.ScaleTransform(width / 198.f, height / 52.f);
        const RectF box(1.f,1.f,196.f,50.f);
        GraphicsPath outline;
        RoundedPath(outline, box, 16.f);
        LinearGradientBrush surface(box, Color(57,36,99), Color(24,22,42), 20.f);
        graphics.FillPath(&surface, &outline);
        Pen border(Color(155,158,124,228), 1.f);
        graphics.DrawPath(&border, &outline);

        GraphicsPath tile;
        RoundedPath(tile, RectF(10,8,36,36), 12.f);
        SolidBrush purple(Color(49,35,82));
        graphics.FillPath(&purple,&tile);
        Pen mic(Color(205,180,255),1.5f);
        mic.SetStartCap(LineCapRound);
        mic.SetEndCap(LineCapRound);
        GraphicsPath capsule;
        RoundedPath(capsule,RectF(24,16,8,13),3.7f);
        graphics.DrawPath(&mic,&capsule);
        graphics.DrawArc(&mic,20.5f,17.f,15.f,15.f,0.f,180.f);
        graphics.DrawLine(&mic,28.f,32.f,28.f,36.f);
        graphics.DrawLine(&mic,24.f,36.f,32.f,36.f);

        Gdiplus::Font label(L"Segoe UI",15.f,FontStyleBold,UnitPixel);
        SolidBrush ink(Color(241,238,255));
        StringFormat format;
        format.SetLineAlignment(StringAlignmentCenter);
        graphics.DrawString(L"Слушаю",-1,&label,RectF(57,0,85,52),&format,&ink);
        const double phase = g_listeningMotion ? (GetTickCount64() - g_listeningSince) / 175.0 : 0;
        for (int i=0;i<5;++i) {
            const float x = float(145+i*8);
            const float amplitude = float(5 + (2-std::abs(i-2))*2 + 5*(1+std::sin(phase-i*.72)));
            Pen bar(Color(i==0 || i==4 ? 170 : 255,193,166,255),2.5f);
            bar.SetStartCap(LineCapRound);
            bar.SetEndCap(LineCapRound);
            graphics.DrawLine(&bar,x,26.f-amplitude/2,x,26.f+amplitude/2);
        }
    });
}

LRESULT CALLBACK ListeningProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (!g_listeningMotion || GetTickCount64() - g_listeningSince >= voice::CommandWindowMs) DestroyWindow(window);
            else PaintListening(window);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            g_listening = nullptr;
            return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

enum ScenarioControl : UINT { ScenarioPhrase = 600, ScenarioKeep, ScenarioHeadset, ScenarioSpeaker,
                             ScenarioPause, ScenarioDiscord, ScenarioSave, ScenarioRemove, ScenarioSlot = 620 };
HWND g_scenarioWindow;
unsigned g_scenarioSlot, g_scenarioOutput, g_scenarioActions;
HFONT g_scenarioFont;
std::wstring g_scenarioStatus;

void LoadScenarioEditor(HWND window, unsigned slot) {
    const auto profiles = voice::LoadProfiles();
    g_scenarioSlot = slot;
    g_scenarioActions = profiles[slot].actions;
    g_scenarioOutput = g_scenarioActions & voice::Headset ? 1 : g_scenarioActions & voice::Speaker ? 2 : 0;
    for (unsigned i = 0; i < profiles.size(); ++i) {
        const auto text = profiles[i].actions ? profiles[i].phrase : L"+ Сценарий " + std::to_wstring(i + 1);
        SetDlgItemTextW(window, ScenarioSlot + i, text.c_str());
    }
    SetDlgItemTextW(window, ScenarioPhrase, profiles[slot].phrase.c_str());
    EnableWindow(GetDlgItem(window, ScenarioRemove), g_scenarioActions != 0);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void DrawScenarioControl(const DRAWITEMSTRUCT& item) {
    const UINT id = item.CtlID;
    const bool entry = id >= ScenarioSlot && id < ScenarioSlot + voice::ScenarioCount;
    const bool output = id >= ScenarioKeep && id <= ScenarioSpeaker;
    const bool toggle = id == ScenarioPause || id == ScenarioDiscord;
    const bool selected = entry ? id - ScenarioSlot == g_scenarioSlot :
        output ? id - ScenarioKeep == g_scenarioOutput :
        toggle && (g_scenarioActions & (id == ScenarioPause ? voice::Pause : voice::Discord));
    const bool primary = id == ScenarioSave;
    Scale p{GetDpiForWindow(item.hwndItem)};
    auto box = item.rcItem;
    Fill(item.hDC, box, entry ? kSidebar : kBackground);
    auto line = selected || (item.itemState & ODS_FOCUS) ? kAccent : kLine;
    Rounded(item.hDC, box, p(10), line);
    InflateRect(&box, -1, -1);
    Rounded(item.hDC, box, p(9), primary ? kAccent : selected ? kReplyInput : kSurface);
    Font font(p(14), selected || primary ? FW_SEMIBOLD : FW_NORMAL, L"Inter");
    wchar_t caption[96]{};
    GetWindowTextW(item.hwndItem, caption, ARRAYSIZE(caption));
    if (entry || toggle) box.left += p(12);
    if (toggle) {
        RECT mark{box.left, box.top + p(10), box.left + p(16), box.top + p(26)};
        Rounded(item.hDC, mark, p(5), selected ? kAccent : kLine);
        if (selected) Text(item.hDC, mark, L"✓", kBackground, font.handle, DT_CENTER);
        box.left += p(26);
    }
    box.right -= p(entry || toggle ? 10 : 0);
    const auto ink = (item.itemState & ODS_DISABLED) ? kQuiet : primary ? kBackground : selected ? kAccent : kInk;
    Text(item.hDC, box, caption, ink, font.handle, entry || toggle ? DT_LEFT : DT_CENTER);
}

LRESULT CALLBACK ScenarioProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    Scale p{GetDpiForWindow(window)};
    switch (message) {
        case WM_CREATE: {
            const auto inst = GetModuleHandleW(nullptr);
            g_scenarioFont = CreateFontW(-p(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Inter");
            auto add = [&](const wchar_t* type, const wchar_t* label, DWORD style, int id, int x, int y, int w, int h) {
                HWND child = CreateWindowW(type, label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                    p(x), p(y), p(w), p(h), window, (HMENU)(UINT_PTR)id, inst, nullptr);
                SendMessageW(child, WM_SETFONT, (WPARAM)g_scenarioFont, TRUE);
                return child;
            };
            auto button = [&](const wchar_t* title, int id, int x, int y, int w, int h) {
                add(L"BUTTON", title, BS_OWNERDRAW, id, x, y, w, h);
            };
            for (unsigned i = 0; i < voice::ScenarioCount; ++i)
                button(L"", ScenarioSlot + i, 14, 61 + int(i) * 44, 152, 34);
            HWND phrase = add(L"EDIT", L"", ES_AUTOHSCROLL, ScenarioPhrase, 208, 106, 386, 23);
            SendMessageW(phrase, EM_LIMITTEXT, 60, 0);
            SendMessageW(phrase, EM_SETCUEBANNER, TRUE, (LPARAM)L"Например, играем");
            button(L"Без смены", ScenarioKeep, 196, 166, 130, 34);
            button(L"Наушники", ScenarioHeadset, 336, 166, 130, 34);
            button(L"Колонки", ScenarioSpeaker, 476, 166, 130, 34);
            button(L"Поставить Яндекс Музыку на паузу", ScenarioPause, 196, 216, 410, 36);
            button(L"Открыть Discord", ScenarioDiscord, 196, 260, 410, 36);
            button(L"Удалить", ScenarioRemove, 196, 320, 100, 36);
            button(L"Сохранить сценарий", ScenarioSave, 420, 320, 186, 36);
            g_scenarioStatus.clear();
            LoadScenarioEditor(window, 0);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            RECT bounds{};
            GetClientRect(window, &bounds);
            PaintDialogHeader(dc,bounds,p,57);
            Fill(dc, {0, 0, p(180), bounds.bottom}, kSidebar);
            Fill(dc, {p(179), 0, p(180), bounds.bottom}, kLine);
            Font heading(p(22), FW_SEMIBOLD, L"Inter");
            Font label(p(13), FW_SEMIBOLD, L"Inter");
            Text(dc, {p(18), p(15), p(168), p(46)}, L"Сценарии", kInk, heading.handle);
            Text(dc, {p(196), p(14), p(606), p(50)}, L"Голосовой сценарий", kInk, heading.handle);
            Text(dc, {p(196), p(63), p(606), p(89)}, L"Фраза после «Контур»", kQuiet, label.handle);
            RECT input{p(196), p(96), p(606), p(137)};
            Rounded(dc, input, p(10), GetFocus() == GetDlgItem(window, ScenarioPhrase) ? kAccent : kLine);
            InflateRect(&input, -1, -1);
            Rounded(dc, input, p(9), kReplyInput);
            Text(dc, {p(196), p(137), p(606), p(164)}, L"Аудиовыход", kQuiet, label.handle);
            Text(dc, {p(196), p(362), p(606), p(393)}, g_scenarioStatus, kAccent, label.handle);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DRAWITEM:
            DrawScenarioControl(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, kInk);
            SetBkColor(dc, kReplyInput);
            static HBRUSH input = CreateSolidBrush(kReplyInput);
            return reinterpret_cast<LRESULT>(input);
        }
        case WM_COMMAND: {
            const UINT id = LOWORD(wp);
            if (id == ScenarioPhrase) {
                if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)
                    InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (id >= ScenarioSlot && id < ScenarioSlot + voice::ScenarioCount) {
                g_scenarioStatus.clear();
                LoadScenarioEditor(window, id - ScenarioSlot);
            } else if (id >= ScenarioKeep && id <= ScenarioSpeaker) {
                g_scenarioOutput = id - ScenarioKeep;
            } else if (id == ScenarioPause || id == ScenarioDiscord) {
                g_scenarioActions ^= id == ScenarioPause ? voice::Pause : voice::Discord;
            } else if (id == ScenarioSave || id == ScenarioRemove) {
                wchar_t phrase[61]{};
                GetDlgItemTextW(window, ScenarioPhrase, phrase, ARRAYSIZE(phrase));
                const bool deleted = id == ScenarioRemove;
                unsigned actions = g_scenarioActions & (voice::Pause | voice::Discord);
                if (g_scenarioOutput == 1) actions |= voice::Headset;
                if (g_scenarioOutput == 2) actions |= voice::Speaker;
                const bool ok = (deleted || actions) &&
                    voice::SaveProfile(g_scenarioSlot, deleted ? voice::Profile{} : voice::Profile{phrase, actions});
                g_scenarioStatus = ok ? (deleted ? L"Сценарий удалён" : L"Сценарий сохранён") :
                    L"Укажи уникальную фразу кириллицей и выбери действие.";
                if (ok) LoadScenarioEditor(window, g_scenarioSlot);
            } else if (id == IDCANCEL) {
                DestroyWindow(window);
                return 0;
            }
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return 0;
        }
        case WM_DESTROY:
            g_scenarioWindow = nullptr;
            return 0;
        case WM_NCDESTROY:
            DeleteObject(g_scenarioFont);
            g_scenarioFont = nullptr;
            break;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void ShowScenarios(HWND owner) {
    if (g_scenarioWindow) { SetForegroundWindow(g_scenarioWindow); return; }
    const auto inst = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.lpfnWndProc = ScenarioProc;
    cls.hInstance = inst;
    cls.hbrBackground = nullptr;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    cls.lpszClassName = L"KonturVoiceScenarios";
    RegisterClassW(&cls);
    Scale p{GetDpiForWindow(owner)};
    RECT bounds{}, size{0, 0, p(626), p(405)};
    GetWindowRect(owner, &bounds);
    constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_CONTROLPARENT, p.dpi);
    const int width = size.right - size.left, height = size.bottom - size.top;
    g_scenarioWindow = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"Мои сценарии",
        style, (bounds.left + bounds.right - width) / 2,
        (bounds.top + bounds.bottom - height) / 2, width, height, owner, nullptr, inst, nullptr);
    if (g_scenarioWindow) {
        DarkCaption(g_scenarioWindow);
        ShowWindow(g_scenarioWindow, SW_SHOW);
    }
}

struct CatalogEntry {
    std::wstring phrase;
    std::wstring detail;
};

enum CatalogControl : UINT { CatalogList = 740, CatalogFirstGroup };
constexpr const wchar_t* kCatalogGroups[]{L"Выходы", L"Музыка", L"Громкость", L"Таймер", L"Сценарии"};
size_t g_catalogGroup;
HWND g_catalogList;
std::vector<CatalogEntry> g_catalogRows;

void PopulateCatalog(HWND window, size_t group) {
    g_catalogGroup = group;
    switch (group) {
        case 0:
            g_catalogRows = {
                {L"Переключи звук на колонки", L"Выбрать выход, назначенный колонками"},
                {L"Переключи звук на наушники", L"Выбрать выход, назначенный наушниками"},
            };
            break;
        case 1:
            g_catalogRows = {
                {L"Включи Яндекс Музыку", L"Открыть настольное приложение"},
                {L"Включи Яндекс Музыку и волну", L"Запустить «Мою волну»"},
                {L"Поставь музыку на паузу", L"Также можно сказать «выключи музыку»"},
                {L"Следующий трек", L"Также можно сказать «пропусти песню»"},
                {L"Предыдущий трек", L"Также можно сказать «предыдущая песня»"},
                {L"Что сейчас играет?", L"Озвучить исполнителя и название трека"},
                {L"Поставь лайк", L"Добавить отметку текущему треку"},
                {L"Убери лайк", L"Также можно сказать «сними лайк»"},
            };
            break;
        case 2:
            g_catalogRows = {
                {L"Музыку на 30 процентов", L"Задать громкость Яндекс Музыки от 0 до 100%"},
                {L"Музыку на максимум", L"Установить 100%"},
                {L"Сделай музыку погромче", L"Прибавить 10 процентных пунктов"},
                {L"Сделай музыку потише", L"Убавить 10 процентных пунктов"},
                {L"Discord на 30 процентов", L"Задать громкость Discord от 0 до 100%"},
                {L"Сделай Discord погромче", L"Прибавить 10 процентных пунктов"},
                {L"Сделай Discord потише", L"Убавить 10 процентных пунктов"},
            };
            break;
        case 3:
            g_catalogRows = {
                {L"Выключи музыку через 20 минут", L"Поставить таймер на паузу музыки через 1–100 минут"},
                {L"Сними таймер", L"Также можно сказать «отмени таймер»"},
            };
            break;
        case 4:
            g_catalogRows.clear();
            for (const auto& profile : voice::LoadProfiles()) {
                if (!profile.actions) continue;
                std::wstring detail;
                auto append = [&](const wchar_t* action) {
                    if (!detail.empty()) detail += L" · ";
                    detail += action;
                };
                if (profile.actions & voice::Headset) append(L"Наушники");
                if (profile.actions & voice::Speaker) append(L"Колонки");
                if (profile.actions & voice::Pause) append(L"Пауза музыки");
                if (profile.actions & voice::Discord) append(L"Открыть Discord");
                g_catalogRows.push_back({profile.phrase, std::move(detail)});
            }
            break;
    }
    SendMessageW(g_catalogList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_catalogList, LB_RESETCONTENT, 0, 0);
    for (const auto& row : g_catalogRows)
        SendMessageW(g_catalogList, LB_ADDSTRING, 0, (LPARAM)row.phrase.c_str());
    SendMessageW(g_catalogList, WM_SETREDRAW, TRUE, 0);
    ShowWindow(g_catalogList, g_catalogRows.empty() ? SW_HIDE : SW_SHOW);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

LRESULT CALLBACK CatalogProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const Scale p{GetDpiForWindow(window)};
    switch (message) {
        case WM_CREATE: {
            const auto inst = GetModuleHandleW(nullptr);
            for (unsigned i = 0; i < ARRAYSIZE(kCatalogGroups); ++i)
                CreateWindowW(L"BUTTON", kCatalogGroups[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    p(21 + i * 108), p(69), p(100), p(33), window, (HMENU)(UINT_PTR)(CatalogFirstGroup + i), inst, nullptr);
            g_catalogList = MakeList(window, CatalogList, L"Фразы и действия", inst);
            MoveWindow(g_catalogList, p(21), p(115), p(532), 8 * p(39), TRUE);
            SendMessageW(g_catalogList, LB_SETITEMHEIGHT, 0, p(39));
            PopulateCatalog(window, 0);
            return 0;
        }
        case WM_COMMAND: {
            const unsigned id = LOWORD(wp);
            if (id >= CatalogFirstGroup && id < CatalogFirstGroup + ARRAYSIZE(kCatalogGroups))
                PopulateCatalog(window, id - CatalogFirstGroup);
            return 0;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            RECT box = item.rcItem;
            if (item.CtlID == CatalogList) {
                if (item.itemID >= g_catalogRows.size()) return TRUE;
                const auto& row = g_catalogRows[item.itemID];
                const bool selected = (item.itemState & ODS_SELECTED) != 0;
                Fill(item.hDC, box, selected ? kSurface : kBackground);
                Fill(item.hDC, {box.left, box.bottom - 1, box.right, box.bottom}, kLine);
                Font phrase(p(14), FW_SEMIBOLD, L"Inter");
                Font detail(p(12), FW_NORMAL, L"Inter");
                Text(item.hDC, {box.left + p(8), box.top + p(1), box.right - p(8), box.top + p(22)},
                     row.phrase, selected ? kAccent : kInk, phrase.handle);
                Text(item.hDC, {box.left + p(8), box.top + p(21), box.right - p(8), box.bottom - p(2)},
                     row.detail, kQuiet, detail.handle);
                FocusAccent(item, box, p);
            } else {
                const auto group = item.CtlID - CatalogFirstGroup;
                if (group >= ARRAYSIZE(kCatalogGroups)) return TRUE;
                const bool selected = group == g_catalogGroup;
                Fill(item.hDC, box, kBackground);
                Rounded(item.hDC, box, p(9), selected ? kSurface : kBackground);
                Font label(p(13), FW_SEMIBOLD, L"Inter");
                Text(item.hDC, box, kCatalogGroups[group], selected ? kAccent : kQuiet, label.handle, DT_CENTER);
                FocusAccent(item, box, p);
            }
            return TRUE;
        }
        case WM_CTLCOLORLISTBOX: {
            static HBRUSH background = CreateSolidBrush(kBackground);
            SetBkColor((HDC)wp, kBackground);
            return (LRESULT)background;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            RECT r{};
            GetClientRect(window, &r);
            PaintDialogHeader(dc,r,p,62);
            Font heading(p(23), FW_SEMIBOLD, L"Inter");
            Font body(p(13), FW_NORMAL, L"Inter");
            Text(dc, {p(22), p(10), r.right - p(22), p(38)}, L"Голосовые команды", kInk, heading.handle);
            Text(dc, {p(22), p(37), r.right - p(22), p(61)},
                 L"Скажи «Контур», затем произнеси фразу из списка.", kQuiet, body.handle);
            Fill(dc, {p(22), p(108), r.right - p(22), p(109)}, kLine);
            if (g_catalogRows.empty())
                Text(dc, {p(22), p(118), r.right - p(22), p(156)},
                     L"Сохрани свою первую фразу в «Моих сценариях».", kQuiet, body.handle);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            g_catalogWindow = nullptr;
            g_catalogList = nullptr;
            g_catalogRows.clear();
            return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void ShowCommandCatalog(HWND owner) {
    if (g_catalogWindow) {
        SetForegroundWindow(g_catalogWindow);
        return;
    }
    const auto inst = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.lpfnWndProc = CatalogProc;
    cls.hInstance = inst;
    cls.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"KonturCommandCatalog";
    RegisterClassW(&cls);
    const Scale p{GetDpiForWindow(owner)};
    RECT bounds{}, size{0, 0, p(574), p(443)};
    GetWindowRect(owner, &bounds);
    constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_CONTROLPARENT, p.dpi);
    const int width = size.right - size.left, height = size.bottom - size.top;
    g_catalogWindow = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"Все команды", style,
        (bounds.left + bounds.right - width) / 2, (bounds.top + bounds.bottom - height) / 2,
        width, height, owner, nullptr, inst, nullptr);
    if (g_catalogWindow) {
        DarkCaption(g_catalogWindow);
        ShowWindow(g_catalogWindow, SW_SHOW);
    }
}

enum SettingControl : UINT { OutputList = 400, OutputName, SaveName, SetSpeakers, SetHeadphones, ToggleExtra, ConnectWindows,
                             ReplyLevel };
HWND g_settingsList, g_settingsName, g_settingsButtons[5];
HFONT g_settingsFont;
bool g_adding;
std::vector<std::wstring> g_optionIds;
std::wstring g_settingsNotice;
void SettingsLayout(HWND window);

int OutputRows() { return (std::clamp)(int(g_optionIds.size()), 1, g_adding ? 4 : 5); }
int SettingsY(int y) { return y - (5 - OutputRows()) * 43; }

void ResizeSettingsDialog(HWND window) {
    if (!window) return;
    Scale p{GetDpiForWindow(window)};
    RECT current{};
    GetClientRect(window, &current);
    RECT size{0, 0, current.right, p(g_adding ? 131 + OutputRows() * 43 : SettingsY(531))};
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
    ResizeSettingsDialog(window);
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
    MoveWindow(g_settingsList, p(22), p(63), w, p(OutputRows() * 43), TRUE);
    if (g_adding) {
        const int footer = r.bottom - p(54);
        MoveWindow(g_settingsButtons[3], r.right - p(153), footer, p(131), p(36), TRUE);
        MoveWindow(g_settingsButtons[4], p(22), footer, p(208), p(36), TRUE);
        ShowWindow(g_settingsName, SW_HIDE);
        for (int i = 0; i < 3; ++i) ShowWindow(g_settingsButtons[i], SW_HIDE);
        return;
    }
    MoveWindow(g_settingsName, p(34), p(SettingsY(316)), w - p(143), p(22), TRUE);
    MoveWindow(g_settingsButtons[0], r.right - p(131), p(SettingsY(307)), p(109), p(39), TRUE);
    MoveWindow(g_settingsButtons[1], p(22), p(SettingsY(357)), p(143), p(38), TRUE);
    MoveWindow(g_settingsButtons[2], p(174), p(SettingsY(357)), p(143), p(38), TRUE);
    MoveWindow(g_settingsButtons[3], r.right - p(200), p(SettingsY(357)), p(178), p(38), TRUE);
    MoveWindow(g_settingsButtons[4], p(22), p(SettingsY(410)), p(208), p(34), TRUE);
    MoveWindow(g_volumeSlider, p(22), p(SettingsY(482)), w, p(32), TRUE);
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
    DashboardCard(item.hDC,{r.left,r.top+1,r.right,r.bottom-p(3)},p,selected);
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

RECT VolumeTrack(HWND window) {
    Scale p{GetDpiForWindow(window)};
    RECT bounds{};
    GetClientRect(window, &bounds);
    const int y = bounds.bottom / 2;
    return {p(9), y - p(2), (std::max)(p(10), int(bounds.right) - p(9)), y + p(2)};
}

LRESULT CALLBACK VolumeSliderProc(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const HDC buffer = CreateCompatibleDC(dc);
        const HBITMAP bitmap = buffer ? CreateCompatibleBitmap(dc, client.right, client.bottom) : nullptr;
        const HGDIOBJ previous = bitmap ? SelectObject(buffer, bitmap) : nullptr;
        const HDC target = previous ? buffer : dc;
        Scale p{GetDpiForWindow(window)};
        const auto track = VolumeTrack(window);
        const int center = track.left + MulDiv(int(track.right - track.left), g_replyVolume, 100);
        const int y = client.bottom / 2;
        Fill(target, client, kBackground);
        Rounded(target, track, p(4), kLine);
        if (center > track.left)
            Rounded(target, {track.left, track.top, center, track.bottom}, p(4), kAccent);
        if (GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
            Rounded(target, {center - p(10), y - p(10), center + p(10), y + p(10)}, p(20), kLine);
        Rounded(target, {center - p(6), y - p(6), center + p(6), y + p(6)}, p(12), kAccent);
        if (previous) {
            BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        EndPaint(window, &paint);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_LBUTTONDOWN) {
        SetFocus(window);
        SetCapture(window);
    }
    if (msg == WM_LBUTTONDOWN || (msg == WM_MOUSEMOVE && GetCapture() == window)) {
        const auto track = VolumeTrack(window);
        const int x = static_cast<short>(LOWORD(lp));
        const int value = std::clamp(MulDiv(x - track.left, 100, track.right - track.left), 0, 100);
        if (value != g_replyVolume) {
            SendMessageW(window, TBM_SETPOS, FALSE, value);
            SendMessageW(GetParent(window), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, value), (LPARAM)window);
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP && GetCapture() == window) {
        ReleaseCapture(); // WM_CAPTURECHANGED commits the final position.
        return 0;
    }
    if (msg == WM_CAPTURECHANGED) {
        SendMessageW(GetParent(window), WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0), (LPARAM)window);
        return 0;
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_UPDATEUISTATE)
        InvalidateRect(window, nullptr, FALSE);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(window, VolumeSliderProc, id);
    return DefSubclassProc(window, msg, wp, lp);
}

LRESULT CALLBACK SettingsProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(window, GWLP_HINSTANCE);
            g_settingsList = MakeList(window, OutputList, L"Доступные аудиовыходы", inst);
            ShowWindow(g_settingsList, SW_SHOW);
            g_settingsName = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                            0, 0, 0, 0, window, (HMENU)(UINT_PTR)OutputName, inst, nullptr);
            g_settingsFont = CreateFontW(-MulDiv(15, GetDpiForWindow(window), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                         DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            SendMessageW(g_settingsName, WM_SETFONT, (WPARAM)g_settingsFont, 0);
            SendMessageW(g_settingsName, EM_SETLIMITTEXT, 127, 0);
            const wchar_t* labels[]{L"Сохранить", L"В колонки", L"В наушники", L"Добавить", L"Подключить в Windows"};
            for (UINT i = 0; i < ARRAYSIZE(g_settingsButtons); ++i)
                g_settingsButtons[i] = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    0, 0, 0, 0, window, (HMENU)(UINT_PTR)(SaveName + i), inst, nullptr);
            if (!g_adding) {
                INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
                InitCommonControlsEx(&controls);
                g_volumeSlider = CreateWindowW(TRACKBAR_CLASSW, L"Громкость ответа Контура",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, 0, 0, 0, 0, window,
                    (HMENU)(UINT_PTR)ReplyLevel, inst, nullptr);
                SendMessageW(g_volumeSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
                SendMessageW(g_volumeSlider, TBM_SETPAGESIZE, 0, 10);
                SendMessageW(g_volumeSlider, TBM_SETPOS, FALSE, g_replyVolume);
                SetWindowSubclass(g_volumeSlider, VolumeSliderProc, 1, 0);
            }
            SettingsLayout(window);
            PopulateOutputSettings();
            return 0;
        }
        case WM_SIZE:
            SettingsLayout(window);
            return 0;
        case WM_HSCROLL:
            if ((HWND)lp == g_volumeSlider && g_volumeSlider) {
                const auto volume = SendMessageW(g_volumeSlider, TBM_GETPOS, 0, 0);
                SendMessageW(g_owner, g_message, panel::ReplyVolume,
                             MAKELPARAM(volume, LOWORD(wp) != TB_THUMBTRACK));
                return 0;
            }
            break;
        case WM_COMMAND: {
            const auto id = LOWORD(wp);
            if (id == OutputName) {
                if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                    Scale p{GetDpiForWindow(window)};
                    RECT r{};
                    GetClientRect(window, &r);
                    RECT field{p(22), p(SettingsY(307)), r.right - p(142), p(SettingsY(346))};
                    InvalidateRect(window, &field, FALSE);
                }
                return 0;
            }
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
        case WM_CTLCOLOREDIT: {
            static HBRUSH input = CreateSolidBrush(kReplyInput);
            SetBkColor((HDC)wp, kReplyInput);
            SetTextColor((HDC)wp, kInk);
            return (LRESULT)input;
        }
        case WM_CTLCOLORLISTBOX: {
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
            PaintDialogHeader(dc,r,p,56);
            Font title(p(22), FW_SEMIBOLD, L"Inter");
            Font small(p(13), FW_NORMAL, L"Inter");
            Text(dc, {p(22), p(15), r.right - p(22), p(53)},
                 g_adding ? L"Какой выход добавить?" : L"Аудиовыходы Windows", kInk, title.handle);
            if (g_optionIds.empty())
                Text(dc, {p(28), p(64), r.right - p(22), p(108)}, L"Новых доступных выходов нет", kQuiet, small.handle);
            if (!g_adding) Text(dc, {p(22), p(SettingsY(281)), r.right - p(22), p(SettingsY(307))}, L"Имя в Контуре", kQuiet, small.handle);
            if (!g_adding) {
                RECT field{p(22), p(SettingsY(307)), r.right - p(142), p(SettingsY(346))};
                Rounded(dc, field, p(10), GetFocus() == g_settingsName ? kAccent : kLine);
                InflateRect(&field, -1, -1);
                Rounded(dc, field, p(9), kReplyInput);
                Text(dc, {p(242), p(SettingsY(410)), r.right - p(22), p(SettingsY(444))}, g_settingsNotice, kQuiet, small.handle, DT_RIGHT);
                Fill(dc, {p(22), p(SettingsY(452)), r.right - p(22), p(SettingsY(453))}, kLine);
                Text(dc, {p(22), p(SettingsY(457)), r.right - p(95), p(SettingsY(480))}, L"Громкость ответа Контура", kInk, small.handle);
                Text(dc, {r.right - p(90), p(SettingsY(457)), r.right - p(22), p(SettingsY(480))},
                     std::to_wstring(g_replyVolume) + L"%", kAccent, small.handle, DT_RIGHT);
            } else if (!g_settingsNotice.empty()) {
                Text(dc, {p(22), r.bottom - p(68), r.right - p(22), r.bottom - p(54)},
                     g_settingsNotice, kQuiet, small.handle);
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            g_settings = nullptr;
            g_volumeSlider = nullptr;
            DeleteObject(g_settingsFont);
            g_settingsNotice.clear();
            return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

}  // namespace

namespace panel {

bool IsOpen() { return g_panel != nullptr; }

void UpdateAvailable(bool available) {
    g_updateAvailable = available;
    if (!g_checkUpdates) return;
    SetWindowTextW(g_checkUpdates, available ? L"Обновления · доступна новая версия" : L"Обновления");
    InvalidateRect(g_checkUpdates, nullptr, FALSE);
}

void ShowUpdateStatus(int status) {
    if (!g_panel || (status >= 0 && !g_updateDialog)) return;
    g_updateStatus = status;
    if (!g_updateDialog) {
        const auto inst = (HINSTANCE)GetWindowLongPtrW(g_panel, GWLP_HINSTANCE);
        constexpr wchar_t dialogClass[] = L"KonturUpdateDialog";
        Register(inst, dialogClass, UpdateDialogProc, g_brand);
        const Scale p{GetDpiForWindow(g_panel)};
        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        RECT size{0, 0, p(406), p(182)}, owner{};
        AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_CONTROLPARENT, p.dpi);
        GetWindowRect(g_panel, &owner);
        const int width = size.right - size.left, height = size.bottom - size.top;
        g_updateDialog = CreateWindowExW(WS_EX_CONTROLPARENT, dialogClass, L"Обновления", style,
            (owner.left + owner.right - width) / 2, (owner.top + owner.bottom - height) / 2,
            width, height, g_panel, nullptr, inst, nullptr);
        if (!g_updateDialog) return;
        DarkCaption(g_updateDialog);
        ShowWindow(g_updateDialog, SW_SHOW);
    }
    InvalidateRect(g_updateDialog, nullptr, FALSE);
}

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
        RECT size{0, 0, p(1100), p(710)};
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
        RECT size{0, 0, p(adding ? 560 : 600), p(adding ? 174 : 531)}, owner{};
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

void UpdateReplyVolume(int volume) {
    if (g_replyVolume == volume) return;
    g_replyVolume = volume;
    if (!g_volumeSlider) return;
    if (SendMessageW(g_volumeSlider, TBM_GETPOS, 0, 0) != volume)
        SendMessageW(g_volumeSlider, TBM_SETPOS, FALSE, volume);
    InvalidateRect(g_volumeSlider, nullptr, FALSE);
    Scale p{GetDpiForWindow(g_settings)};
    RECT r{};
    GetClientRect(g_settings, &r);
    const RECT percent{r.right - p(90), p(SettingsY(457)), r.right - p(22), p(SettingsY(480))};
    InvalidateRect(g_settings, &percent, FALSE);
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
    const int width = p(198), height = p(52);
    const int left = area.left + (area.right - area.left - width) / 2;
    const int top = area.top + (area.bottom - area.top) * 58 / 100 - height / 2;
    g_listening = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST |
                                  WS_EX_TOOLWINDOW, kListeningClass, L"Контур слушает команду", WS_POPUP,
                                  left, top, width, height, owner, nullptr, inst, nullptr);
    if (!g_listening) return;
    BOOL motion = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motion, 0);
    g_listeningMotion = motion != FALSE;
    g_listeningSince = GetTickCount64();
    PaintListening(g_listening);
    ShowWindow(g_listening, SW_SHOWNOACTIVATE);
    SetTimer(g_listening, 1, g_listeningMotion ? 50 : voice::CommandWindowMs, nullptr);
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
    if (g_catalogWindow && (message.hwnd == g_catalogWindow || IsChild(g_catalogWindow, message.hwnd))) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            DestroyWindow(g_catalogWindow);
            return true;
        }
        return IsDialogMessageW(g_catalogWindow, &message);
    }
    if (g_scenarioWindow && IsDialogMessageW(g_scenarioWindow, &message)) return true;
    if (g_updateDialog && (message.hwnd == g_updateDialog || IsChild(g_updateDialog, message.hwnd))) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            DestroyWindow(g_updateDialog);
            return true;
        }
        return IsDialogMessageW(g_updateDialog, &message);
    }
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
    const Scale p{GetDpiForSystem()};
    if (text.empty() || !GraphicsReady()) return;
    const int width = (std::min)(p(382),int(screen.rcWork.right-screen.rcWork.left)-p(28));
    if (width <= p(62)) return;
    using namespace Gdiplus;
    Gdiplus::Font label(L"Segoe UI",float(p(14)),FontStyleRegular,UnitPixel);
    Bitmap measureBitmap(1,1,PixelFormat32bppPARGB);
    Graphics measure(&measureBitmap);
    RectF measured;
    measure.MeasureString(text.c_str(),int(text.size()),&label,
                          RectF(0,0,float(width-p(62)),float(screen.rcWork.bottom-screen.rcWork.top)),&measured);
    const int height = (std::max)(p(48),int(std::ceil(measured.Height))+p(24));
    const int x = screen.rcWork.right - width - p(14);
    const int y = screen.rcWork.bottom - height - p(14);
    g_toast = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                              kToast,text.c_str(),WS_POPUP,x,y,width,height,owner,nullptr,inst,nullptr);
    if (g_toast) {
        PaintToast(g_toast,text,p);
        ShowWindow(g_toast,SW_SHOWNOACTIVATE);
        SetTimer(g_toast,1,UINT(std::clamp<size_t>(1500+text.size()*23,2200,8000)),nullptr);
    }
}

}  // namespace panel
