#include "windows_actions.h"
#include <string>

bool StartWindowsAction(UINT resource, const wchar_t* setup) {
    const auto module = GetModuleHandleW(nullptr);
    const auto script = FindResourceW(module, MAKEINTRESOURCEW(resource), RT_RCDATA);
    if (!script) return false;
    const auto data = static_cast<const char*>(LockResource(LoadResource(module, script)));
    const int bytes = SizeofResource(module, script);
    if (!data || !bytes) return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, bytes, nullptr, 0);
    if (!length) return false;
    std::wstring program(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, bytes, program.data(), length);
    program.insert(0, setup);
    constexpr wchar_t alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* raw = reinterpret_cast<const unsigned char*>(program.data());
    const size_t size = program.size() * sizeof(wchar_t);
    std::wstring command = L"powershell.exe -NoProfile -NonInteractive -EncodedCommand ";
    for (size_t i = 0; i < size; i += 3) {
        const unsigned a = raw[i], b = i + 1 < size ? raw[i + 1] : 0, c = i + 2 < size ? raw[i + 2] : 0;
        command += alphabet[a >> 2];
        command += alphabet[((a & 3) << 4) | (b >> 4)];
        command += i + 1 < size ? alphabet[((b & 15) << 2) | (c >> 6)] : L'=';
        command += i + 2 < size ? alphabet[c & 63] : L'=';
    }
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, ARRAYSIZE(system))) return false;
    const std::wstring exe = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child)) return false;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return true;
}
