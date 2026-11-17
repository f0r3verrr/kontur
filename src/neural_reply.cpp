#include "neural_reply.h"
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

namespace {
constexpr DWORD kRate = 22050;
constexpr size_t kMaxPcm = kRate * 2 * 20;
constexpr wchar_t kModel[] = L"ru_RU-denis-medium.onnx";
std::string g_lastText;
std::vector<char> g_lastPcm;

struct Close {
    void operator()(void* handle) const { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, Close>;

bool MakePipe(Handle& read, Handle& write, bool childReads) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE input = nullptr, output = nullptr;
    if (!CreatePipe(&input, &output, &security, 0)) return false;
    read.reset(input);
    write.reset(output);
    return SetHandleInformation(childReads ? output : input, HANDLE_FLAG_INHERIT, 0) != FALSE;
}

bool Play(std::vector<char>& pcm, HANDLE stop) {
    if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT) return false;
    Handle done(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!done) return false;
    WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, kRate, kRate * 2, 2, 16, 0};
    HWAVEOUT output = nullptr;
    if (waveOutOpen(&output, WAVE_MAPPER, &format, (DWORD_PTR)done.get(), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
        return false;
    WAVEHDR header{};
    header.lpData = pcm.data();
    header.dwBufferLength = (DWORD)pcm.size();
    bool played = false;
    if (waveOutPrepareHeader(output, &header, sizeof(header)) == MMSYSERR_NOERROR) {
        if (waveOutWrite(output, &header, sizeof(header)) == MMSYSERR_NOERROR) {
            HANDLE events[]{stop, done.get()};
            const auto deadline = GetTickCount64() + 22000;
            while (!(header.dwFlags & WHDR_DONE) && GetTickCount64() < deadline) {
                const auto wait = WaitForMultipleObjects(2, events, FALSE, 250);
                if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
            }
            played = (header.dwFlags & WHDR_DONE) != 0;
            if (!played) waveOutReset(output);
        }
        waveOutUnprepareHeader(output, &header, sizeof(header));
    }
    waveOutClose(output);
    return played;
}

void ApplyGain(std::vector<char>& pcm, int volume) {
    for (size_t offset = 0; offset + sizeof(int16_t) <= pcm.size(); offset += sizeof(int16_t)) {
        int16_t sample;
        std::memcpy(&sample, pcm.data() + offset, sizeof(sample));
        sample = static_cast<int16_t>(sample * volume / 100);
        std::memcpy(pcm.data() + offset, &sample, sizeof(sample));
    }
}
}

namespace neural_reply {
bool Synthesize(const std::wstring& folder, const std::string& text, HANDLE stop, std::vector<char>& pcm) {
    pcm.clear();
    if (text.empty() || text.size() > 512 || WaitForSingleObject(stop, 0) != WAIT_TIMEOUT) return false;
    Handle childIn, input, output, childOut;
    if (!MakePipe(childIn, input, true) || !MakePipe(output, childOut, false)) return false;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle errors(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    if (errors.get() == INVALID_HANDLE_VALUE) return false;
    const auto exe = folder + L"piper.exe";
    auto command = L"\"" + exe + L"\" --model " + kModel + L" --output_raw";
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childIn.get();
    startup.StartupInfo.hStdOutput = childOut.get();
    startup.StartupInfo.hStdError = errors.get();
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (!size) return false;
    std::vector<BYTE> storage(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) return false;
    HANDLE inherited[]{childIn.get(), childOut.get(), errors.get()};
    PROCESS_INFORMATION child{};
    const bool created = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr) &&
        CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                       nullptr, folder.c_str(), &startup.StartupInfo, &child);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) return false;
    Handle process(child.hProcess), thread(child.hThread);
    childIn.reset();
    childOut.reset();
    errors.reset();
    auto line = text + "\n";
    DWORD written = 0;
    const bool sent = WriteFile(input.get(), line.data(), (DWORD)line.size(), &written, nullptr) && written == line.size();
    input.reset();
    const auto deadline = GetTickCount64() + 15000;
    if (sent) {
        std::array<char, 8192> buffer{};
        while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
            DWORD available = 0;
            if (PeekNamedPipe(output.get(), nullptr, 0, nullptr, &available, nullptr) && available) {
                DWORD bytes = 0;
                if (!ReadFile(output.get(), buffer.data(), (std::min)(available, (DWORD)buffer.size()), &bytes, nullptr) ||
                    pcm.size() + bytes > kMaxPcm) break;
                pcm.insert(pcm.end(), buffer.begin(), buffer.begin() + bytes);
            } else if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
                DWORD code = 1;
                return GetExitCodeProcess(process.get(), &code) && code == 0 && !pcm.empty() && pcm.size() % 2 == 0;
            } else {
                WaitForSingleObject(stop, 15);
            }
        }
    }
    if (WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 1000);
    }
    return false;
}

Result Speak(const std::wstring& folder, const std::string& text, int volume, HANDLE stop) {
    volume = std::clamp(volume, 0, 100);
    if (volume == 0) return Result::Played;
    if (GetFileAttributesW((folder + L"piper.exe").c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((folder + kModel).c_str()) == INVALID_FILE_ATTRIBUTES)
        return Result::Unavailable;
    if (g_lastText != text || g_lastPcm.empty()) {
        std::vector<char> pcm;
        if (!Synthesize(folder, text, stop, pcm)) return Result::Failed;
        g_lastText = text;
        g_lastPcm = std::move(pcm);
    }
    if (volume == 100) return Play(g_lastPcm, stop) ? Result::Played : Result::Failed;
    auto attenuated = g_lastPcm;
    ApplyGain(attenuated, volume);
    return Play(attenuated, stop) ? Result::Played : Result::Failed;
}

void ClearCache() {
    g_lastText.clear();
    std::vector<char>().swap(g_lastPcm);
}
}
