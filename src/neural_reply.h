#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace neural_reply {
enum class Result { Unavailable, Failed, Played };
bool Synthesize(const std::wstring& folder, const std::string& text, HANDLE stop, std::vector<char>& pcm);
Result Speak(const std::wstring& folder, const std::string& text, int volume, HANDLE stop);
void ClearCache();
}
