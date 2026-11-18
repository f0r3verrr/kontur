#include "voice.h"
#include "neural_reply.h"

#include <mmsystem.h>
#include <sapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

namespace {
constexpr int kRate = 16000;
constexpr int kSamples = kRate / 10;
constexpr int kPreRoll = 12;
constexpr ULONGLONG kCommandTime = 5000;
constexpr std::string_view kWakeWords[]{u8"контур"};
struct Command { std::string_view phrase; voice::Event event; };
constexpr Command kCommands[]{
    {u8"переключи звук на колонки", voice::Speakers},
    {u8"переключи на колонки", voice::Speakers},
    {u8"переключи звук на наушники", voice::Headphones},
    {u8"переключи на наушники", voice::Headphones},
    {u8"включи яндекс музыку", voice::OpenMusic},
    {u8"включи яндекс музыку и волну", voice::PlayWave},
    {u8"включи яндекс музыку и мою волну", voice::PlayWave},
    {u8"выключи музыку", voice::PauseMusic},
};

std::string Grammar(bool wakeOnly) {
    std::string grammar = R"(["[unk]")";
    auto add = [&](std::string_view phrase) { grammar += ",\""; grammar += phrase; grammar += "\""; };
    for (const auto& command : kCommands) {
        add(command.phrase);
        if (!wakeOnly)
            for (auto word : kWakeWords) add(std::string(word) + " " + std::string(command.phrase));
    }
    if (wakeOnly) {
        for (auto word : kWakeWords) add(word);
        // Negative examples reduce the chance of forcing unrelated speech to a wake word.
        for (auto word : {u8"который", u8"конкурс", u8"контроль", u8"контора"}) add(word);
    }
    grammar += "]";
    return grammar;
}

size_t WakePrefix(std::string_view text) {
    for (auto word : kWakeWords)
        if (text.size() > word.size() && text.substr(0, word.size()) == word && text[word.size()] == ' ')
            return word.size() + 1;
    return 0;
}

using Samples = std::array<short, kSamples>;
HANDLE g_stop, g_thread, g_reply;
HWND g_owner;
UINT g_message;
LPARAM g_session;
std::atomic<bool> g_running;
std::atomic<bool> g_speaking;
std::atomic<ULONGLONG> g_mutedUntil;

void Report(voice::Event event) { PostMessageW(g_owner, g_message, event, g_session); }

bool RecognitionPaused(ULONGLONG now) { return g_speaking || now < g_mutedUntil; }

std::string JsonText(const char* json, const char* key) {
    if (!json) return {};
    const std::string value(json);
    const auto field = value.find(key);
    if (field == std::string::npos) return {};
    const auto colon = value.find(':', field);
    if (colon == std::string::npos) return {};
    const auto begin = value.find('"', colon);
    if (begin == std::string::npos) return {};
    const auto end = value.find('"', begin + 1);
    return end == std::string::npos ? std::string{} : value.substr(begin + 1, end - begin - 1);
}

voice::Event ParseCommand(std::string_view text) {
    text.remove_prefix(WakePrefix(text));
    for (const auto& command : kCommands)
        if (text == command.phrase) return command.event;
    return voice::EngineError;
}

struct Engine {
    HMODULE dll = nullptr;
    void* (*modelNew)(const char*) = nullptr;
    void (*modelFree)(void*) = nullptr;
    void* (*recognizerNew)(void*, float, const char*) = nullptr;
    void (*recognizerFree)(void*) = nullptr;
    int (*accept)(void*, const char*, int) = nullptr;
    const char* (*result)(void*) = nullptr;
    const char* (*partial)(void*) = nullptr;
    const char* (*finalResult)(void*) = nullptr;
    void (*reset)(void*) = nullptr;
    void (*logLevel)(int) = nullptr;

    template<class T> bool Load(T& fn, const char* name) {
        fn = reinterpret_cast<T>(GetProcAddress(dll, name));
        return fn != nullptr;
    }
    bool Open(const std::wstring& path) {
        dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dll && Load(modelNew, "vosk_model_new") && Load(modelFree, "vosk_model_free") &&
               Load(recognizerNew, "vosk_recognizer_new_grm") && Load(recognizerFree, "vosk_recognizer_free") &&
               Load(accept, "vosk_recognizer_accept_waveform") && Load(result, "vosk_recognizer_result") &&
               Load(partial, "vosk_recognizer_partial_result") && Load(finalResult, "vosk_recognizer_final_result") &&
               Load(reset, "vosk_recognizer_reset") && Load(logLevel, "vosk_set_log_level");
    }
    ~Engine() { if (dll) FreeLibrary(dll); }
};

std::wstring VoiceDirectory() {
    wchar_t path[32768]{};
    DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!n || n >= ARRAYSIZE(path)) return {};
    const std::wstring exe(path, n);
    return exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"voice\\";
}

std::string Utf8(const std::wstring& text) {
    const auto count = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), count, nullptr, nullptr);
    return result;
}

struct Capture {
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEIN mic = nullptr;
    std::array<Samples, 4> samples{};
    std::array<WAVEHDR, 4> headers{};
    size_t prepared = 0;

    bool Open() {
        WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, kRate, kRate * 2, 2, 16, 0};
        if (!ready || waveInOpen(&mic, WAVE_MAPPER, &format, (DWORD_PTR)ready, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
            return false;
        for (size_t i = 0; i < headers.size(); ++i) {
            auto& hdr = headers[i];
            hdr.lpData = reinterpret_cast<char*>(samples[i].data());
            hdr.dwBufferLength = sizeof(Samples);
            if (waveInPrepareHeader(mic, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) return false;
            ++prepared;
            if (waveInAddBuffer(mic, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) return false;
        }
        return waveInStart(mic) == MMSYSERR_NOERROR;
    }
    ~Capture() {
        if (mic) {
            waveInReset(mic);
            for (size_t i = 0; i < prepared; ++i) waveInUnprepareHeader(mic, &headers[i], sizeof(WAVEHDR));
            waveInClose(mic);
        }
        for (auto& data : samples) SecureZeroMemory(data.data(), sizeof(data));
        if (ready) CloseHandle(ready);
    }
};

struct Decoder {
    using Recognizer = std::unique_ptr<void, decltype(Engine::recognizerFree)>;
    struct Block { Samples samples; int bytes; };
    Engine& engine;
    void* model;
    void (*report)(voice::Event);
    Recognizer wake, command;
    std::deque<Block> recent;
    ULONGLONG deadline = 0;
    ULONGLONG audibleUntil = 0;
    int consecutive = 0;

    Decoder(Engine& api, void* loadedModel, void (*notify)(voice::Event))
        : engine(api), model(loadedModel), report(notify),
          wake(api.recognizerNew(model, kRate, Grammar(true).c_str()), api.recognizerFree),
          command(nullptr, api.recognizerFree) {}
    ~Decoder() { Clear(); }

    void Clear() {
        for (auto& block : recent) SecureZeroMemory(block.samples.data(), sizeof(Samples));
        recent.clear();
    }

    bool Submit(const char* json, bool replay = false) {
        auto text = JsonText(json, "\"text\"");
        if (replay && !WakePrefix(text)) return false;
        const auto event = ParseCommand(text);
        if (event == voice::EngineError) return false;
        report(event);
        return true;
    }

    void Finish() {
        const bool wasListening = command != nullptr;
        command.reset();
        Clear();
        engine.reset(wake.get());
        if (wasListening) report(voice::Finished);
    }

    void Tick(ULONGLONG now) {
        if (command && now >= deadline) {
            Submit(engine.finalResult(command.get()));
            Finish();
        }
    }

    bool Feed(const Samples& samples, int bytes, ULONGLONG now) {
        const auto data = reinterpret_cast<const char*>(samples.data());
        if (command) {
            if (engine.accept(command.get(), data, bytes) > 0 && Submit(engine.result(command.get()))) Finish();
            return true;
        }
        for (int i = 0; i < bytes / (int)sizeof(short); ++i) {
            if (samples[i] < -120 || samples[i] > 120) {
                audibleUntil = now + 800;
                break;
            }
        }
        if (now >= audibleUntil) {
            if (!recent.empty()) Finish();
            consecutive = 0;
            return true;
        }
        if (recent.size() == kPreRoll) {
            SecureZeroMemory(recent.front().samples.data(), sizeof(Samples));
            recent.pop_front();
        }
        recent.push_back({samples, bytes});
        const bool ended = engine.accept(wake.get(), data, bytes) > 0;
        const auto text = ended ? JsonText(engine.result(wake.get()), "\"text\"") :
                                  JsonText(engine.partial(wake.get()), "\"partial\"");
        const bool wakeWord = std::find(std::begin(kWakeWords), std::end(kWakeWords), text) != std::end(kWakeWords);
        consecutive = wakeWord ? consecutive + 1 : 0;
        if (!wakeWord || (!ended && consecutive < 2)) return true;
        consecutive = 0;
        command.reset(engine.recognizerNew(model, kRate, Grammar(false).c_str()));
        if (!command) return false;
        deadline = now + kCommandTime;
        report(voice::Wake);
        bool complete = false;
        for (const auto& block : recent) {
            if (engine.accept(command.get(), reinterpret_cast<const char*>(block.samples.data()), block.bytes) > 0 &&
                Submit(engine.result(command.get()), true)) {
                complete = true;
                break;
            }
        }
        Clear();
        if (complete) Finish();
        return true;
    }
};

voice::Event Listen() {
    const auto dir = VoiceDirectory();
    const auto dllPath = dir + L"libvosk.dll", modelPath = dir + L"model";
    if (dir.empty() || GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((modelPath + L"\\am\\final.mdl").c_str()) == INVALID_FILE_ATTRIBUTES)
        return voice::MissingFiles;
    Engine engine;
    if (!engine.Open(dllPath)) return voice::EngineError;
    engine.logLevel(-1);
    wchar_t shortPath[32768]{};
    DWORD shortLength = GetShortPathNameW(modelPath.c_str(), shortPath, ARRAYSIZE(shortPath));
    const auto modelString = Utf8(shortLength && shortLength < ARRAYSIZE(shortPath) ? shortPath : modelPath);
    std::unique_ptr<void, decltype(engine.modelFree)> model(engine.modelNew(modelString.c_str()), engine.modelFree);
    if (!model) return voice::EngineError;
    Decoder decoder(engine, model.get(), Report);
    if (!decoder.wake) return voice::EngineError;
    if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) return voice::Ready;
    Capture capture;
    if (!capture.Open()) return voice::MicrophoneError;
    Report(voice::Ready);
    size_t next = 0;
    HANDLE events[]{g_stop, capture.ready};
    bool wasPaused = false;
    for (;;) {
        const auto wait = WaitForMultipleObjects(2, events, FALSE, decoder.command ? 100 : INFINITE);
        if (wait == WAIT_OBJECT_0) return voice::Ready;
        if (wait == WAIT_FAILED) return voice::MicrophoneError;
        const auto now = GetTickCount64();
        const bool paused = RecognitionPaused(now);
        if (paused && !wasPaused) decoder.Finish();
        if (!paused) decoder.Tick(now);
        wasPaused = paused;
        while (capture.headers[next].dwFlags & WHDR_DONE) {
            if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) return voice::Ready;
            auto& hdr = capture.headers[next];
            const auto bytes = (int)hdr.dwBytesRecorded;
            if (bytes && !paused && !decoder.Feed(capture.samples[next], bytes, now)) return voice::EngineError;
            SecureZeroMemory(hdr.lpData, hdr.dwBufferLength);
            if (waveInAddBuffer(capture.mic, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) return voice::MicrophoneError;
            next = (next + 1) % capture.headers.size();
        }
    }
}

DWORD WINAPI Worker(LPVOID) {
    const auto event = Listen();
    g_running = false;
    if (event != voice::Ready && WaitForSingleObject(g_stop, 0) == WAIT_TIMEOUT) Report(event);
    return 0;
}

void PreferRussian(ISpVoice* voice) {
    for (auto path : {L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens",
                      L"SOFTWARE\\Microsoft\\Speech_OneCore\\Voices\\Tokens"}) {
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &root) != ERROR_SUCCESS) continue;
        bool selected = false;
        for (DWORD i = 0; !selected; ++i) {
            wchar_t name[256]{};
            DWORD length = ARRAYSIZE(name);
            if (RegEnumKeyExW(root, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            const auto attributes = std::wstring(name) + L"\\Attributes";
            wchar_t language[64]{};
            DWORD bytes = sizeof(language);
            if (RegGetValueW(root, attributes.c_str(), L"Language", RRF_RT_REG_SZ, nullptr, language, &bytes) !=
                ERROR_SUCCESS || wcstoul(language, nullptr, 16) != 0x419) continue;
            ISpObjectToken* token = nullptr;
            if (FAILED(CoCreateInstance(__uuidof(SpObjectToken), nullptr, CLSCTX_INPROC_SERVER,
                                        __uuidof(ISpObjectToken), (void**)&token))) break;
            const auto id = std::wstring(L"HKEY_LOCAL_MACHINE\\") + path + L"\\" + name;
            selected = SUCCEEDED(token->SetId(nullptr, id.c_str(), FALSE)) && SUCCEEDED(voice->SetVoice(token));
            token->Release();
        }
        RegCloseKey(root);
        if (selected) return;
    }
}

bool SpeakWithWindows(const std::wstring& text, int volume) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ISpVoice* speaker = nullptr;
    bool success = false;
    if (SUCCEEDED(initialized)) {
        if (SUCCEEDED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_INPROC_SERVER,
                                       __uuidof(ISpVoice), (void**)&speaker))) {
            PreferRussian(speaker);
            speaker->SetVolume((USHORT)volume);
            if (WaitForSingleObject(g_stop, 0) == WAIT_TIMEOUT &&
                SUCCEEDED(speaker->Speak(text.c_str(), SPF_ASYNC | SPF_IS_NOT_XML, nullptr))) {
                const auto deadline = GetTickCount64() + 12000;
                HRESULT result;
                do {
                    result = speaker->WaitUntilDone(100);
                } while (result == S_FALSE && GetTickCount64() < deadline &&
                         WaitForSingleObject(g_stop, 0) == WAIT_TIMEOUT);
                success = result == S_OK;
                if (!success) speaker->Speak(nullptr, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
            }
            speaker->Release();
        }
        CoUninitialize();
    }
    return success;
}

struct ReplyRequest { std::wstring text; int volume; };

DWORD WINAPI Speak(LPVOID payload) {
    std::unique_ptr<ReplyRequest> request(static_cast<ReplyRequest*>(payload));
    const auto neural = neural_reply::Speak(VoiceDirectory() + L"piper\\", Utf8(request->text), request->volume, g_stop);
    const bool success = neural == neural_reply::Result::Played ||
                         (neural == neural_reply::Result::Unavailable && SpeakWithWindows(request->text, request->volume));
    g_mutedUntil = GetTickCount64() + 500;
    g_speaking = false;
    if (!success && WaitForSingleObject(g_stop, 0) == WAIT_TIMEOUT) Report(voice::ReplyError);
    return 0;
}
} // namespace

namespace voice {
bool Start(HWND owner, UINT message) {
    if (g_running) return true;
    Stop();
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) return false;
    g_owner = owner;
    g_message = message;
    ++g_session;
    g_running = true;
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_thread) Stop();
    return g_thread != nullptr;
}

void Stop() {
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
    }
    if (g_reply) {
        WaitForSingleObject(g_reply, INFINITE);
        CloseHandle(g_reply);
        g_reply = nullptr;
    }
    neural_reply::ClearCache();
    if (g_stop) CloseHandle(g_stop);
    g_thread = g_stop = nullptr;
    g_running = false;
    g_speaking = false;
    g_mutedUntil = 0;
    ++g_session;
}

bool Running() { return g_running; }
bool CurrentEvent(LPARAM session) { return session == g_session; }

void Reply(const std::wstring& text, int volume) {
    if (!g_running || text.empty() || volume <= 0) return;
    if (g_reply) {
        if (WaitForSingleObject(g_reply, 0) != WAIT_OBJECT_0) return;
        CloseHandle(g_reply);
    }
    g_speaking = true;
    auto* payload = new ReplyRequest{text, (std::min)(volume, 100)};
    g_reply = CreateThread(nullptr, 0, Speak, payload, 0, nullptr);
    if (!g_reply) {
        delete payload;
        g_speaking = false;
        Report(ReplyError);
    }
}
}
