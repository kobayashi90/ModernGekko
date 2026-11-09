#pragma once

namespace moderngekko {
// Live window state control for the settings menu.
//
// The platform layer owns the toplevel, and on Wayland a proxy request must be
// issued from that platform's own event thread, so these only record a request
// that the platform applies on its next loop iteration. Both are safe to call
// from any thread; calling them with no window (headless, or before boot) is a
// no-op.
void SetWindowFullscreen(bool fullscreen);
bool IsWindowFullscreen();
} // namespace moderngekko
