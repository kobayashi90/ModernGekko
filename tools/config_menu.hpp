#pragma once

#include "frontend_config.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace moderngekko {
class Runtime;
}

namespace moderngekko::frontend {

// One row of the Cheats tab. The code body is deliberately not copied here:
// Dolphin's own GameSettings/<game id>.ini holds the codes, and every toggle is
// written back to it and re-loaded through the same path the core uses at
// boot. That keeps one source of truth instead of two copies that can drift.
struct CheatListEntry {
  std::string name;
  bool action_replay = false;
  bool enabled = false;
};

// F1 settings overlay drawn INSIDE the game window. Opening it pauses
// emulation; closing it resumes and persists config.ini. Display/input/audio
// settings are persisted through the normal frontend config (config.ini +
// GCPadNew.ini).
//
// Threading: there is no menu window any more. The menu draws through a hook
// registered on VideoCommon::OnScreenUI, which invokes it on the video thread
// inside OnScreenUI::Finalize() with the OSD's ImGui context current and the
// ImGui lock held — so the menu renders in the same frame as the OSD, in the
// game's backbuffer. Keyboard/pointer events arrive from the platform layer
// (PlatformWayland) through Presenter::SetKeysym/SetMousePos/... into that
// same ImGui context. While the menu is visible emulation is paused and no
// frames are produced, so the menu thread drives the presenter itself
// (Presenter::PresentOverlayFrame) at ~60 Hz to keep the overlay on screen.
class ConfigMenu {
public:
  struct Hooks {
    Runtime *runtime = nullptr;
    std::filesystem::path user_directory;
    ConfigResult *config = nullptr;
    std::string game_name;
    std::string disc_id;
    std::string module_path;
    // Where the disc that is running was extracted from, and where the
    // executable lives. The System tab's region combo needs both: the first to
    // find the sibling extractions of the other regions, the second to find a
    // module sitting next to the binary (the installed/AppImage layout).
    std::filesystem::path game_root;
    std::filesystem::path executable_directory;
    // False on Wayland: this build cannot create an OpenGL context on a
    // Wayland surface, so the Display tab must not offer OGL there.
    bool allow_ogl = true;
  };

  ConfigMenu() = default;
  ~ConfigMenu();

  ConfigMenu(const ConfigMenu &) = delete;
  ConfigMenu &operator=(const ConfigMenu &) = delete;

  // Registers the keyboard/gamepad binding providers, the platform hotkey
  // hook (F1) and the in-game overlay draw hook, then starts the overlay
  // thread. ControllerInterface must already be initialized.
  void Start(Hooks hooks);
  // Shuts everything down. Call after Runtime::Run() returns; safe to call
  // twice.
  void Stop();

  bool WantsRestart() const { return m_restart.load(); }

  // Set when the System tab's region combo picked a different disc. RunMain
  // rewrites --game/--module from these before replaying argv through execv,
  // so a region switch rides the same restart path as "Save && restart".
  bool WantsRegionSwitch() const { return m_region_switch.load(); }
  std::string RegionSwitchGameRoot() const;
  std::string RegionSwitchModule() const;

private:
  static bool HotkeyThunk(void *userdata, unsigned keysym);

  void Show();
  void Hide();
  void ThreadMain();
  // Runs on the video thread inside OnScreenUI::Finalize (OSD ImGui context
  // current, ImGui lock held).
  void DrawOverlayFrame();
  void Draw();
  void DrawDisplayTab();
  void DrawInputTab();
  void DrawAudioTab();
  void DrawTexturesTab();
  void DrawStatesTab();
  void DrawCheatsTab();
  void DrawSystemTab();
  // One row of the System tab's region combo: the disc id, its human label
  // ("NTSC-U"), and where its game root and recompiled module live. Either
  // path may be empty, which is what makes an uninstalled region a disabled
  // row instead of a selection that would boot-fail.
  struct RegionChoice {
    std::string disc_id;
    std::string label;
    std::filesystem::path game_root;
    std::filesystem::path module;
  };
  // Scans for the other regions of the running title once, on first draw.
  void RefreshRegions();

  // One extracted game version on disk. Unlike RegionChoice (which is derived
  // from the running disc id and therefore covers exactly the regions this
  // title shipped in), this is whatever the user actually has extracted: a
  // debug build, a different revision, or a region the catalogue does not
  // predict.
  struct GameVersionEntry {
    std::string disc_id;
    std::string name;
    std::filesystem::path game_root;
    std::filesystem::path module;
  };
  void ScanVersions();
  void ScanInstallImages();
  // Runs the launcher's `--extract <image>` mode in a detached background
  // thread. The runner does not link Dolphin's disc reader, the launcher does,
  // so this is the only way to add a version without leaving the game. State
  // lives in a shared_ptr because the thread outlives a quit request.
  void InstallVersion(const std::filesystem::path &image);
  void RefreshDevices();
  void Save(const std::string &what);
  void SetStatus(std::string text);


  // Live application. The core is paused while the menu is open, so these only
  // stage changes: Config::SetCurrent() writes the config layer, and the core
  // picks the values up in CheckForConfigChanges() (driven by after_frame_event)
  // on the first emulated frame after the menu closes. Pad::LoadConfig() is the
  // exception -- it re-binds input immediately, which is safe while paused.
  void ApplyDisplayLive();
  void ApplyControllerLive();
  void ApplyAudioBackendLive();
  void ApplyTexturesLive();
  void RescanDevices();
  // Cheats: re-reads GameSettings/<id>.ini for the tab's list, and re-loads +
  // re-applies the enabled codes through Core/Cheats. Both are safe while the
  // menu is open (the core is paused); the code handler is installed lazily by
  // the next emulated frame, not here.
  void RefreshCheats();
  void ApplyCheatsLive();
  // Throttled report of what the RUNNING core actually sees, so a test can
  // prove a setting landed without trusting the menu's own copy of it.
  void ReportLiveState();

  // Actions that need the CORE, not the config layer: savestates, reset and
  // screenshots. They are executed on the overlay thread (a host thread, not
  // the CPU thread and not the video thread), after the menu has closed and
  // resumed emulation — exactly the thread and timing Dolphin's own host
  // hotkeys use. Doing this from the ImGui callback instead would mean
  // PauseAndLock() from inside Present with the core already paused by the
  // menu, which is a deadlock shape nobody needs.
  enum class HostAction { None, SaveState, LoadState, Reset, Screenshot };
  void QueueHostAction(HostAction action);
  void RunPendingHostActions();

  // HD texture pack bookkeeping (video thread only).
  void RefreshTexturePacks();
  std::string TextureDirectory() const;

  // --- Presentation -------------------------------------------------------
  // Dolphin's OSD draws yellow text in a black box at the top-left and looks
  // like a debug build. Everything the menu triggers therefore goes through a
  // filter installed on VideoCommon::OSD: the stock string is swallowed and a
  // toast in this frontend's own style is queued instead, drawn from the
  // overlay hook (so it works with the menu closed, which is when savestates
  // and screenshots actually run).
  bool FilterOsdMessage(std::string &message);
  void PushToast(std::string text);
  void DrawToasts();
  // Same idea for the frame counter: GFX_SHOW_FPS renders Dolphin's own
  // performance overlay, so the menu draws its own pill instead.
  void DrawFrameCounter();

  Hooks m_hooks;
  Runtime *m_runtime = nullptr;

  std::atomic<bool> m_quit{false};
  std::atomic<bool> m_visible{false};
  std::atomic<bool> m_restart{false};
  // Region switch. The strings are written on the video thread and read by
  // RunMain after Run() returns, so they are guarded; the flag is the
  // release/acquire edge, exactly like m_restart.
  std::atomic<bool> m_region_switch{false};
  mutable std::mutex m_region_mutex;
  std::string m_region_game_root;
  std::string m_region_module;
  // Set by Hide(); consumed by DrawOverlayFrame() on the video thread so the
  // config save happens with no other thread touching ConfigResult.
  std::atomic<bool> m_save_pending{false};
  // Set by Show(); consumed once by DrawOverlayFrame() to give the menu
  // keyboard focus on its first frame (once, not every frame — a per-frame
  // focus grab would break combo popups).
  std::atomic<bool> m_focus_pending{false};
  std::thread m_thread;

  // m_status may be written from the host event thread (Hide's auto-save)
  // and read/written on the video thread.
  mutable std::mutex m_status_mutex;
  std::string m_status;

  // Video-thread-only state.
  struct DeviceChoice {
    std::string display;
    std::string value;
  };
  bool m_imgui_flags_done = false;
  bool m_ports_loaded = false;
  bool m_devices_loaded = false;
  bool m_backends_loaded = false;
  bool m_textures_scanned = false;
  std::size_t m_texture_files = 0;
  std::string m_texture_scan_dir;
  // Cheats tab. m_cheats_game_id is the disc id the list was built for, so a
  // game swap (or the first draw) triggers a re-read.
  std::vector<CheatListEntry> m_cheats;
  bool m_cheats_scanned = false;
  std::string m_cheats_game_id;
  std::string m_cheats_ini_path;
  // Region combo. Built once (the discs next to this one do not appear while
  // the menu is open) and rebuilt if the running disc id ever changes.
  std::vector<RegionChoice> m_regions;
  bool m_regions_scanned = false;
  std::string m_regions_game_id;
  // Extracted versions + the disc images that can be installed from here.
  std::vector<GameVersionEntry> m_versions;
  bool m_versions_scanned = false;
  std::string m_versions_game_id;
  std::vector<std::filesystem::path> m_install_images;
  bool m_install_images_scanned = false;
  struct InstallState {
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    int exit_code = 0;
    std::mutex mutex;
    std::string image;
  };
  std::shared_ptr<InstallState> m_install;
  std::array<std::string, 4> m_ports{};
  // What the live pad is actually configured with. A selection change is only
  // committed to m_ports after it was written to disk AND re-bound, so the UI
  // never shows a binding that failed to apply.
  std::array<std::string, 4> m_applied_ports{};
  std::vector<DeviceChoice> m_choices;
  std::vector<std::string> m_audio_backends;

  // Queued host-side core action (see HostAction). Guarded because the click
  // happens on the video thread and the execution on the overlay thread.
  mutable std::mutex m_action_mutex;
  HostAction m_pending_action = HostAction::None;
  int m_pending_slot = 1;

  // Toasts replace Dolphin's OSD text (see FilterOsdMessage). Written from
  // whichever thread emitted the message, drawn on the video thread.
  struct Toast {
    std::string text;
    std::chrono::steady_clock::time_point expires;
    bool error = false;
  };
  mutable std::mutex m_toast_mutex;
  std::vector<Toast> m_toasts;
  // Frame-counter bookkeeping, video thread only.
  std::chrono::steady_clock::time_point m_fps_window_start{};
  unsigned m_fps_frames = 0;
  double m_fps = 0.0;
};

} // namespace moderngekko::frontend
