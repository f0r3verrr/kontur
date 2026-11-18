#pragma once

namespace music_volume {
// Changes the desktop Yandex Music player's own slider. Returns false when it is unavailable.
bool Set(int percent, bool relative);
}
