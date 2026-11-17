#pragma once
#include <windows.h>

namespace voice {
enum Event : WPARAM { Ready, Wake, Speakers, Headphones, MissingFiles, MicrophoneError, EngineError };
bool Start(HWND owner, UINT message);
void Stop();
bool Running();
bool CurrentEvent(LPARAM session);
}
