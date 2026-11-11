#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moderngekko::frontend {
struct ResolutionOption {
  const char *text;
  int dolphin_scale;
};

struct GraphicsBackendOption {
  const char *text;
  const char *value;
};

// A GameCube system language. `value` is the value Dolphin stores in
// MAIN_GC_LANGUAGE / the SRAM settings block (DiscIO::Language as an int).
struct LanguageOption {
  const char *text;
  int value;
};

// A Dolphin aspect ratio mode. `value` is the string persisted in config.ini;
// `mode` is the VideoCommon::AspectMode enumerator it maps to. The config layer
// deliberately keeps this as an int so it never has to include VideoCommon.
struct AspectRatioOption {
  const char *text;
  const char *value;
  int mode;
};

struct ConfigResult {
  int dolphin_scale = 0;
  std::string resolution;
  std::string graphics_backend = "Vulkan";
  std::string audio_backend;
  std::string controller;
  std::vector<std::string> controllers;
  bool show_fps_in_title = true;
  bool fullscreen = false;
  bool vsync = true;
  int volume = 100;
  bool mute = false;
  // GameCube system language, as the persisted name ("English", "German", ...).
  // Read from the SRAM block at boot, so changing it needs a restart.
  std::string language = "English";
  // Aspect ratio mode value ("auto", "16:9", "4:3", "stretch", "raw").
  // "stretch" fills a 16:9 window with the 4:3 picture, which is how the tuned
  // profile was played and verified; the other modes leave letterbox bars.
  std::string aspect_ratio = "stretch";
  bool widescreen_hack = false;
  // Frontend-drawn FPS pill (the title-bar counter is show_fps_in_title). The
  // menu paints this itself; Dolphin's GFX_SHOW_FPS overlay is never enabled.
  bool show_fps = false;
  // HD (custom) texture packs from <user>/Load/Textures/<game id>/.
  bool hires_textures = false;
  bool cache_hires_textures = false;
  // Savestate slot used by the in-game menu (Dolphin supports 1..10).
  int state_slot = 1;
  // Master switch for Gecko / Action Replay codes (Dolphin's MAIN_ENABLE_CHEATS).
  // Which codes are on lives in Dolphin's own GameSettings/<ID>.ini, so the
  // per-code state travels with the game id instead of this file.
  bool enable_cheats = false;
  std::string netplay_nickname = "Player";
  std::string netplay_address = "127.0.0.1";
  std::uint16_t netplay_port = 2626;
  std::string netplay_buffer = "auto";
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

const std::vector<ResolutionOption> &SupportedResolutions();
const std::vector<GraphicsBackendOption> &SupportedGraphicsBackends();
const std::vector<LanguageOption> &SupportedLanguages();
const std::vector<AspectRatioOption> &SupportedAspectRatios();
// -1 when the name/value is not one of the supported ones.
int GameCubeLanguageValue(std::string_view name);
int AspectRatioMode(std::string_view value);
ConfigResult LoadConfig(const std::filesystem::path &user_directory,
                        bool create_if_missing);
bool SaveConfig(const std::filesystem::path &user_directory,
                const ConfigResult &config, std::string *error);
bool SaveConfig(const std::filesystem::path &user_directory,
                std::string_view resolution, bool show_fps_in_title,
                std::string_view controller, std::string *error);
std::string
ReadConfiguredController(const std::filesystem::path &user_directory);
std::vector<std::string>
ReadConfiguredControllers(const std::filesystem::path &user_directory);
bool ControllerConfigExists(const std::filesystem::path &user_directory);
bool GenerateControllerConfig(const std::filesystem::path &user_directory,
                              std::span<const std::string> controllers,
                              std::string *message);
bool GenerateControllerConfig(const std::filesystem::path &user_directory,
                              std::string_view controller,
                              std::string *message);
// Renders the binding block (Device= line plus control bindings) for a single
// GameCube pad section bound to the keyboard. `spec` is the configured device
// value: "Keyboard" for auto-detection or "Keyboard|<device name>" for an
// explicit device. Set by the runner, which has access to ControllerInterface;
// the config tests compile this file standalone so it must not depend on
// InputCommon. Returns an empty string when no keyboard input device is
// available.
using KeyboardPadBindingsProvider = std::string (*)(std::string_view spec);
void SetKeyboardPadBindingsProvider(KeyboardPadBindingsProvider provider);
// Same idea for gamepads: `spec` is the configured device value (the
// qualified "evdev/N/<name>" string). The provider walks the actual device
// so the written bindings use the input names that device really exposes
// (evdev names like SOUTH/TL2/"Axis 1-", not the SDL gamecontroller labels).
// Set by the runner; returns an empty string when the device is not
// connected.
using GamepadPadBindingsProvider = std::string (*)(std::string_view spec);
void SetGamepadPadBindingsProvider(GamepadPadBindingsProvider provider);
bool EnsureControllerConfig(const std::filesystem::path &user_directory,
                            std::span<const std::string> controllers,
                            std::string *message);
bool EnsureControllerConfig(const std::filesystem::path &user_directory,
                            std::string_view controller, std::string *message);
} // namespace moderngekko::frontend
