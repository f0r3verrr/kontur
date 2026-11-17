#include "history.h"
#include "appearance.h"
#include <gdiplus.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <map>

namespace {

constexpr int64_t kDay = 24 * 60 * 60;
constexpr int64_t kWeek = 7 * kDay;
constexpr int64_t kSampleInterval = 15 * 60;
constexpr size_t kMaxSamples = 12000;
constexpr wchar_t kRegistry[] = L"Software\\WirelessBattery\\History";
constexpr wchar_t kClass[] = L"WirelessBatteryHistory";

struct Reading {
    int64_t time;
    int64_t percent;
};

struct Events {
    int64_t lastUsb = 0;
    int64_t lastCharging = 0;
    int64_t lastFull = 0;
    int64_t state = 0;
    Reading lastReading{};
};

constexpr int kUsb = 1, kCharging = 2, kFull = 4;
struct History {
    Events events;
    std::vector<Reading> readings;
};
std::map<std::wstring, History> g_history;
HWND g_window;
DeviceStatus g_selected;

int64_t Now() { return std::time(nullptr); }

History& Load(const std::wstring& key) {
    auto [it, inserted] = g_history.try_emplace(key);
    auto& h = it->second;
    if (!inserted) return h;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistry, key.c_str(), RRF_RT_REG_BINARY,
                     nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(Events) || size > sizeof(Events) + kMaxSamples * sizeof(Reading) ||
        (size - sizeof(Events)) % sizeof(Reading)) return h;
    std::vector<BYTE> bytes(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistry, key.c_str(), RRF_RT_REG_BINARY,
                     nullptr, bytes.data(), &size) != ERROR_SUCCESS || size != bytes.size()) return h;
    memcpy(&h.events, bytes.data(), sizeof(Events));
    h.readings.resize((size - sizeof(Events)) / sizeof(Reading));
    if (!h.readings.empty())
        memcpy(h.readings.data(), bytes.data() + sizeof(Events), size - sizeof(Events));
    return h;
}

void Save(const std::wstring& key, const History& h) {
    const size_t readingsSize = h.readings.size() * sizeof(Reading);
    std::vector<BYTE> bytes(sizeof(Events) + readingsSize);
    memcpy(bytes.data(), &h.events, sizeof(Events));
    if (readingsSize) memcpy(bytes.data() + sizeof(Events), h.readings.data(), readingsSize);
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegistry, key.c_str(), REG_BINARY, bytes.data(), (DWORD)bytes.size());
}

std::wstring Ago(int64_t timestamp, bool russian = false) {
    const auto minutes = std::max<int64_t>(0, Now() - timestamp) / 60;
    if (minutes == 0) return russian ? L"только что" : L"just now";
    if (minutes < 60) return std::to_wstring(minutes) + (russian ? L" мин назад" : L" min ago");
    if (minutes < 1440) return std::to_wstring(minutes / 60) + (russian ? L" ч назад" : L" h ago");
    return std::to_wstring(minutes / 1440) + (russian ? L" дн. назад" : L" d ago");
}

using ChartRun = std::vector<Gdiplus::PointF>;

void PaintTrace(HDC dc, const RECT& bounds, const RECT& plot, const std::vector<ChartRun>& runs, float scale) {
    using namespace Gdiplus;
    struct GraphicsRuntime {
        ULONG_PTR token = 0;
        GraphicsRuntime() { GdiplusStartupInput input; GdiplusStartup(&token, &input, nullptr); }
        ~GraphicsRuntime() { if (token) GdiplusShutdown(token); }
    };
    static GraphicsRuntime runtime;
    if (!runtime.token) return;
    Graphics graphics(dc);
    graphics.TranslateTransform((REAL)bounds.left, (REAL)bounds.top);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const RectF frame(0, 0, (REAL)(bounds.right - bounds.left), (REAL)(bounds.bottom - bounds.top));
    LinearGradientBrush background(frame, Color(39, 37, 57), Color(32, 31, 47), 80.0f);
    graphics.FillRectangle(&background, frame);
    if (plot.right <= plot.left || plot.bottom <= plot.top) return;

    const PointF top(0, (REAL)plot.top - 3 * scale), bottom(0, (REAL)plot.bottom + 3 * scale);
    LinearGradientBrush fill(top, bottom, Color(58, 184, 167, 255), Color(0, 126, 104, 203));
    LinearGradientBrush line(top, bottom, Color(222, 210, 255), Color(155, 141, 232));
    Pen grid(Color(46, 214, 179, 233), 1);
    for (int step = 0; step <= 4; ++step) {
        const int y = plot.top + (plot.bottom - plot.top) * step / 4;
        graphics.DrawLine(&grid, (int)plot.left, y, (int)plot.right, y);
    }

    GraphicsPath trace;
    for (const auto& run : runs) {
        if (run.size() < 2) continue;
        GraphicsPath area;
        area.AddLines(run.data(), (INT)run.size());
        const auto& start = run.front();
        const auto& end = run.back();
        area.AddLine(end, PointF(end.X, (REAL)plot.bottom));
        area.AddLine(PointF(end.X, (REAL)plot.bottom), PointF(start.X, (REAL)plot.bottom));
        area.CloseFigure();
        graphics.FillPath(&fill, &area);
        trace.StartFigure();
        trace.AddLines(run.data(), (INT)run.size());
    }
    for (auto [width, alpha] : { std::pair{10.0f, 13}, std::pair{5.0f, 27} }) {
        Pen glow(Color((BYTE)alpha, 184, 167, 255), width * scale);
        glow.SetLineJoin(LineJoinRound);
        glow.SetStartCap(LineCapRound);
        glow.SetEndCap(LineCapRound);
        graphics.DrawPath(&glow, &trace);
    }
    Pen edge(&line, 2.8f * scale);
    edge.SetLineJoin(LineJoinRound);
    edge.SetStartCap(LineCapRound);
    edge.SetEndCap(LineCapRound);
    graphics.DrawPath(&edge, &trace);
    for (const auto& run : runs) {
        if (run.size() != 1 && &run != &runs.back()) continue;
        const auto point = run.back();
        SolidBrush glow(Color(40, 184, 167, 255));
        SolidBrush core(Color(222, 210, 255));
        const float radius = (&run == &runs.back() ? 4.0f : 2.2f) * scale;
        graphics.FillEllipse(&glow, point.X - radius * 2, point.Y - radius * 2, radius * 4, radius * 4);
        graphics.FillEllipse(&core, point.X - radius, point.Y - radius, radius * 2, radius * 2);
    }
}

void PaintChart(HWND window, HDC dc, const RECT& bounds, const DeviceStatus& device, bool oneDay) {
    using namespace appearance;
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    const RECT client{0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
    const UINT dpi = GetDpiForWindow(window);
    auto px = [dpi](int x) { return MulDiv(x, dpi, 96); };
    const RECT plot{px(53), px(72), client.right - px(50), client.bottom - px(34)};
    const auto now = Now();
    const auto range = oneDay ? kDay : kWeek;
    const auto& record = Load(device.key);
    Reading previous{};
    Gdiplus::PointF last;
    std::vector<ChartRun> runs;
    if (plot.right > plot.left && plot.bottom > plot.top) {
        for (const auto& sample : record.readings) {
            if (sample.time < now - range || sample.time > now || sample.percent < 0 || sample.percent > 100) continue;
            const auto x = (float)(plot.left + (sample.time - (now - range)) * (plot.right - plot.left) / double(range));
            const auto y = (float)(plot.bottom - (plot.bottom - plot.top) * sample.percent / 100.0);
            if (!previous.time || sample.time - previous.time > kSampleInterval * 4 / 3) runs.emplace_back();
            else if (last.Y != y) runs.back().emplace_back(x, last.Y);
            runs.back().emplace_back(x, y);
            last = {x, y};
            previous = sample;
        }
    }

    SetDCBrushColor(dc, kSurface);
    FillRect(dc, &bounds, (HBRUSH)GetStockObject(DC_BRUSH));
    PaintTrace(dc, bounds, plot, runs, dpi / 96.0f);
    SetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
    SetBkMode(dc, TRANSPARENT);
    auto makeFont = [&](int size, int weight) {
        return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    };
    HFONT font = makeFont(12, FW_NORMAL), title = makeFont(17, FW_SEMIBOLD), value = makeFont(26, FW_SEMIBOLD);
    HGDIOBJ oldFont = SelectObject(dc, font);
    auto label = [dc](int x, int y, const std::wstring& text) {
        TextOutW(dc, x, y, text.c_str(), (int)text.size());
    };
    const auto summary = history::ChargeSummary(device, true);
    SetTextColor(dc, kInk);
    SelectObject(dc, title);
    RECT name{px(16), px(10), client.right - px(124), px(37)};
    DrawTextW(dc, device.name.c_str(), (int)device.name.size(), &name,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, value);
    SetTextColor(dc, kAccent);
    RECT stat{client.right - px(116), px(9), client.right - px(18), px(41)};
    const int percent = device.percent >= 0 ? device.percent : -1;
    const auto current = device.usbOnly ? L"USB" : percent >= 0 ? std::to_wstring(percent) + L"%" : L"–";
    DrawTextW(dc, current.c_str(), (int)current.size(), &stat, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, font);
    SetTextColor(dc, kQuiet);
    if (!summary.empty()) label(px(16), px(41), summary);

    const auto [left, top, right, bottom] = plot;
    if (right > left && bottom > top) {
        for (int percent : {100, 75, 50, 25, 0}) {
            const int y = bottom - (bottom - top) * percent / 100;
            label(px(13), y - px(7), std::to_wstring(percent) + L"%");
        }
        const int ticks = oneDay ? 4 : 7;
        for (int tick = 0; tick <= ticks; ++tick) {
            const int x = left + (right - left) * tick / ticks;
            label(x - px(10), bottom + px(8), tick == ticks ? L"Сейчас" :
                  std::to_wstring((ticks - tick) * (oneDay ? 6 : 1)) + (oneDay ? L" ч" : L" д"));
        }

        if (!previous.time) label(left + px(12), top + px(14), L"Пока нет замеров заряда");
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteObject(title);
    DeleteObject(value);
    RestoreDC(dc, saved);
}

LRESULT CALLBACK HistoryProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT bounds{};
            GetClientRect(window, &bounds);
            PaintChart(window, dc, bounds, g_selected, false);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            g_window = nullptr;
            return 0;
        case WM_DPICHANGED: {
            const auto& rect = *reinterpret_cast<RECT*>(lp);
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
    }
    return DefWindowProcW(window, msg, wp, lp);
}

}  // namespace

namespace history {

void Paint(HWND window, HDC dc, const RECT& bounds, const DeviceStatus& device, bool oneDay) {
    PaintChart(window, dc, bounds, device, oneDay);
}

void Observe(const DeviceStatus& device) {
    if (!device.connected || device.asleep) return;
    auto& h = Load(device.key);
    auto& events = h.events;
    const auto now = Now();
    const auto oldSize = h.readings.size();
    h.readings.erase(std::remove_if(h.readings.begin(), h.readings.end(), [now](const Reading& r) {
        return r.time < now - kWeek || r.time > now;
    }), h.readings.end());
    const int state = (device.usbOnly ? kUsb : 0) | (device.charging ? kCharging : 0) |
                      (device.chargeComplete ? kFull : 0);
    const auto entered = state & ~events.state;
    bool changed = state != events.state || oldSize != h.readings.size();
    if (entered & kUsb) events.lastUsb = now;
    if (entered & kFull) events.lastFull = now;
    if (device.charging && ((entered & kCharging) || now - events.lastCharging >= kSampleInterval)) {
        events.lastCharging = now;
        changed = true;
    }
    events.state = state;
    if (device.percent >= 0 && device.percent <= 100) {
        events.lastReading = { now, device.percent };
        if (h.readings.empty() || h.readings.back().percent != device.percent ||
            now - h.readings.back().time >= kSampleInterval) {
            if (h.readings.size() >= kMaxSamples) h.readings.erase(h.readings.begin());
            h.readings.push_back(events.lastReading);
            changed = true;
        }
    }
    if (changed) Save(device.key, h);
    if (g_window && g_selected.key == device.key) {
        g_selected = device;
        InvalidateRect(g_window, nullptr, FALSE);
    }
}

void UsbDisconnected(const std::wstring& key) {
    auto& h = Load(key);
    if (!(h.events.state & kUsb)) return;
    h.events.state &= ~kUsb;
    Save(key, h);
}

int LastPercent(const std::wstring& key) {
    const auto& reading = Load(key).events.lastReading;
    return reading.time ? (int)reading.percent : -1;
}

std::wstring LastReadingAge(const std::wstring& key) {
    auto time = Load(key).events.lastReading.time;
    return time ? Ago(time) : L"";
}

std::wstring ChargeSummary(const DeviceStatus& device, bool russian) {
    const auto& events = Load(device.key).events;
    if (device.usbOnly) {
        if (russian && events.lastUsb) return L"Подключение по USB отмечено " + Ago(events.lastUsb, true);
        return russian ? L"Подключено по USB" : L"Connected via USB";
    }
    if (device.charging) return russian ? L"Заряжается" : L"Charging";
    if (events.lastFull)
        return (russian ? L"Полный заряд отмечен " : L"Full charge last reported: ") + Ago(events.lastFull, russian);
    if (events.lastCharging)
        return (russian ? L"Последняя активная зарядка " : L"Last observed charging: ") + Ago(events.lastCharging, russian);
    if (events.lastUsb)
        return (russian ? L"Последнее подключение по USB " : L"Last USB connection: ") + Ago(events.lastUsb, russian);
    return L"";
}

void Show(HWND owner, const DeviceStatus& device) {
    g_selected = device;
    const std::wstring title = device.name + L" · История заряда";
    if (g_window) {
        SetWindowTextW(g_window, title.c_str());
        InvalidateRect(g_window, nullptr, FALSE);
        ShowWindow(g_window, SW_RESTORE);
        SetForegroundWindow(g_window);
        return;
    }
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    WNDCLASSW cls{};
    cls.lpfnWndProc = HistoryProc;
    cls.hInstance = inst;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    cls.lpszClassName = kClass;
    RegisterClassW(&cls);
    const UINT dpi = GetDpiForWindow(owner);
    g_window = CreateWindowExW(WS_EX_APPWINDOW, kClass, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(620, dpi, 96), MulDiv(360, dpi, 96),
                              owner, nullptr, inst, nullptr);
    if (g_window) {
        appearance::DarkCaption(g_window);
        ShowWindow(g_window, SW_SHOW);
        SetForegroundWindow(g_window);
    }
}

}  // namespace history
