#include "audio.h"

#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <memory>

namespace {

// Windows exposes enumeration and notifications through Core Audio. Switching the
// default uses the Windows IPolicyConfig ABI, which is not part of the public SDK.
constexpr GUID kPolicyClass{0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
constexpr GUID kPolicyInterface{0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
struct PolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
};

struct ReleaseCom {
    void operator()(IUnknown* p) const { p->Release(); }
};
template<class T> using ComPtr = std::unique_ptr<T, ReleaseCom>;

class Notifications final : public IMMNotificationClient {
    std::atomic<ULONG> refs_{1};
    HWND owner_;
    UINT message_;
    HRESULT Notify() const {
        PostMessageW(owner_, message_, 0, 0);
        return S_OK;
    }
public:
    Notifications(HWND owner, UINT message) : owner_(owner), message_(message) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMMNotificationClient)) return E_NOINTERFACE;
        *object = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto remaining = --refs_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return Notify(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Notify(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return Notify(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        return flow == eRender && role == eMultimedia ? Notify() : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
        return IsEqualPropertyKey(key, PKEY_Device_FriendlyName) ? Notify() : S_OK;
    }
};

constexpr wchar_t kSettings[] = L"Software\\WirelessBattery";
constexpr wchar_t kNames[] = L"Software\\WirelessBattery\\AudioNames";
constexpr const wchar_t* kSlots[]{L"AudioSpeakers", L"AudioHeadphones"};
ComPtr<IMMDeviceEnumerator> g_enumerator;
ComPtr<Notifications> g_notifications;
bool g_com;
std::vector<audio::Endpoint> g_endpoints;
std::array<std::wstring, 2> g_slots;
std::vector<std::wstring> g_extra;
std::wstring g_current;

std::wstring DeviceId(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

std::wstring DefaultId(ERole role) {
    IMMDevice* raw = nullptr;
    if (!g_enumerator || FAILED(g_enumerator->GetDefaultAudioEndpoint(eRender, role, &raw))) return {};
    ComPtr<IMMDevice> device(raw);
    return DeviceId(device.get());
}

void DetectPair() {
    std::vector<const audio::Endpoint*> speakers, blacksharks, headphones;
    for (const auto& endpoint : g_endpoints) {
        if (!endpoint.available) continue;
        std::wstring name = endpoint.name;
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return std::towlower(c); });
        // A software loopback cable can report the same form factor as physical speakers.
        if (name.find(L"virtual cable") != std::wstring::npos || name.find(L"vb-audio") != std::wstring::npos)
            continue;
        if (name.find(L"blackshark") != std::wstring::npos) blacksharks.push_back(&endpoint);
        else if (endpoint.form == 1) speakers.push_back(&endpoint);  // Core Audio Speakers
        else if (endpoint.form == 3 || endpoint.form == 5) headphones.push_back(&endpoint);
    }
    if (g_slots[1].empty()) {
        const auto& candidates = blacksharks.empty() ? headphones : blacksharks;
        if (candidates.size() == 1) audio::Assign(1, candidates.front()->id);
    }
    if (g_slots[0].empty() && speakers.size() == 1) audio::Assign(0, speakers.front()->id);
}

}  // namespace

namespace audio {

void Start(HWND owner, UINT message) {
    g_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IMMDeviceEnumerator* raw = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw)))) return;
    g_enumerator.reset(raw);
    if (owner) {
        g_notifications.reset(new Notifications(owner, message));
        if (FAILED(g_enumerator->RegisterEndpointNotificationCallback(g_notifications.get())))
            g_notifications.reset();
    }
    for (int i = 0; i < 2; ++i) {
        wchar_t buffer[512]{};
        DWORD bytes = sizeof(buffer);
        if (RegGetValueW(HKEY_CURRENT_USER, kSettings, kSlots[i], RRF_RT_REG_SZ,
                         nullptr, buffer, &bytes) == ERROR_SUCCESS) g_slots[i] = buffer;
    }
    wchar_t saved[8192]{};
    DWORD bytes = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettings, L"AudioExtraOutputs", RRF_RT_REG_SZ,
                     nullptr, saved, &bytes) == ERROR_SUCCESS) {
        std::wstring list(saved);
        for (size_t pos = 0; pos < list.size();) {
            const auto end = list.find(L'\n', pos);
            g_extra.push_back(list.substr(pos, end - pos));
            if (end == std::wstring::npos) break;
            pos = end + 1;
        }
    }
    Refresh();
}

void Stop() {
    if (g_notifications) g_enumerator->UnregisterEndpointNotificationCallback(g_notifications.get());
    g_notifications.reset();
    g_enumerator.reset();
    if (g_com) CoUninitialize();
    g_com = false;
}

void Refresh() {
    g_current = DefaultId(eMultimedia);
    g_endpoints.clear();
    IMMDeviceCollection* raw = nullptr;
    if (!g_enumerator || FAILED(g_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &raw))) return;
    ComPtr<IMMDeviceCollection> devices(raw);
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* rawDevice = nullptr;
        if (FAILED(devices->Item(i, &rawDevice))) continue;
        ComPtr<IMMDevice> device(rawDevice);
        IPropertyStore* rawProperties = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &rawProperties))) continue;
        ComPtr<IPropertyStore> properties(rawProperties);
        Endpoint endpoint;
        endpoint.id = DeviceId(device.get());
        DWORD state = 0;
        device->GetState(&state);
        endpoint.available = state == DEVICE_STATE_ACTIVE;
        PROPVARIANT value{};
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
            endpoint.name = value.pwszVal;
        PropVariantClear(&value);
        if (SUCCEEDED(properties->GetValue(PKEY_AudioEndpoint_FormFactor, &value)) && value.vt == VT_UI4)
            endpoint.form = value.ulVal;
        PropVariantClear(&value);
        if (!endpoint.id.empty() && !endpoint.name.empty()) {
            wchar_t alias[128]{};
            DWORD bytes = sizeof(alias);
            if (RegGetValueW(HKEY_CURRENT_USER, kNames, endpoint.id.c_str(), RRF_RT_REG_SZ,
                             nullptr, alias, &bytes) == ERROR_SUCCESS) endpoint.alias = alias;
            g_endpoints.push_back(std::move(endpoint));
        }
    }
    DetectPair();
}

const std::vector<Endpoint>& Endpoints() { return g_endpoints; }
const std::wstring& Current() { return g_current; }
const std::wstring& Configured(int slot) { return g_slots[slot]; }

const Endpoint* Find(const std::wstring& id) {
    auto found = std::find_if(g_endpoints.begin(), g_endpoints.end(), [&](const Endpoint& e) { return e.id == id; });
    return found == g_endpoints.end() ? nullptr : &*found;
}

bool Assign(int slot, const std::wstring& id) {
    if (id.empty() || id == g_slots[1 - slot]) return false;
    const DWORD bytes = (DWORD)((id.size() + 1) * sizeof(wchar_t));
    if (RegSetKeyValueW(HKEY_CURRENT_USER, kSettings, kSlots[slot], REG_SZ, id.c_str(), bytes) != ERROR_SUCCESS)
        return false;
    g_slots[slot] = id;
    return true;
}

const std::vector<std::wstring>& ExtraOutputs() { return g_extra; }

bool Rename(const std::wstring& id, const std::wstring& name) {
    if (id.empty() || name.size() >= 128) return false;
    if (RegSetKeyValueW(HKEY_CURRENT_USER, kNames, id.c_str(), REG_SZ, name.c_str(),
                        (DWORD)((name.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) return false;
    for (auto& endpoint : g_endpoints)
        if (endpoint.id == id) endpoint.alias = name;
    return true;
}

bool SaveExtra(const std::wstring& id, bool include) {
    auto next = g_extra;
    const auto found = std::find(next.begin(), next.end(), id);
    if (include && found == next.end()) next.push_back(id);
    if (!include && found != next.end()) next.erase(found);
    std::wstring data;
    for (const auto& entry : next) data += entry + L"\n";
    if (data.size() >= 8192) return false;
    const DWORD bytes = (DWORD)((data.size() + 1) * sizeof(wchar_t));
    if (RegSetKeyValueW(HKEY_CURRENT_USER, kSettings, L"AudioExtraOutputs", REG_SZ, data.c_str(), bytes) != ERROR_SUCCESS)
        return false;
    g_extra = std::move(next);
    return true;
}

std::vector<std::wstring> Cycle() {
    std::vector<std::wstring> result;
    auto append = [&](const std::wstring& id) {
        const auto* endpoint = Find(id);
        if (endpoint && endpoint->available && std::find(result.begin(), result.end(), id) == result.end())
            result.push_back(id);
    };
    for (const auto& id : g_slots) append(id);
    for (const auto& id : g_extra) append(id);
    return result;
}

std::wstring SwitchTo(const std::wstring& id) {
    if (id.empty()) return L"Сначала выбери два выхода в настройках";
    const auto* target = Find(id);
    if (!target || !target->available) return L"Выбранный выход сейчас недоступен";
    PolicyConfig* raw = nullptr;
    if (FAILED(CoCreateInstance(kPolicyClass, nullptr, CLSCTX_ALL, kPolicyInterface,
                                reinterpret_cast<void**>(&raw)))) return L"Windows не разрешила сменить выход";
    ComPtr<PolicyConfig> policy(raw);
    constexpr ERole roles[]{ eConsole, eMultimedia, eCommunications };
    const std::wstring previous[]{DefaultId(roles[0]), DefaultId(roles[1]), DefaultId(roles[2])};
    for (int i = 0; i < 3; ++i) {
        if (id == previous[i]) continue;
        if (FAILED(policy->SetDefaultEndpoint(id.c_str(), roles[i]))) {
            for (int j = 0; j < i; ++j)
                if (!previous[j].empty() && previous[j] != id)
                    policy->SetDefaultEndpoint(previous[j].c_str(), roles[j]);
            return L"Windows не смогла сменить выход";
        }
    }
    g_current = DefaultId(eMultimedia);
    return g_current == id ? L"" : L"Windows не подтвердила смену выхода";
}

}  // namespace audio
