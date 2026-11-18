#include "windows_actions.h"
#include <string>
#include <memory>

namespace {
struct ActionResult { HANDLE process; HWND window; UINT message; HANDLE pipe; unsigned context; };
DWORD WINAPI WaitForAction(void* context) {
    std::unique_ptr<ActionResult> result(static_cast<ActionResult*>(context));
    DWORD code = 3;
    std::unique_ptr<std::wstring> output;
    if (result->pipe) {
        output = std::make_unique<std::wstring>();
        std::string bytes;
        char block[512];
        DWORD read = 0;
        while (ReadFile(result->pipe, block, sizeof(block), &read, nullptr) && read)
            if (bytes.size() + read <= 8192) bytes.append(block, read);
        CloseHandle(result->pipe);
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
        if (count > 0) {
            output->resize(count);
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), output->data(), count);
        }
    }
    if (WaitForSingleObject(result->process, INFINITE) == WAIT_OBJECT_0)
        GetExitCodeProcess(result->process, &code);
    CloseHandle(result->process);
    const auto payload = reinterpret_cast<LPARAM>(output.get());
    if (PostMessageW(result->window, result->message, code | (WPARAM(result->context) << 32), payload) && payload)
        output.release();
    return 0;
}
}

bool StartWindowsAction(UINT resource, const wchar_t* setup, HWND notify, UINT message, bool capture, unsigned context) {
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
    HANDLE readPipe = nullptr, writePipe = nullptr, nullInput = nullptr;
    if (capture && notify) {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        if (!CreatePipe(&readPipe, &writePipe, &inherit, 0)) return false;
        nullInput = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &inherit, OPEN_EXISTING, 0, nullptr);
        if (nullInput == INVALID_HANDLE_VALUE || !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
            if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return false;
        }
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = nullInput;
        startup.hStdOutput = writePipe;
        startup.hStdError = nullInput;
    }
    const BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, writePipe != nullptr,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    if (writePipe) {
        CloseHandle(writePipe);
        CloseHandle(nullInput);
    }
    if (!started) {
        if (readPipe) CloseHandle(readPipe);
        return false;
    }
    CloseHandle(child.hThread);
    if (notify) {
        auto result = std::make_unique<ActionResult>(ActionResult{child.hProcess, notify, message, readPipe, context});
        if (QueueUserWorkItem(WaitForAction, result.get(), WT_EXECUTELONGFUNCTION)) {
            result.release();
            return true;
        }
        CloseHandle(child.hProcess);
        if (readPipe) CloseHandle(readPipe);
        return false;
    }
    CloseHandle(child.hProcess);
    return true;
}
