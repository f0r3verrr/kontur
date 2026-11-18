#pragma once

#include <windows.h>
#include <cstdint>
#include "devices.h"

namespace history {
void Observe(const DeviceStatus& device);
void UsbDisconnected(const std::wstring& key);
int LastPercent(const std::wstring& key);
std::wstring LastReadingAge(const std::wstring& key);
std::int64_t LastCharging(const std::wstring& key);
std::wstring ChargingAge(std::int64_t timestamp);
std::wstring ChargeSummary(const DeviceStatus& device, bool russian = false);
void Paint(HWND window, HDC dc, const RECT& bounds, const DeviceStatus& device, bool oneDay = false, bool showHeader = true);
void Show(HWND owner, const DeviceStatus& device);
}
