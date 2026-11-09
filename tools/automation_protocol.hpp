#pragma once

#include "InputCommon/ControllerInterface/Touch/InputOverrider.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace moderngekko::automation
{
enum class CommandType
{
  Pad,
  PadFrames,
  ClearPad,
  Pause,
  Resume,
  SaveState,
  LoadState,
  Screenshot,
  ReadMemory,
  WriteMemory,
  Stop,
  // Delivers a keyboard keysym exactly as the platform layer would, so the
  // settings overlay (and any other hotkey handler) can be exercised by a
  // headless test run. Background consumers like a stuck screen locker can
  // hold the compositor's keyboard focus, which makes real synthetic input
  // undeliverable; this goes straight to the same callback.
  Hotkey,
  // Feeds one keysym into the OSD ImGui context (down or up) WITHOUT running
  // the hotkey callback. `hotkey` only reaches the frontend's hotkey hook, so
  // it can open the menu but cannot operate it; this is what makes widget
  // interaction (tab switching, combos, checkboxes) testable headlessly.
  Key,
  // Moves the ImGui mouse and/or presses a button. Same reasoning as Key: the
  // settings overlay is the only ImGui consumer that needs pointer input, and
  // a headless test must be able to click it.
  Mouse,
};

struct PadState
{
  int port = 0;
  std::array<double, ciface::Touch::LAST_WII_CONTROL - ciface::Touch::FIRST_GC_CONTROL + 1>
      controls{};
};

struct Command
{
  CommandType type = CommandType::Pause;
  std::string source_name;
  PadState pad;
  std::filesystem::path path;
  std::uint32_t address = 0;
  std::uint32_t size = 0;
  std::uint32_t frames = 0;
  std::uint32_t keysym = 0;
  // Key: true for a press, false for a release. Defaults to a press so a bare
  // `command=key keysym=0x20` behaves like tapping the key once `down=0` is
  // sent afterwards; the test harness always sends both explicitly.
  bool key_down = true;
  // Mouse: any subset of {position, button, wheel} may be present.
  bool has_mouse_position = false;
  float mouse_x = 0.0f;
  float mouse_y = 0.0f;
  int mouse_button = -1;  // 0 left, 1 right, 2 middle; -1 means "no button event"
  bool mouse_button_down = false;
  bool has_mouse_wheel = false;
  float wheel_x = 0.0f;
  float wheel_y = 0.0f;
  std::vector<std::uint8_t> data;
};

struct Status
{
  std::string state = "stopped";
  bool booted = false;
  double fps = 0.0;
  double vps = 0.0;
  double speed = 0.0;
  std::uint64_t frame_count = 0;
  std::uint64_t present_count = 0;
  std::string title;
  std::string game_id;
  std::string game_name;
  int navigation_mode = 0;
  bool navigation_valid = false;
  unsigned int navigation_room_id = 0;
  float navigation_player_x = 0.0f;
  float navigation_player_z = 0.0f;
  float navigation_facing = 0.0f;
  bool navigation_collision_valid = false;
  std::uint32_t navigation_collision_base = 0;
  std::uint32_t navigation_collision_segments = 0;
  std::uint32_t navigation_npc_count = 0;
  std::string navigation_archive;
  std::string navigation_location;
  std::uint64_t processed_commands = 0;
  std::string last_command;
  std::string last_error;
};

std::filesystem::path ResolveControlPath(const std::filesystem::path& automation_directory,
                                         const std::filesystem::path& value);
std::vector<std::filesystem::path>
ListCommandFiles(const std::filesystem::path& commands_directory);
bool ParseCommandFile(const std::filesystem::path& path, Command* command, std::string* error);
std::string FormatStatus(const Status& status);
}  // namespace moderngekko::automation
