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

enum Action : UINT { SelectOutput, ConfigureOutput, AddOutput, SelectExtra, AudioSettingsChanged,
                     ToggleVoice, ToggleReply, SaveReply };

bool IsOpen();
void Show(HWND owner, UINT message);
void UpdateAudio();
void UpdateVoice(bool enabled, const std::wstring& status);
void UpdateReply(bool enabled, const std::wstring& text);
std::wstring ReplyText();
void ShowListening(HWND owner);
void HideListening();
void ShowOutputSettings(bool adding);
std::wstring ExtraId(size_t index);
void SetBatteries(std::vector<BatteryRow> rows);
void Toast(HWND owner, const RECT& anchor, const std::wstring& text);
bool HandleKeyboard(MSG& message);

}
