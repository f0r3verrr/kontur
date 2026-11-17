#pragma once
#include <windows.h>
#include <string>

namespace voice {
enum Event : WPARAM { Ready, Wake, Finished, Speakers, Headphones, MissingFiles, MicrophoneError, EngineError, ReplyError };
bool Start(HWND owner, UINT message);
void Stop();
bool Running();
bool CurrentEvent(LPARAM session);
void Reply(const std::wstring& text);
}
