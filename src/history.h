#pragma once

#include <windows.h>
#include "devices.h"

namespace history {
void Observe(const DeviceStatus& device);
void UsbDisconnected(const std::wstring& key);
int LastPercent(const std::wstring& key);
std::wstring LastReadingAge(const std::wstring& key);
std::wstring ChargeSummary(const DeviceStatus& device, bool russian = false);
void Paint(HWND window, HDC dc, const RECT& bounds, const DeviceStatus& device);
void Show(HWND owner, const DeviceStatus& device);
}
