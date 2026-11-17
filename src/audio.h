#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace audio {
struct Endpoint {
    std::wstring id, name;
    bool available = false;
    unsigned form = 0;
};

void Start(HWND owner, UINT message);
void Stop();
void Refresh();
const std::vector<Endpoint>& Endpoints();
const std::wstring& Current();
const std::wstring& Configured(int slot);
const Endpoint* Find(const std::wstring& id);
bool Assign(int slot, const std::wstring& id);
const std::vector<std::wstring>& ExtraOutputs();
bool SaveExtra(const std::wstring& id, bool include);
std::vector<std::wstring> Cycle();
std::wstring SwitchTo(const std::wstring& id);  // Empty on success, localized error otherwise.
}
