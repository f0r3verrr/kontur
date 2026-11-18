#pragma once
#include <windows.h>
bool StartWindowsAction(UINT resource, const wchar_t* setup = L"", HWND notify = nullptr, UINT message = 0, bool capture = false, unsigned context = 0);
