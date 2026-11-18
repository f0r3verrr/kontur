#include "music_volume.h"

#include <windows.h>
#include <UIAutomation.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
struct ReleaseCom { void operator()(IUnknown* value) const { if (value) value->Release(); } };
template<class T> using ComPtr = std::unique_ptr<T, ReleaseCom>;

bool MusicWindow(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768]{};
    DWORD size = ARRAYSIZE(path);
    const bool found = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!found) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return !_wcsicmp(name, L"Яндекс Музыка.exe") || !_wcsicmp(name, L"YandexMusic.exe");
}

struct WindowList { HWND windows[16]{}; unsigned count = 0; };
BOOL CALLBACK CollectWindow(HWND window, LPARAM context) {
    auto& list = *reinterpret_cast<WindowList*>(context);
    if (list.count < ARRAYSIZE(list.windows) && MusicWindow(window)) list.windows[list.count++] = window;
    return TRUE;
}

bool SetOnWindow(IUIAutomation* automation, IUIAutomationCondition* condition, HWND window,
                 int percent, bool relative) {
    const bool minimized = IsIconic(window);
    const bool hidden = !IsWindowVisible(window);
    if (minimized || hidden) ShowWindowAsync(window, SW_SHOWNOACTIVATE);
    bool success = false;
    bool valueSent = false;
    for (int attempt = 0; attempt < 4 && !success; ++attempt) {
        IUIAutomationElement* rawRoot = nullptr;
        if (SUCCEEDED(automation->ElementFromHandle(window, &rawRoot)) && rawRoot) {
            ComPtr<IUIAutomationElement> root(rawRoot);
            IUIAutomationElement* rawSlider = nullptr;
            if (SUCCEEDED(root->FindFirst(TreeScope_Descendants, condition, &rawSlider)) && rawSlider) {
                ComPtr<IUIAutomationElement> slider(rawSlider);
                IUIAutomationRangeValuePattern* rawRange = nullptr;
                if (SUCCEEDED(slider->GetCurrentPatternAs(UIA_RangeValuePatternId, IID_PPV_ARGS(&rawRange))) && rawRange) {
                    ComPtr<IUIAutomationRangeValuePattern> range(rawRange);
                    double current = 0, minimum = 0, maximum = 0;
                    BOOL readOnly = TRUE;
                    if (SUCCEEDED(range->get_CurrentValue(&current)) && SUCCEEDED(range->get_CurrentMinimum(&minimum)) &&
                        SUCCEEDED(range->get_CurrentMaximum(&maximum)) && SUCCEEDED(range->get_CurrentIsReadOnly(&readOnly)) &&
                        !readOnly && maximum > minimum) {
                        const double target = std::clamp(relative ? current + percent * (maximum - minimum) / 100.0 :
                            minimum + percent * (maximum - minimum) / 100.0, minimum, maximum);
                        if (SUCCEEDED(range->SetValue(target))) {
                            valueSent = true;
                            for (int poll = 0; poll < 4 && !success; ++poll) {
                                Sleep(120);
                                IUIAutomationElement* rawFreshRoot = nullptr;
                                if (SUCCEEDED(automation->ElementFromHandle(window, &rawFreshRoot)) && rawFreshRoot) {
                                    ComPtr<IUIAutomationElement> freshRoot(rawFreshRoot);
                                    IUIAutomationElement* rawFreshSlider = nullptr;
                                    if (SUCCEEDED(freshRoot->FindFirst(TreeScope_Descendants, condition, &rawFreshSlider)) &&
                                        rawFreshSlider) {
                                        ComPtr<IUIAutomationElement> freshSlider(rawFreshSlider);
                                        IUIAutomationRangeValuePattern* rawFreshRange = nullptr;
                                        if (SUCCEEDED(freshSlider->GetCurrentPatternAs(UIA_RangeValuePatternId,
                                                                                       IID_PPV_ARGS(&rawFreshRange))) && rawFreshRange) {
                                            ComPtr<IUIAutomationRangeValuePattern> freshRange(rawFreshRange);
                                            double actual = 0;
                                            success = SUCCEEDED(freshRange->get_CurrentValue(&actual)) &&
                                                std::abs(actual - target) <= (maximum - minimum) * 0.025;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (valueSent) break;
        if (!success && attempt < 3) Sleep(180);
    }
    if (minimized) ShowWindowAsync(window, SW_SHOWMINNOACTIVE);
    else if (hidden) ShowWindowAsync(window, SW_HIDE);
    return success;
}
}  // namespace

bool music_volume::Set(int percent, bool relative) {
    if ((!relative && (percent < 0 || percent > 100)) || (relative && (percent < -100 || percent > 100)))
        return false;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return false;
    IUIAutomation* rawAutomation = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&rawAutomation));
    ComPtr<IUIAutomation> automation(rawAutomation);
    bool success = false;
    if (SUCCEEDED(created) && automation) {
        VARIANT name{};
        name.vt = VT_BSTR;
        name.bstrVal = SysAllocString(L"Управление громкостью");
        IUIAutomationCondition* rawName = nullptr;
        IUIAutomationCondition* rawType = nullptr;
        IUIAutomationCondition* rawBoth = nullptr;
        if (name.bstrVal && SUCCEEDED(automation->CreatePropertyCondition(UIA_NamePropertyId, name, &rawName))) {
            VARIANT type{};
            type.vt = VT_I4;
            type.lVal = UIA_SliderControlTypeId;
            if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &rawType)))
                automation->CreateAndCondition(rawName, rawType, &rawBoth);
        }
        VariantClear(&name);
        ComPtr<IUIAutomationCondition> nameCondition(rawName), typeCondition(rawType), condition(rawBoth);
        if (condition) {
            WindowList list;
            EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&list));
            for (unsigned i = 0; i < list.count && !success; ++i)
                success = SetOnWindow(automation.get(), condition.get(), list.windows[i], percent, relative);
        }
    }
    CoUninitialize();
    return success;
}
