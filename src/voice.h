#pragma once
#include <windows.h>
#include <string>
#include <array>

namespace voice {
constexpr UINT CommandWindowMs = 7000;
enum Event : WPARAM { Ready, Wake, Finished, Speakers, Headphones, OpenMusic, PlayWave, PauseMusic,
                     MissingFiles, MicrophoneError, EngineError, ReplyError, NextTrack, LikeTrack, CurrentTrack, DiscordQuieter, CancelTimer, PreviousTrack, MusicLouder, MusicQuieter, DiscordLouder, Unrecognized, RemoveLike, MusicVolume = 100, MusicTimer = 300, Scenario = 500, DiscordVolume = 600 };
constexpr unsigned ScenarioCount = 5;
enum ScenarioAction : unsigned { Headset = 1, Pause = 2, Discord = 4, Speaker = 8 };
struct Profile { std::wstring phrase; unsigned actions = 0; };
using Profiles = std::array<Profile, ScenarioCount>;
Profiles LoadProfiles();
bool SaveProfile(unsigned slot, const Profile& profile);
bool Start(HWND owner, UINT message);
void Stop();
bool Running();
bool CurrentEvent(LPARAM session);
void Reply(const std::wstring& text, int volume);
}
