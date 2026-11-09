#pragma once

namespace moderngekko {
// Host-platform hotkey hook. The platform key handlers (DolphinNoGUI
// PlatformWayland / PlatformX11) call NotifyHotkey() before their own
// built-in hotkeys, so a registered callback can claim a key first.
//
// `keysym` is an X11/xkb keysym: XKB_KEY_F1 == XK_F1 == 0xFFBE on both
// platforms, so one value works everywhere.
//
// Returns true when the key was consumed and the platform must not run
// its own hotkey handling for it.
using HotkeyCallback = bool (*)(void *user_data, unsigned keysym);

void SetHotkeyCallback(HotkeyCallback callback, void *user_data);
bool NotifyHotkey(unsigned keysym);
} // namespace moderngekko
