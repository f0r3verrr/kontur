#include "battery_monitor.h"
#include "devices.h"

namespace {
constexpr DWORD kPollInterval = 60000;
HWND g_owner;
UINT g_resultsMessage;
HANDLE g_refresh, g_stop, g_worker;

DWORD WINAPI Poll(LPVOID) {
    HANDLE signals[]{g_stop, g_refresh};
    while (WaitForSingleObject(g_stop, 0) == WAIT_TIMEOUT) {
        auto* result = new std::vector<DeviceStatus>(PollAllDevices());
        if (WaitForSingleObject(g_stop, 0) != WAIT_TIMEOUT ||
            !PostMessageW(g_owner, g_resultsMessage, 0, (LPARAM)result)) delete result;
        const auto wait = WaitForMultipleObjects(2, signals, FALSE, kPollInterval);
        if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT) break;
    }
    return 0;
}
}

namespace battery_monitor {
bool Start(HWND owner, UINT resultsMessage) {
    g_owner = owner;
    g_resultsMessage = resultsMessage;
    g_refresh = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_refresh && g_stop) g_worker = CreateThread(nullptr, 0, Poll, nullptr, 0, nullptr);
    if (!g_worker) { Stop(); return false; }
    return true;
}

void Refresh() { if (g_refresh) SetEvent(g_refresh); }

void Stop() {
    if (g_stop) SetEvent(g_stop);
    if (g_worker) {
        // Vendor queries have their own timeouts. Keep their handles valid until shutdown.
        if (WaitForSingleObject(g_worker, 5000) != WAIT_OBJECT_0) return;
        CloseHandle(g_worker);
    }
    if (g_refresh) CloseHandle(g_refresh);
    if (g_stop) CloseHandle(g_stop);
    g_worker = g_refresh = g_stop = nullptr;
}
}
