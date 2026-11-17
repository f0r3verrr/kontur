#pragma once
#include <windows.h>

namespace battery_monitor {
bool Start(HWND owner, UINT resultsMessage);
void Refresh();
void Stop();
}
