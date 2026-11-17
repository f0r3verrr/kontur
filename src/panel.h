#pragma once

#include <windows.h>
#include "devices.h"

namespace panel {

struct BatteryRow {
    DeviceStatus device;
    std::wstring detail, value;
    int percent;
    bool charging;
    bool low;
};

enum Action : UINT { SelectOutput, ConfigureOutput, AddOutput, ShowExtras };

bool IsOpen();
void Show(HWND owner, UINT message);
void UpdateAudio();
void SetBatteries(std::vector<BatteryRow> rows);
void Toast(HWND owner, const RECT& anchor, const std::wstring& text);
bool HandleKeyboard(MSG& message);

}
