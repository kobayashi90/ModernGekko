#include "config_menu.hpp"

#include "moderngekko/game.hpp"
#include "moderngekko/hotkey.hpp"
#include "moderngekko/runtime.hpp"
#include "moderngekko/window.hpp"

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Cheats/ActionReplay.h"
#include "Core/Cheats/GeckoCode.h"
#include "Core/Cheats/GeckoCodeConfig.h"
#include "Core/Config/CheatSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace moderngekko::frontend {
namespace {

// XKB_KEY_F1 == XK_F1 == 0xFFBE (the F-keys start at 0xFFBE, NOT 0xFFC1 —
// 0xFFC1 is F4) and XKB_KEY_Escape == XK_Escape == 0xFF1B: both host
// platforms hand us X11/xkb keysyms, so plain constants work.
constexpr unsigned kF1Keysym = 0xFFBE;
constexpr unsigned kEscapeKeysym = 0xFF1B;

// Widget-geometry log for the headless UI harness. The menu is drawn with
// ImGui inside the game window, so the only way a test can click a specific
// control without hardcoding a layout is to be told where it ended up. Only
// emitted under MODERNGEKKO_MENU_DEBUG=1 (same switch as the live-state
// report), so normal runs stay quiet.
// Frame counter so the [menu-ui] log can be read as a timeline: which frame
// the pointer arrived on, which frame the item was hovered, which frame the
// button went down. Incremented once per Draw().
unsigned g_ui_frame = 0;

// Forces the next live-state report through even if nothing changed. Closing
// the menu sets this: a setting that was toggled away and back inside one menu
// session leaves the running core's snapshot identical to the one printed
// before, so a change-only report would stay silent and a test could not tell
// "restored" from "never applied at all".
std::atomic<bool> g_live_report_dirty{true};

bool UiDebugEnabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("MODERNGEKKO_MENU_DEBUG");
    return value && *value && *value != '0';
  }();
  return enabled;
}

// Logs the rect of the LAST submitted item. Call it right after the widget and
// before SameLine()/Spacing(), which would replace the last-item data.
void LogItemRect(const char *kind, const std::string &name) {
  if (!UiDebugEnabled())
    return;
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  // hover=1 means ImGui agrees the pointer is on this item THIS frame, i.e. a
  // button press now would land here. It is what separates "the harness clicked
  // the wrong place" from "the menu ignored a click on the right place".
  std::fprintf(stderr,
               "[menu-ui] f=%u %s=%s x=%.0f y=%.0f w=%.0f h=%.0f hover=%d\n",
               g_ui_frame, kind, name.c_str(), min.x, min.y, max.x - min.x,
               max.y - min.y, ImGui::IsItemHovered() ? 1 : 0);
}

// One line of plain English under a setting: what it changes, and whether the
// change lands now or after a restart.
//
// This replaces the bare "live" / "restart" tags the menu used to append with
// SameLine(). Those were shorthand between developers, not information a player
// could act on: "live" does not say what the control does, and "restart" does
// not say why. Long descriptions wrap at the window edge and are indented to
// sit under the widget they belong to, so a tab reads as setting, explanation,
// setting, explanation.
void Description(const char *text) {
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  const float indent = ImGui::GetFrameHeight() * 0.6f;
  ImGui::Indent(indent);
  // Wrap a few pixels inside the panel's right edge so the last glyph of a
  // line never sits on (or past) the border, which reads as being clipped.
  ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 4.0f);
  ImGui::TextWrapped("%s", text);
  ImGui::PopTextWrapPos();
  ImGui::Unindent(indent);
  ImGui::PopStyleColor();
}

std::string UpperCopy(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

std::string LowerCopy(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

// A device exposing SPACE/RETURN/UP inputs is a keyboard (evdev raw
// keyboards, or the X11 XInput2 KeyboardMouse device). SDL gamepads never
// expose those.
bool InputLooksLikeKeyboard(const ciface::Core::Device &device) {
  bool space = false;
  bool ret = false;
  bool up = false;
  for (const auto *input : device.Inputs()) {
    const std::string name = UpperCopy(input->GetName());
    if (name == "SPACE")
      space = true;
    else if (name == "RETURN" || name == "ENTER")
      ret = true;
    else if (name == "UP")
      up = true;
  }
  return space && ret && up;
}

// override: a qualified name ("Keyboard|<source/id/name>") or a plain name,
// empty for auto-detection (prefers devices that call themselves keyboard).
std::shared_ptr<ciface::Core::Device>
FindKeyboardDevice(const std::string &override_name) {
  std::shared_ptr<ciface::Core::Device> fallback;
  for (auto &device : g_controller_interface.GetAllDevices()) {
    if (!device || device->Inputs().empty() ||
        !InputLooksLikeKeyboard(*device))
      continue;
    if (!override_name.empty()) {
      if (device->GetQualifiedName() == override_name ||
          device->GetName() == override_name)
        return device;
      continue;
    }
    if (LowerCopy(device->GetName()).find("keyboard") != std::string::npos)
      return device;
    if (!fallback)
      fallback = device;
  }
  return override_name.empty() ? fallback : nullptr;
}

std::shared_ptr<ciface::Core::Device> DeviceForValue(const std::string &value) {
  if (value.empty())
    return nullptr;
  if (value.starts_with("Keyboard"))
    return FindKeyboardDevice(value.starts_with("Keyboard|")
                                  ? value.substr(std::string("Keyboard|").size())
                                  : std::string{});
  for (auto &device : g_controller_interface.GetAllDevices()) {
    if (device && (device->GetQualifiedName() == value ||
                   device->GetName() == value))
      return device;
  }
  return nullptr;
}

// frontend::GenerateControllerConfig callback: renders the binding block
// (Device= line plus control bindings) for one [GCPadN] section bound to a
// keyboard device.
std::string ProvideKeyboardPadBindings(std::string_view spec) {
  std::string override_name;
  if (spec.starts_with("Keyboard|"))
    override_name = std::string(spec.substr(std::string("Keyboard|").size()));
  const auto device = FindKeyboardDevice(override_name);
  if (!device)
    return {};

  // Map UPPERCASE input names to their real names.
  std::map<std::string, std::string> inputs;
  for (const auto *input : device->Inputs())
    inputs.emplace(UpperCopy(input->GetName()), input->GetName());

  // GameCube pad control -> acceptable key input names, in preference order.
  // evdev names look like SPACE/LEFTSHIFT/RETURN; the X11 XInput2 device
  // names keys by keysym (space/Shift_L/Return) — the uppercase map covers
  // both.
  static const std::vector<std::pair<const char *, std::vector<const char *>>>
      layout = {
          {"Buttons/A", {"SPACE"}},
          {"Buttons/B", {"LEFTSHIFT", "SHIFT_L"}},
          {"Buttons/X", {"Z"}},
          {"Buttons/Y", {"C"}},
          {"Buttons/Z", {"X"}},
          {"Buttons/Start", {"RETURN", "ENTER"}},
          {"Main Stick/Up", {"UP"}},
          {"Main Stick/Down", {"DOWN"}},
          {"Main Stick/Left", {"LEFT"}},
          {"Main Stick/Right", {"RIGHT"}},
          {"C-Stick/Up", {"W"}},
          {"C-Stick/Down", {"S"}},
          {"C-Stick/Left", {"A"}},
          {"C-Stick/Right", {"D"}},
          {"Triggers/L", {"Q"}},
          {"Triggers/R", {"E"}},
          {"Triggers/L-Analog", {"Q"}},
          {"Triggers/R-Analog", {"E"}},
          {"D-Pad/Up", {"I"}},
          {"D-Pad/Down", {"J"}},
          {"D-Pad/Left", {"K"}},
          {"D-Pad/Right", {"L"}},
      };

  std::string out = "Device = " + device->GetQualifiedName() + "\n";
  for (const auto &[control, keys] : layout) {
    for (const char *key : keys) {
      if (const auto it = inputs.find(key); it != inputs.end()) {
        out += control;
        out += " = `";
        out += it->second;
        out += "`\n";
        break;
      }
    }
  }
  out += "Main Stick/Calibration = 100.00\n";
  out += "C-Stick/Calibration = 100.00\n";
  return out;
}

// frontend::GenerateControllerConfig callback: renders the binding block for
// one [GCPadN] section bound to a gamepad by walking the real device, so the
// written names are the ones the running backend actually exposes. This
// build uses Dolphin's evdev backend, which names buttons after the kernel
// event codes (SOUTH/EAST/NORTH/WEST/TL/TR/TL2/TR2/START/DPAD_UP/...) and
// axes "Axis N±" numbered in ascending ABS-code order — none of the SDL
// gamecontroller labels ("Button A", "Left Y+", "Trigger L", "Pad N") exist
// there, which is why generated configs silently bound nothing.
std::string ProvideGamepadPadBindings(std::string_view spec) {
  const auto device = DeviceForValue(std::string(spec));
  if (!device)
    return {};

  // Exact input names the device exposes (NamedButton returns the event code
  // name; axis inputs are "Axis N-"/"Axis N+").
  std::set<std::string> names;
  for (const auto *input : device->Inputs())
    names.insert(input->GetName());
  const auto has = [&names](const char *name) { return names.count(name) != 0; };

  // evdev codes are positional (SOUTH=bottom, EAST=right, NORTH=top,
  // WEST=left). Nintendo-labeled pads put A/B/X/Y on east/south/north/west
  // respectively, so bind by the printed label there (A stays A on a Switch
  // pad); Xbox-labeled pads get the standard a=south, b=east, x=west,
  // y=north label mapping.
  const std::string device_name = LowerCopy(device->GetName());
  const bool nintendo_labels =
      device_name.find("nintendo") != std::string::npos ||
      device_name.find("switch") != std::string::npos ||
      device_name.find("joy-con") != std::string::npos ||
      device_name.find("joycon") != std::string::npos;

  // Axis groups in ascending index order. Standard pads expose, in ABS code
  // order: X, Y, [Z, RZ on pads with analog triggers], RX, RY, HAT0X,
  // HAT0Y — so the c-stick/hat positions shift by two when triggers are
  // analog axes rather than digital buttons.
  std::map<int, std::pair<bool, bool>> axes; // index -> {has '-', has '+'}
  for (const auto *input : device->Inputs()) {
    const std::string &name = input->GetName();
    if (name.size() < 7 || name.compare(0, 5, "Axis ") != 0)
      continue;
    const char dir = name.back();
    if (dir != '-' && dir != '+')
      continue;
    int index = -1;
    const auto [ptr, ec] =
        std::from_chars(name.data() + 5, name.data() + name.size() - 1, index);
    if (ec != std::errc{} || ptr != name.data() + name.size() - 1 || index < 0)
      continue;
    auto &[minus, plus] = axes[index];
    if (dir == '-')
      minus = true;
    else
      plus = true;
  }

  int c_stick_x = 2, c_stick_y = 3, hat_x = 4, hat_y = 5;
  const bool analog_triggers =
      !axes.empty() && axes.rbegin()->first >= 7; // Z/RZ present
  if (analog_triggers) {
    c_stick_x = 3;
    c_stick_y = 4;
    hat_x = 6;
    hat_y = 7;
  }

  std::string out = "Device = " + device->GetQualifiedName() + "\n";
  const auto bind = [&out,
                     &has](const char *control,
                           std::initializer_list<const char *> candidates) {
    for (const char *candidate : candidates) {
      if (has(candidate)) {
        out += control;
        out += " = `";
        out += candidate;
        out += "`\n";
        return;
      }
    }
  };
  const auto axis = [&out, &axes](const char *control, int index, char dir) {
    const auto it = axes.find(index);
    if (it == axes.end())
      return;
    const bool present = dir == '-' ? it->second.first : it->second.second;
    if (!present)
      return;
    out += control;
    out += " = `Axis ";
    out += std::to_string(index);
    out += dir;
    out += "`\n";
  };

  if (nintendo_labels) {
    bind("Buttons/A", {"EAST"});
    bind("Buttons/B", {"SOUTH"});
    bind("Buttons/X", {"NORTH"});
    bind("Buttons/Y", {"WEST", "C"});
  } else {
    bind("Buttons/A", {"SOUTH"});
    bind("Buttons/B", {"EAST"});
    bind("Buttons/X", {"WEST"});
    bind("Buttons/Y", {"NORTH"});
  }
  bind("Buttons/Z", {"TR"});
  bind("Buttons/Start", {"START"});

  axis("Main Stick/Up", 1, '-');
  axis("Main Stick/Down", 1, '+');
  axis("Main Stick/Left", 0, '-');
  axis("Main Stick/Right", 0, '+');
  axis("C-Stick/Up", c_stick_y, '-');
  axis("C-Stick/Down", c_stick_y, '+');
  axis("C-Stick/Left", c_stick_x, '-');
  axis("C-Stick/Right", c_stick_x, '+');

  // GC L/R sit where the pad's triggers do: analog axis when the pad has
  // one, otherwise the digital trigger buttons (ZL/ZR on a Switch pad).
  const auto bind_trigger = [&](const char *control, int trigger_axis,
                                const char *button) {
    if (analog_triggers) {
      const std::string before = out;
      axis(control, trigger_axis, '+');
      if (out != before)
        return;
    }
    bind(control, {button});
  };
  bind_trigger("Triggers/L", 2, "TL2");
  bind_trigger("Triggers/R", 5, "TR2");
  bind_trigger("Triggers/L-Analog", 2, "TL2");
  bind_trigger("Triggers/R-Analog", 5, "TR2");

  if (has("DPAD_UP")) {
    bind("D-Pad/Up", {"DPAD_UP"});
    bind("D-Pad/Down", {"DPAD_DOWN"});
    bind("D-Pad/Left", {"DPAD_LEFT"});
    bind("D-Pad/Right", {"DPAD_RIGHT"});
  } else {
    axis("D-Pad/Up", hat_y, '-');
    axis("D-Pad/Down", hat_y, '+');
    axis("D-Pad/Left", hat_x, '-');
    axis("D-Pad/Right", hat_x, '+');
  }

  // evdev exposes rumble as Strong/Weak force-feedback outputs — and only
  // when the device supports it at all (our uinput Pro Controller does not,
  // so the line is simply omitted there).
  bool rumble = false;
  for (const auto *output : device->Outputs())
    if (output->GetName() == "Strong" || output->GetName() == "Weak")
      rumble = true;
  if (rumble)
    out += "Rumble/Motor = `Strong` | `Weak`\n";

  out += "Main Stick/Calibration = 100.00\n";
  out += "C-Stick/Calibration = 100.00\n";
  return out;
}

} // namespace

ConfigMenu::~ConfigMenu() { Stop(); }

void ConfigMenu::Start(Hooks hooks) {
  m_hooks = std::move(hooks);
  m_runtime = m_hooks.runtime;
  SetKeyboardPadBindingsProvider(&ProvideKeyboardPadBindings);
  SetGamepadPadBindingsProvider(&ProvideGamepadPadBindings);
  SetHotkeyCallback(&ConfigMenu::HotkeyThunk, this);
  VideoCommon::OnScreenUI::SetOverlayDrawHook(
      [this] { DrawOverlayFrame(); });
  // Silences the stock Dolphin OSD text for everything this menu triggers and
  // presents it as a toast instead.
  OSD::SetMessageFilter(
      [this](std::string &message) { return FilterOsdMessage(message); });
  std::fprintf(stderr,
               "[menu] hooks registered (hotkey + in-game overlay draw)\n");
  m_thread = std::thread([this] { ThreadMain(); });
}

void ConfigMenu::Stop() {
  if (m_quit.exchange(true))
    return;
  SetHotkeyCallback(nullptr, nullptr);
  OSD::SetMessageFilter(nullptr);
  // Safe: Stop() runs after Runtime::Run() returned, so the video thread
  // that invokes the hook is already gone.
  VideoCommon::OnScreenUI::SetOverlayDrawHook(nullptr);
  if (m_thread.joinable())
    m_thread.join();
}

bool ConfigMenu::HotkeyThunk(void *userdata, unsigned keysym) {
  auto *self = static_cast<ConfigMenu *>(userdata);
  if (keysym == kF1Keysym) {
    const bool visible =
        self->m_visible.load(std::memory_order_relaxed);
    std::fprintf(stderr, "[menu] F1 -> %s\n", visible ? "hide" : "show");
    if (visible)
      self->Hide();
    else
      self->Show();
    return true; // claim F1 so the savestate hotkeys don't also fire
  }
  if (self->m_visible.load(std::memory_order_relaxed)) {
    if (keysym == kEscapeKeysym) {
      self->Hide();
      return false; // let the platform keep handling e.g. Ctrl+Escape
    }
    // Menu open: claim everything else so savestate/F10 hotkeys can't fire
    // mid-configuration. The key has already been fed to the menu's ImGui
    // context by the platform layer before this hook runs.
    std::fprintf(stderr, "[menu] claimed key 0x%x while visible\n", keysym);
    return true;
  }
  return false;
}

void ConfigMenu::Show() {
  bool expected = false;
  if (!m_visible.compare_exchange_strong(expected, true)) {
    std::fprintf(stderr, "[menu] Show: already visible, ignoring\n");
    return;
  }
  std::fprintf(stderr, "[menu] Show: visible=1, pausing runtime\n");
  m_focus_pending.store(true, std::memory_order_relaxed);
  if (m_runtime)
    m_runtime->Pause();
  std::fprintf(stderr, "[menu] Show: runtime paused\n");
}

void ConfigMenu::Hide() {
  bool expected = true;
  if (!m_visible.compare_exchange_strong(expected, false))
    return;
  // Closing the menu (F1/ESC) persists whatever was changed — this is what
  // makes settings actually stick without a trip through the System tab.
  // Consumed by DrawOverlayFrame() on the video thread so ConfigResult is
  // never touched concurrently.
  m_save_pending.store(true, std::memory_order_relaxed);
  g_live_report_dirty.store(true, std::memory_order_relaxed);
  if (m_runtime)
    m_runtime->Resume();
  std::fprintf(stderr, "[menu] Hide: resumed, config save queued\n");
}

void ConfigMenu::ThreadMain() {
  using namespace std::chrono_literals;
  while (!m_quit.load(std::memory_order_relaxed)) {
    if (!m_visible.load(std::memory_order_relaxed)) {
      // Deferred core actions (savestate, reset, screenshot) run here and
      // nowhere else: the menu is closed, so the core is running again and
      // the CPU thread can take the job. See QueueHostAction.
      RunPendingHostActions();
      std::this_thread::sleep_for(50ms);
      continue;
    }
    // Emulation is paused while the menu is open, so no XFB swaps happen
    // and nothing presents frames: drive the presenter from here to keep the
    // frozen game frame + overlay on screen. Present() blocks on vsync when
    // the backend does; the sleep_until only pads the remainder, so the loop
    // runs at ~60 Hz either way.
    const auto start = std::chrono::steady_clock::now();
    VideoCommon::Presenter::PresentOverlayFrame();
    std::this_thread::sleep_until(start + 16ms);
  }
}

void ConfigMenu::QueueHostAction(HostAction action) {
  std::lock_guard lock(m_action_mutex);
  m_pending_action = action;
  m_pending_slot =
      std::clamp(m_hooks.config ? m_hooks.config->state_slot : 1, 1,
                 static_cast<int>(State::NUM_STATES));
}

void ConfigMenu::RunPendingHostActions() {
  HostAction action = HostAction::None;
  int slot = 1;
  {
    std::lock_guard lock(m_action_mutex);
    action = m_pending_action;
    slot = m_pending_slot;
    m_pending_action = HostAction::None;
  }
  if (action == HostAction::None)
    return;

  auto &system = Core::System::GetInstance();
  switch (action) {
  case HostAction::SaveState: {
    std::fprintf(stderr, "[menu] state: saving slot %d\n", slot);
    PushToast("Saving state to slot " + std::to_string(slot));
    // Asynchronous by design: the CPU thread snapshots into a buffer and a
    // compression thread writes the file, so the game keeps running.
    State::Save(system, slot);
    break;
  }
  case HostAction::LoadState: {
    std::fprintf(stderr, "[menu] state: loading slot %d\n", slot);
    // Dolphin reports a missing file through the OSD, which the filter turns
    // into a toast, so there is nothing to check here.
    State::Load(system, slot);
    break;
  }
  case HostAction::Reset: {
    std::fprintf(stderr, "[menu] state: reset button\n");
    PushToast("Resetting the console");
    system.GetProcessorInterface().ResetButton_Tap();
    break;
  }
  case HostAction::Screenshot: {
    std::fprintf(stderr, "[menu] state: screenshot requested\n");
    PushToast("Capturing screenshot");
    // Sets a request; the PNG is written by the next Present, which is why the
    // menu closes itself first -- otherwise the shot would contain the menu.
    Core::SaveScreenShot();
    break;
  }
  case HostAction::None:
    break;
  }
}

bool ConfigMenu::FilterOsdMessage(std::string &message) {
  // Invoked from whatever thread emitted the message, with the OSD's message
  // lock held, so this must not call back into OSD and must not block on the
  // video thread. Pushing a toast (its own mutex) is all it does.
  // The harness asserts this line to prove the filter really caught emulator
  // text: every branch below returns true, so a message that is logged here can
  // never be drawn with Dolphin's own styling.
  if (UiDebugEnabled())
    std::fprintf(stderr, "[menu-osd] message: %s\n", message.c_str());
  auto starts_with = [&message](std::string_view prefix) {
    return message.compare(0, prefix.size(), prefix) == 0;
  };

  // Savestates. Dolphin says "Saved State to <file>" etc.; nobody needs the
  // filename, and the phrase "Savestate" reads like an emulator debug aid.
  if (message == "Saving State...") {
    PushToast("Saving state…");
    return true;
  }
  if (starts_with("Saved State to ")) {
    PushToast("State saved");
    return true;
  }
  if (starts_with("Loaded State from ")) {
    PushToast("State loaded");
    return true;
  }
  if (message == "State doesn't exist") {
    PushToast("That slot is empty");
    return true;
  }
  if (message == "The savestate could not be loaded") {
    PushToast("That state could not be loaded");
    return true;
  }
  if (message == "Failed to create state file" ||
      message == "Failed to write state file" ||
      message == "Failed to close state file" ||
      message == "Failed to rename state file" ||
      message == "Unable to save: Internal DoState Error") {
    PushToast("The state could not be saved");
    return true;
  }

  // Screenshots.
  if (starts_with("Screenshot saved to ")) {
    PushToast("Screenshot saved");
    return true;
  }

  // HD texture packs. Dolphin prints this every time the loader refreshes,
  // including "Found 0 custom textures", which is noise.
  if (starts_with("Loading '") && message.find("custom textures") != std::string::npos) {
    PushToast("HD texture pack loaded");
    return true;
  }
  if (starts_with("Found '") && message.find("custom textures") != std::string::npos) {
    PushToast("HD texture pack scanned");
    return true;
  }

  // Anything left is still Dolphin's own presentation: its font, its corner of
  // the screen, its fade. Forward the text to the frontend's toast and swallow
  // the original, so the player never sees the emulator's chrome. Warnings that
  // no rule above rewrites still reach the screen, just in this menu's style.
  if (!message.empty())
    PushToast(message);
  return true;
}

void ConfigMenu::PushToast(std::string text) {
  std::lock_guard lock(m_toast_mutex);
  const auto expiry = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  // The OSD filter forwards every surviving emulator message here, and several
  // of them repeat per frame ("No memory card in Slot A" types). Refreshing the
  // existing toast keeps one line on screen instead of cycling the same text
  // through the whole queue.
  for (Toast &toast : m_toasts) {
    if (toast.text == text) {
      toast.expires = expiry;
      return;
    }
  }
  m_toasts.push_back(Toast{std::move(text), expiry});
  // A queue of toasts stacked in the corner is a console log, not a UI.
  constexpr std::size_t kMaxToasts = 3;
  if (m_toasts.size() > kMaxToasts)
    m_toasts.erase(m_toasts.begin(), m_toasts.end() - kMaxToasts);
}

void ConfigMenu::DrawToasts() {
  // Drawn into the background draw list so it lands in the game's frame no
  // matter which window (if any) is open -- the whole point is that it works
  // while the menu is closed, which is when the actions run.
  std::vector<Toast> toasts;
  {
    std::lock_guard lock(m_toast_mutex);
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_toasts, [now](const Toast &toast) { return toast.expires <= now; });
    toasts = m_toasts;
  }
  if (toasts.empty())
    return;

  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  const ImGuiIO &io = ImGui::GetIO();
  const float scale = std::max(io.DisplayFramebufferScale.x, 1.0f);
  const float pad_x = 14.0f * scale;
  const float pad_y = 9.0f * scale;
  const float gap = 6.0f * scale;
  const float margin = 24.0f * scale;

  // Top-left, stacked downwards.
  float y = margin;
  for (const Toast &toast : toasts) {
    const ImVec2 size = ImGui::CalcTextSize(toast.text.c_str());
    const float width = size.x + pad_x * 2.0f;
    const float height = size.y + pad_y * 2.0f;
    if (y + height > io.DisplaySize.y)
      break;
    const ImVec2 min(margin, y);
    const ImVec2 max(margin + width, y + height);
    // This frontend's palette: near-black glass, warm ember accent.
    const ImU32 accent = ImGui::GetColorU32(ImVec4(1.00f, 0.55f, 0.20f, 1.0f));
    draw->AddRectFilled(min, max, IM_COL32(12, 12, 14, 232), height * 0.35f);
    draw->AddRectFilled(ImVec2(min.x, min.y + height * 0.25f),
                        ImVec2(min.x + 3.0f * scale, min.y + height * 0.75f), accent,
                        1.5f * scale);
    draw->AddRect(min, max, IM_COL32(255, 255, 255, 26), height * 0.35f);
    draw->AddText(ImVec2(min.x + pad_x, min.y + pad_y),
                  ImGui::GetColorU32(ImVec4(0.93f, 0.93f, 0.95f, 1.0f)),
                  toast.text.c_str());
    y += height + gap;
  }
}

void ConfigMenu::DrawFrameCounter() {
  // Replaces GFX_SHOW_FPS: Dolphin's performance overlay is a wall of
  // millisecond figures in its own font. One number is all that is wanted.
  const auto now = std::chrono::steady_clock::now();
  if (m_fps_window_start.time_since_epoch().count() == 0)
    m_fps_window_start = now;
  ++m_fps_frames;
  const double elapsed =
      std::chrono::duration<double>(now - m_fps_window_start).count();
  if (elapsed >= 0.5) {
    m_fps = static_cast<double>(m_fps_frames) / elapsed;
    m_fps_frames = 0;
    m_fps_window_start = now;
  }
  if (m_fps <= 0.0)
    return;

  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  const ImGuiIO &io = ImGui::GetIO();
  const float scale = std::max(io.DisplayFramebufferScale.x, 1.0f);
  char text[32];
  std::snprintf(text, sizeof(text), "%.0f fps", m_fps);
  const ImVec2 size = ImGui::CalcTextSize(text);
  const float pad = 8.0f * scale;
  const ImVec2 min(io.DisplaySize.x - size.x - pad * 2.0f - 16.0f * scale,
                   16.0f * scale);
  const ImVec2 max(min.x + size.x + pad * 2.0f, min.y + size.y + pad);
  draw->AddRectFilled(min, max, IM_COL32(12, 12, 14, 200), (max.y - min.y) * 0.35f);
  draw->AddText(ImVec2(min.x + pad, min.y + pad * 0.5f),
                ImGui::GetColorU32(ImVec4(1.00f, 0.55f, 0.20f, 1.0f)), text);
}

void ConfigMenu::DrawOverlayFrame() {
  // Called on the video thread from OnScreenUI::Finalize: the OSD's ImGui
  // context is current and the ImGui lock is held, so this draws straight
  // into the game window's frame.
  if (!m_imgui_flags_done) {
    m_imgui_flags_done = true;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  }
  // Persist-on-close: runs even when the menu is (already) hidden.
  if (m_save_pending.exchange(false, std::memory_order_relaxed))
    Save("config");
  // Proof for the regression harness that the running core actually picked up
  // a live-applied setting (only under MODERNGEKKO_MENU_DEBUG=1).
  ReportLiveState();
  // Presentation that belongs to the game session, not to the menu: toasts
  // replace Dolphin's OSD text and the counter replaces GFX_SHOW_FPS.
  DrawToasts();
  if (m_hooks.config && m_hooks.config->show_fps)
    DrawFrameCounter();
  if (!m_visible.load(std::memory_order_relaxed))
    return;
  if (m_focus_pending.exchange(false, std::memory_order_relaxed))
    ImGui::SetNextWindowFocus();
  // The OSD resets FontSizeBase from its config after ImGui::Render() every
  // frame, so bumping it here only affects our window this frame.
  // Text scales with the panel: a 16 px font inside a 1150x820 panel on a
  // 1382-tall backbuffer is unreadable small print.
  const float ui_scale =
      std::clamp(ImGui::GetIO().DisplaySize.y / 900.0f, 1.0f, 1.5f);
  ImGui::GetStyle().FontSizeBase =
      std::max(ImGui::GetStyle().FontSizeBase, 16.0f * ui_scale);
  Draw();
}

namespace {

// A coherent dark theme for the overlay: the stock ImGui defaults are grey and
// cramped, and this window sits on top of a game (its own colours), so it needs
// to look deliberate rather than like a debug panel. Values are deliberately
// few and flat -- rounded frames, a teal accent for "on" states, muted borders.
struct MenuStyleColors
{
  ImGuiCol index;
  ImVec4 color;
};

constexpr MenuStyleColors kMenuColors[] = {
    {ImGuiCol_WindowBg, ImVec4(0.075f, 0.085f, 0.115f, 0.985f)},
    {ImGuiCol_TitleBg, ImVec4(0.10f, 0.12f, 0.16f, 1.00f)},
    {ImGuiCol_TitleBgActive, ImVec4(0.12f, 0.145f, 0.19f, 1.00f)},
    {ImGuiCol_Border, ImVec4(0.24f, 0.28f, 0.36f, 0.65f)},
    {ImGuiCol_FrameBg, ImVec4(0.145f, 0.17f, 0.22f, 1.00f)},
    {ImGuiCol_FrameBgHovered, ImVec4(0.21f, 0.25f, 0.32f, 1.00f)},
    {ImGuiCol_FrameBgActive, ImVec4(0.26f, 0.31f, 0.39f, 1.00f)},
    {ImGuiCol_Button, ImVec4(0.17f, 0.21f, 0.28f, 1.00f)},
    {ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.31f, 0.40f, 1.00f)},
    {ImGuiCol_ButtonActive, ImVec4(0.30f, 0.39f, 0.50f, 1.00f)},
    {ImGuiCol_Header, ImVec4(0.19f, 0.25f, 0.33f, 1.00f)},
    {ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.33f, 0.43f, 1.00f)},
    {ImGuiCol_HeaderActive, ImVec4(0.29f, 0.39f, 0.51f, 1.00f)},
    {ImGuiCol_Tab, ImVec4(0.13f, 0.15f, 0.20f, 1.00f)},
    {ImGuiCol_TabHovered, ImVec4(0.24f, 0.32f, 0.42f, 1.00f)},
    {ImGuiCol_TabSelected, ImVec4(0.19f, 0.25f, 0.33f, 1.00f)},
    {ImGuiCol_CheckMark, ImVec4(0.35f, 0.80f, 0.67f, 1.00f)},
    {ImGuiCol_SliderGrab, ImVec4(0.35f, 0.80f, 0.67f, 1.00f)},
    {ImGuiCol_SliderGrabActive, ImVec4(0.45f, 0.90f, 0.78f, 1.00f)},
    {ImGuiCol_Separator, ImVec4(0.28f, 0.33f, 0.42f, 0.70f)},
    {ImGuiCol_Text, ImVec4(0.90f, 0.92f, 0.95f, 1.00f)},
    {ImGuiCol_TextDisabled, ImVec4(0.58f, 0.62f, 0.70f, 1.00f)},
    {ImGuiCol_ScrollbarBg, ImVec4(0.10f, 0.12f, 0.16f, 1.00f)},
    {ImGuiCol_ScrollbarGrab, ImVec4(0.26f, 0.31f, 0.40f, 1.00f)},
    {ImGuiCol_PopupBg, ImVec4(0.10f, 0.12f, 0.16f, 0.98f)},
};

void PushMenuStyle()
{
  for (const auto& entry : kMenuColors)
    ImGui::PushStyleColor(entry.index, entry.color);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 13.0f);
}

void PopMenuStyle()
{
  ImGui::PopStyleVar(11);
  ImGui::PopStyleColor(IM_ARRAYSIZE(kMenuColors));
}

} // namespace

void ConfigMenu::Draw() {
  const ImGuiIO &io = ImGui::GetIO();
  ++g_ui_frame;
  if (UiDebugEnabled()) {
    // Pointer position as the menu sees it (backbuffer pixels). Logged on
    // change only: the harness asserts the pointer actually arrived, which is
    // otherwise indistinguishable from "the click missed".
    static ImVec2 previous{FLT_MAX, FLT_MAX};
    if (io.MousePos.x != previous.x || io.MousePos.y != previous.y) {
      previous = io.MousePos;
      std::fprintf(stderr, "[menu-ui] f=%u mouse x=%.0f y=%.0f down=%d\n",
                   g_ui_frame, io.MousePos.x, io.MousePos.y,
                   io.MouseDown[0] ? 1 : 0);
    }
  }
  // The panel is a share of the backbuffer, not a fixed 720x560: at a 2560-wide
  // window the fixed size looked like a postage stamp and the System tab had to
  // scroll three rows of text in a box the size of a business card. The caps
  // keep it sane on a small window and stop it from becoming a full-screen
  // settings wall on a 4K one.
  // The floors matter more than the fractions: the System tab (five info lines,
  // language, region, the installed-version box and four buttons) needs ~560 px
  // of height at font size 16, so a 1280x720 window must still get a 560 px
  // panel instead of 0.62 x 720 = 446 px, which pushed the buttons out of the
  // window -- the "the System tab makes it too small" complaint.
  const float width =
      std::clamp(io.DisplaySize.x * 0.45f, 440.0f,
                 std::min(io.DisplaySize.x - 20.0f, 1180.0f));
  const float height =
      std::clamp(std::max(io.DisplaySize.y * 0.62f, 560.0f), 280.0f,
                 std::min(io.DisplaySize.y - 20.0f, 820.0f));
  PushMenuStyle();
  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  ImGui::Begin("Settings", nullptr,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
  // Accent, not TextDisabled: the header is the one line that says which game
  // is paused, so it should read as a header instead of grey fine print.
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.85f, 0.78f, 1.00f));
  ImGui::Text("%s [%s] — emulation paused while this menu is open",
              m_hooks.game_name.c_str(), m_hooks.disc_id.c_str());
  ImGui::PopStyleColor();
  // No separator here: the tab bar already reads as a section break, and the
  // extra rule pushed the header text far enough from the tabs that it stopped
  // reading as their heading.
  // Tighter spacing before the tab bar: the header line is the heading for the
  // tabs, and the default gap made it read as a separate, unrelated row.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                      ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f));
  const bool tab_bar_visible = ImGui::BeginTabBar("settings-tabs");
  ImGui::PopStyleVar();
  if (tab_bar_visible) {
    // The rect is logged immediately after BeginTabItem, not after EndTabItem.
    // EndTabItem does not restore ImGui's "last item" state, so for the *selected*
    // tab it still described whatever was drawn last inside the content -- the
    // harness then aimed its clicks at a separator instead of the tab button.
    // Non-selected tabs only looked correct because their content never ran.
    const bool display_open = ImGui::BeginTabItem("Display");
    LogItemRect("tab", "Display");
    if (display_open) {
      DrawDisplayTab();
      ImGui::EndTabItem();
    }
    const bool input_open = ImGui::BeginTabItem("Input");
    LogItemRect("tab", "Input");
    if (input_open) {
      DrawInputTab();
      ImGui::EndTabItem();
    }
    const bool audio_open = ImGui::BeginTabItem("Audio");
    LogItemRect("tab", "Audio");
    if (audio_open) {
      DrawAudioTab();
      ImGui::EndTabItem();
    }
    const bool textures_open = ImGui::BeginTabItem("Textures");
    LogItemRect("tab", "Textures");
    if (textures_open) {
      DrawTexturesTab();
      ImGui::EndTabItem();
    }
    const bool states_open = ImGui::BeginTabItem("States");
    LogItemRect("tab", "States");
    if (states_open) {
      DrawStatesTab();
      ImGui::EndTabItem();
    }
    const bool cheats_open = ImGui::BeginTabItem("Cheats");
    LogItemRect("tab", "Cheats");
    if (cheats_open) {
      DrawCheatsTab();
      ImGui::EndTabItem();
    }
    const bool system_open = ImGui::BeginTabItem("System");
    LogItemRect("tab", "System");
    if (system_open) {
      DrawSystemTab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  std::string status;
  {
    std::lock_guard lock(m_status_mutex);
    status = m_status;
  }
  if (!status.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%s", status.c_str());
  }
  ImGui::End();
  PopMenuStyle();
}

void ConfigMenu::DrawDisplayTab() {
  ConfigResult &cfg = *m_hooks.config;
  const auto &resolutions = SupportedResolutions();
  int current = 0;
  for (int i = 0; i < static_cast<int>(resolutions.size()); ++i) {
    if (resolutions[static_cast<std::size_t>(i)].text == cfg.resolution) {
      current = i;
      break;
    }
  }
  if (ImGui::BeginCombo("Internal resolution",
                        resolutions[static_cast<std::size_t>(current)].text)) {
    for (int i = 0; i < static_cast<int>(resolutions.size()); ++i) {
      const auto &option = resolutions[static_cast<std::size_t>(i)];
      if (ImGui::Selectable(option.text, i == current)) {
        cfg.resolution = option.text;
        cfg.dolphin_scale = option.dolphin_scale;
        Save("config");
        ApplyDisplayLive();
        SetStatus("internal resolution applied");
      }
      LogItemRect("item", std::string("resolution/") + option.text);
    }
    ImGui::EndCombo();
  }
  LogItemRect("widget", "resolution");
  Description("Renders the game at a higher resolution than the disc's own "
              "640x480 and scales the result to the window. Costs GPU time. "
              "Applies on the next frame.");

  // OGL cannot initialize on a Wayland surface in this build (GLContext::
  // Create has no Wayland path), so never offer it there — picking it would
  // boot-fail with "Failed to initialize video backend!".
  const auto &all_backends = SupportedGraphicsBackends();
  std::vector<GraphicsBackendOption> backends;
  backends.reserve(all_backends.size());
  for (const auto &backend : all_backends) {
    if (m_hooks.allow_ogl || backend.value != "OGL")
      backends.push_back(backend);
  }
  int backend_current = 0;
  for (int i = 0; i < static_cast<int>(backends.size()); ++i) {
    if (backends[static_cast<std::size_t>(i)].value == cfg.graphics_backend) {
      backend_current = i;
      break;
    }
  }
  if (ImGui::BeginCombo("Graphics backend", backends[backend_current].text)) {
    for (int i = 0; i < static_cast<int>(backends.size()); ++i) {
      if (ImGui::Selectable(backends[static_cast<std::size_t>(i)].text,
                            i == backend_current)) {
        cfg.graphics_backend = backends[static_cast<std::size_t>(i)].value;
        Save("config");
        SetStatus("graphics backend saved — needs a restart");
      }
      LogItemRect("item", std::string("backend/") +
                              backends[static_cast<std::size_t>(i)].value);
    }
    ImGui::EndCombo();
  }
  LogItemRect("widget", "backend");
  Description("Chooses the renderer. Needs a restart: the window and the "
              "graphics device are created once at boot, so a new backend "
              "cannot be swapped in under a running game.");

  if (ImGui::Checkbox("Fullscreen", &cfg.fullscreen)) {
    Save("config");
    ApplyDisplayLive();
    SetStatus("fullscreen applied");
  }
  LogItemRect("widget", "fullscreen");
  Description("Fills the whole screen instead of running in a window. Applies "
              "immediately.");

  if (ImGui::Checkbox("VSync", &cfg.vsync)) {
    Save("config");
    ApplyDisplayLive();
    SetStatus("vsync applied");
  }
  LogItemRect("widget", "vsync");
  Description("Locks presentation to the monitor's refresh rate. Removes "
              "tearing at the cost of a little input latency. Applies "
              "immediately.");

  // This one is only a window-title decoration, and the title is rebuilt by the
  // runtime when it changes, so a save is enough.
  if (ImGui::Checkbox("Show FPS in window title", &cfg.show_fps_in_title))
    Save("config");

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextDisabled("Aspect and display extras");

  const auto &ratios = SupportedAspectRatios();
  int ratio_current = 0;
  for (int i = 0; i < static_cast<int>(ratios.size()); ++i) {
    if (ratios[static_cast<std::size_t>(i)].value == cfg.aspect_ratio) {
      ratio_current = i;
      break;
    }
  }
  if (ImGui::BeginCombo("Aspect ratio", ratios[ratio_current].text)) {
    for (int i = 0; i < static_cast<int>(ratios.size()); ++i) {
      const auto &option = ratios[static_cast<std::size_t>(i)];
      if (ImGui::Selectable(option.text, i == ratio_current)) {
        cfg.aspect_ratio = option.value;
        Save("config");
        ApplyDisplayLive();
        SetStatus("aspect ratio applied");
      }
      LogItemRect("item", std::string("aspect/") + option.value);
      if (i == ratio_current)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  LogItemRect("widget", "aspect");
  Description("Sets the shape of the image. Applies immediately.");

  if (ImGui::Checkbox("Widescreen hack", &cfg.widescreen_hack)) {
    Save("config");
    ApplyDisplayLive();
    SetStatus("widescreen hack applied");
  }
  LogItemRect("widget", "widescreen");
  Description("Widens the 3D scene to fill a 16:9 screen by stretching it, not "
              "by revealing more of it. The HUD and 2D layers usually survive, "
              "but shapes can look slightly wide. Applies immediately.");

  // The frontend's own counter, drawn as a pill in the corner. It is NOT
  // GFX_SHOW_FPS: that renders Dolphin's frame-time wall, which is exactly the
  // emulator chrome this build is trying not to show.
  if (ImGui::Checkbox("Show FPS overlay", &cfg.show_fps)) {
    Save("config");
    ApplyDisplayLive();
    SetStatus("on-screen FPS counter applied");
  }
  LogItemRect("widget", "showfps");
  Description("Draws this build's own frame counter in the corner of the "
              "screen. Applies immediately.");
}

void ConfigMenu::DrawInputTab() {
  ConfigResult &cfg = *m_hooks.config;
  if (!m_devices_loaded) {
    m_devices_loaded = true;
    RefreshDevices();
  }
  if (!m_ports_loaded) {
    m_ports_loaded = true;
    for (std::size_t i = 0; i < m_ports.size(); ++i)
      m_ports[i] = i < cfg.controllers.size() ? cfg.controllers[i] : "";
    m_applied_ports = m_ports;
  }

  if (ImGui::Button("Rescan devices"))
    RescanDevices();
  LogItemRect("widget", "rescan");
  ImGui::SameLine();
  ImGui::TextDisabled("%s", Pad::IsInitialized() ? "applies immediately"
                                                 : "pad is not initialised");

  for (int port = 0; port < 4; ++port) {
    ImGui::PushID(port);
    const std::string &value = m_ports[static_cast<std::size_t>(port)];
    const char *preview = "(none)";
    bool known = value.empty();
    for (const auto &choice : m_choices) {
      if (choice.value == value) {
        preview = choice.display.c_str();
        known = true;
        break;
      }
    }
    // A saved device that is not present right now used to render as "(none)",
    // which reads as "this port is empty" -- it is not, it is disconnected.
    const std::string missing = known ? std::string() : " (not connected)";
    if (!known)
      preview = value.c_str();
    if (ImGui::BeginCombo("Port device", preview)) {
      for (const auto &choice : m_choices) {
        const bool selected = choice.value == value;
        if (ImGui::Selectable(choice.display.c_str(), selected)) {
          if (choice.value != value) {
            m_ports[static_cast<std::size_t>(port)] = choice.value;
            ApplyControllerLive();
          }
        }
        LogItemRect("item", "port" + std::to_string(port) + "/" +
                                choice.display);
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    LogItemRect("widget", "port" + std::to_string(port));
    if (!missing.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", missing.c_str());
    }
    ImGui::PopID();
  }

  if (ImGui::CollapsingHeader("Keyboard mapping")) {
    ImGui::TextWrapped(
        "Main stick: arrow keys     C-stick: W A S D\n"
        "A: Space    B: Left Shift    X: Z key    Y: C key    "
        "Z (shoulder): X key\n"
        "L / R (analog): Q / E    Start: Return    D-pad: I J K L");
  }

  ImGui::Separator();
  ImGui::Text("Live input test (currently pressed):");
  bool any_device = false;
  for (int port = 0; port < 4; ++port) {
    const auto device = DeviceForValue(m_ports[static_cast<std::size_t>(port)]);
    if (!device)
      continue;
    any_device = true;
    static_cast<void>(device->UpdateInput());
    std::string pressed;
    for (const auto *input : device->Inputs()) {
      if (input->GetState() > 0.5f) {
        if (!pressed.empty())
          pressed += ", ";
        pressed += input->GetName();
        if (pressed.size() > 70) {
          pressed += ", …";
          break;
        }
      }
    }
    ImGui::Text("P%d %s: %s", port + 1, device->GetName().c_str(),
                pressed.empty() ? "—" : pressed.c_str());
  }
  if (!any_device)
    ImGui::TextDisabled("no device selected on any port");
}

void ConfigMenu::ApplyControllerLive() {
  ConfigResult &cfg = *m_hooks.config;
  // Resolve against the live interface, not a list captured at menu open: the
  // device the user just picked may have been plugged in since boot.
  g_controller_interface.RefreshDevices();
  std::string message;
  if (!GenerateControllerConfig(
          m_hooks.user_directory,
          std::span<const std::string>(m_ports.data(), m_ports.size()),
          &message)) {
    // Roll the UI back to the binding that is actually live. Leaving the failed
    // choice in the combo would claim a controller is bound when it is not.
    m_ports = m_applied_ports;
    RefreshDevices();
    std::fprintf(stderr, "[menu] controller config failed: %s\n",
                 message.c_str());
    SetStatus("controller config failed: " + message);
    return;
  }
  // Keep slot positions: port N must stay GCPadN, so empty entries are kept.
  cfg.controllers.assign(m_ports.begin(), m_ports.end());
  cfg.controller.clear();
  for (const auto &value : m_ports) {
    if (!value.empty()) {
      cfg.controller = value;
      break;
    }
  }
  Save("config");
  // Re-read GCPadNew.ini and re-resolve every binding against the device list.
  // This is what removes the old "restart to bind input" requirement: without
  // it the pad keeps the CommandGroup references it built at boot.
  if (Pad::IsInitialized())
    Pad::LoadConfig();
  m_applied_ports = m_ports;
  RefreshDevices();
  std::string live_pad = "(pad not initialized)";
  if (Pad::IsInitialized()) {
    if (const auto *pad = Pad::GetConfig()->GetController(0))
      live_pad = pad->GetDefaultDevice().ToString();
  }
  std::fprintf(stderr, "[menu] controller applied: pad0=%s\n",
               live_pad.c_str());
  SetStatus("controller config applied live");
}

void ConfigMenu::RescanDevices() {
  g_controller_interface.RefreshDevices();
  RefreshDevices();
  SetStatus("device list refreshed");
}

void ConfigMenu::ApplyDisplayLive() {
  // The core is paused while the menu is open, so nothing here takes effect
  // immediately: Config::SetCurrent writes the config layer and the running
  // core picks the values up in CheckForConfigChanges(), which
  // VideoConfig registers on after_frame_event -- i.e. on the first emulated
  // frame after the menu closes. Talented people have tried to poke
  // g_ActiveConfig directly from this thread; it is not worth the crash.
  ConfigResult &cfg = *m_hooks.config;
  const int scale = std::clamp(cfg.dolphin_scale, 1, 12);
  Config::SetCurrent(Config::GFX_EFB_SCALE, scale);
  Config::SetCurrent(Config::GFX_MAX_EFB_SCALE,
                     std::max(scale, Config::Get(Config::GFX_MAX_EFB_SCALE)));
  Config::SetCurrent(Config::GFX_VSYNC, cfg.vsync);
  Config::SetCurrent(Config::MAIN_FULLSCREEN, cfg.fullscreen);
  // Aspect ratio and the widescreen hack are read by CheckForConfigChanges
  // and turned into CONFIG_CHANGE_BIT_ASPECT_RATIO / a geometry rebuild, so
  // they land on the next emulated frame just like the resolution does.
  const int aspect = AspectRatioMode(cfg.aspect_ratio);
  Config::SetCurrent(Config::GFX_ASPECT_RATIO,
                     static_cast<AspectMode>(aspect < 0 ? 0 : aspect));
  Config::SetCurrent(Config::GFX_WIDESCREEN_HACK, cfg.widescreen_hack);
  // GFX_SHOW_FPS is deliberately NOT set: it renders Dolphin's performance
  // overlay. The menu draws its own counter from DrawFrameCounter() instead,
  // reading cfg.show_fps -- so the toggle is live without touching Dolphin's
  // OSD at all.
  // Exclusive fullscreen (Windows) and the Wayland toplevel state cannot be
  // reached through Config at all; both backends expose a setter.
  if (g_gfx)
    g_gfx->SetFullscreen(cfg.fullscreen);
  moderngekko::SetWindowFullscreen(cfg.fullscreen);
  std::fprintf(stderr,
               "[menu] display applied: scale=%d vsync=%d fullscreen=%d "
               "aspect=%d widescreen=%d show_fps=%d\n",
               scale, cfg.vsync ? 1 : 0, cfg.fullscreen ? 1 : 0,
               aspect < 0 ? 0 : aspect, cfg.widescreen_hack ? 1 : 0,
               cfg.show_fps ? 1 : 0);
}

void ConfigMenu::ApplyTexturesLive() {
  // Same contract as ApplyDisplayLive: stage the config layer, let
  // CheckForConfigChanges hand it to TextureCacheBase::OnConfigChanged, which
  // is what re-reads the pack folder (HiresTexture::Update) and invalidates
  // the decoded texture cache when the setting actually flipped.
  ConfigResult &cfg = *m_hooks.config;
  Config::SetCurrent(Config::GFX_HIRES_TEXTURES, cfg.hires_textures);
  Config::SetCurrent(Config::GFX_CACHE_HIRES_TEXTURES, cfg.cache_hires_textures);
  std::fprintf(stderr, "[menu] textures applied: hires=%d cache=%d\n",
               cfg.hires_textures ? 1 : 0, cfg.cache_hires_textures ? 1 : 0);
}

void ConfigMenu::ApplyAudioBackendLive() {
  ConfigResult &cfg = *m_hooks.config;
  Core::System &system = Core::System::GetInstance();
  // Backend selection is currently a constructor argument of the sound stream,
  // so a live change is a full stream swap. Emulation is paused, no AI buffer
  // is in flight, and both calls are the same pair Core uses at boot/shutdown.
  //
  // This runs on the thread that draws the menu, so it blocks drawing until the
  // new device is open: opening PulseAudio/ALSA can take seconds the first
  // time, and the window visibly stops repainting meanwhile. The duration is
  // logged so that cost stays visible instead of looking like a freeze.
  const auto start = std::chrono::steady_clock::now();
  AudioCommon::ShutdownSoundStream(system);
  if (cfg.audio_backend.empty())
  {
    // "(default)": let InitSoundStream pick, which is what an empty config
    // value means at boot too.
    Config::SetCurrent(Config::MAIN_AUDIO_BACKEND, std::string());
  }
  else
  {
    Config::SetCurrent(Config::MAIN_AUDIO_BACKEND, cfg.audio_backend);
  }
  AudioCommon::InitSoundStream(system);
  AudioCommon::PostInitSoundStream(system);
  AudioCommon::UpdateSoundStream(system);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  std::fprintf(stderr, "[menu] audio backend applied: %s (%lld ms)\n",
               cfg.audio_backend.empty() ? "(default)" : cfg.audio_backend.c_str(),
               static_cast<long long>(elapsed.count()));
}

void ConfigMenu::ReportLiveState() {
  static const bool debug = [] {
    const char *value = std::getenv("MODERNGEKKO_MENU_DEBUG");
    return value && *value && *value != '0';
  }();
  if (!debug)
    return;
  // Throttle: this runs on the video thread for every produced frame, and a
  // std::string comparison per frame is not free. Every 8th frame is still
  // fast enough for a test that closes the menu and polls for a line.
  static unsigned counter = 0;
  if (++counter % 8 != 0)
    return;
  const bool forced = g_live_report_dirty.exchange(false, std::memory_order_relaxed);

  // Only ever reports the RUNNING core's view. The menu's own copies (cfg.*,
  // m_ports) are what it WANTS; printing both is the point -- a mismatch means
  // the live apply did not land.
  std::string pad_device = "(pad not initialized)";
  if (Pad::IsInitialized()) {
    if (const auto *pad = Pad::GetConfig()->GetController(0))
      pad_device = pad->GetDefaultDevice().ToString();
  }
  const std::string audio = Config::Get(Config::MAIN_AUDIO_BACKEND);
  const int fullscreen = moderngekko::IsWindowFullscreen() ? 1 : 0;

  struct Snapshot {
    int efb_scale;
    bool vsync;
    int fullscreen;
    int aspect;
    bool widescreen;
    bool hires;
    bool cache_hires;
    std::string audio;
    std::string pad0;
    bool operator==(const Snapshot &) const = default;
  };
  static Snapshot previous{-1, false, -1, -1, false, false, false, std::string(),
                           std::string()};
  const Snapshot current{g_ActiveConfig.iEFBScale,
                         g_ActiveConfig.bVSyncActive,
                         fullscreen,
                         static_cast<int>(g_ActiveConfig.aspect_mode),
                         g_ActiveConfig.bWidescreenHack,
                         g_ActiveConfig.bHiresTextures,
                         g_ActiveConfig.bCacheHiresTextures,
                         audio,
                         pad_device};
  if (current == previous && !forced)
    return;
  previous = current;
  std::fprintf(stderr,
               "[menu-live] efb_scale=%d vsync=%d fullscreen=%d aspect=%d "
               "widescreen=%d hires=%d cache_hires=%d audio=%s pad0=%s\n",
               current.efb_scale, current.vsync ? 1 : 0, current.fullscreen,
               current.aspect, current.widescreen ? 1 : 0,
               current.hires ? 1 : 0, current.cache_hires ? 1 : 0,
               audio.empty() ? "(default)" : audio.c_str(),
               pad_device.c_str());
}

void ConfigMenu::DrawAudioTab() {
  ConfigResult &cfg = *m_hooks.config;
  if (!m_backends_loaded) {
    m_backends_loaded = true;
    m_audio_backends = AudioCommon::GetSoundBackends();
  }

  const char *backend_preview =
      cfg.audio_backend.empty() ? "(default)" : cfg.audio_backend.c_str();
  if (ImGui::BeginCombo("Audio backend", backend_preview)) {
    if (ImGui::Selectable("(default)", cfg.audio_backend.empty())) {
      if (!cfg.audio_backend.empty()) {
        cfg.audio_backend.clear();
        Save("config");
        ApplyAudioBackendLive();
        SetStatus("audio backend applied live");
      }
    }
    LogItemRect("item", "audio/(default)");
    for (const auto &backend : m_audio_backends) {
      const bool selected = backend == cfg.audio_backend;
      if (ImGui::Selectable(backend.c_str(), selected)) {
        if (backend != cfg.audio_backend) {
          cfg.audio_backend = backend;
          Save("config");
          ApplyAudioBackendLive();
          SetStatus("audio backend applied live");
        }
      }
      LogItemRect("item", "audio/" + backend);
      if (selected)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  LogItemRect("widget", "audio");
  Description("Chooses which sound system the emulator talks to. Applies "
              "immediately. If a backend refuses to open, the default is used "
              "instead.");

  auto apply_audio = [&cfg] {
    Config::SetCurrent(Config::MAIN_AUDIO_VOLUME, cfg.volume);
    Config::SetCurrent(Config::MAIN_AUDIO_MUTED, cfg.mute);
    AudioCommon::UpdateSoundStream(Core::System::GetInstance());
  };

  int volume = cfg.volume;
  if (ImGui::SliderInt("Volume", &volume, 0, 100)) {
    cfg.volume = volume;
    apply_audio();
  }
  if (ImGui::Checkbox("Mute", &cfg.mute))
    apply_audio();

  ImGui::Spacing();
  ImGui::TextDisabled(
      "Volume and mute apply immediately.");
}

namespace {

// The fourth character of a six-character GameCube/Wii code is the region
// letter: GBSP8P is PAL, GBSE8P is NTSC-U, GBSJ8P is NTSC-J. Deriving the
// catalogue from the disc that is actually running keeps this generic instead
// of hardcoding one title's ids.
std::string RegionLabel(char region) {
  switch (region) {
  case 'P':
    return "PAL";
  case 'E':
    return "NTSC-U";
  case 'J':
    return "NTSC-J";
  case 'K':
    return "NTSC-K";
  default:
    return std::string("Region ") + region;
  }
}

std::string ModuleSuffix() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

// The extracted game for a disc id, or empty. Two layouts exist: the launcher
// keeps one directory per disc id under <user>/games, and this repo keeps
// extracted/<slug> next to the disc that is running. InspectGame validates a
// candidate by reading boot.bin and main.dol, so scanning a folder of
// unrelated directories costs one hash each and cannot return a non-game.
std::filesystem::path
FindGameRoot(const std::string &disc_id, const std::filesystem::path &current,
             const std::filesystem::path &user_directory) {
  std::vector<std::filesystem::path> bases;
  bases.push_back(user_directory / "games" / disc_id);
  if (!current.empty() && current.has_parent_path())
    bases.push_back(current.parent_path());

  // Read only sys/boot.bin. InspectGame() would also hash the whole files/
  // archive (hundreds of MB) to fill assets_sha256, and the region scan probes
  // several candidate directories per region -- which is why opening the System
  // tab appeared to hang. The disc id lives in the first six bytes of boot.bin,
  // so this is the whole check.
  const auto matches = [&](const std::filesystem::path &path) -> bool {
    std::ifstream boot(path / "sys" / "boot.bin", std::ios::binary);
    if (!boot)
      return false;
    char id[6]{};
    boot.read(id, sizeof(id));
    if (boot.gcount() != static_cast<std::streamsize>(sizeof(id)))
      return false;
    return std::equal(id, id + sizeof(id), disc_id.begin(), disc_id.end()) &&
           std::all_of(id, id + sizeof(id),
                       [](unsigned char c) { return std::isalnum(c) != 0; });
  };

  for (const auto &base : bases) {
    if (matches(base))
      return base;
    // A fresh error_code per base: the first base is usually absent (there is
    // no <user>/games/<id> in the dev layout), and an ec left set by that
    // failed construction would make the `!ec` guard skip the base that does
    // exist -- which is exactly how every region ended up "not installed".
    std::error_code ec;
    // Capped: a game root's parent is normally a two-entry folder, and there
    // is no reason to walk a whole drive looking for one disc id.
    std::size_t seen = 0;
    for (std::filesystem::directory_iterator it(base, ec), end;
         !ec && it != end && seen < 64; it.increment(ec), ++seen) {
      std::error_code type_error;
      if (!it->is_directory(type_error))
        continue;
      if (matches(it->path()))
        return it->path();
    }
  }
  return {};
}

// The recompiled module for a disc id, or empty. The first two probes mirror
// the runner's own auto-resolution (next to the executable, then
// <user>/StaticRecompModules) so that "the menu offers this region" and "the
// runner could boot it" stay the same statement. The third covers the dev
// layout, where the module sits at <root>/<disc_id>/<hash>/<name> beside the
// module that is running.
std::filesystem::path FindModule(
    const std::string &disc_id, const std::string &current_disc_id,
    const std::filesystem::path &executable_directory,
    const std::filesystem::path &user_directory,
    const std::filesystem::path &current_module) {
  const std::string name = "g" + disc_id + "_recomp" + ModuleSuffix();
  std::error_code ec;
  const auto beside = executable_directory / name;
  if (std::filesystem::is_regular_file(beside, ec))
    return beside;
  const auto cached = user_directory / "StaticRecompModules" / name;
  if (std::filesystem::is_regular_file(cached, ec))
    return cached;

  const std::string current_name =
      "g" + current_disc_id + "_recomp" + ModuleSuffix();
  if (current_module.empty() ||
      current_module.filename().string() != current_name)
    return {};
  // Walk up to the directory named after the running disc id. Two shapes exist
  // and both have to work: <root>/<disc id>/<hash>/<so>, and the on-demand
  // layout <root>/<disc id>/<hash>/module-build/<so> -- the second is what the
  // launcher produces, and stopping at a fixed depth made every other region
  // read as "no module" when the running module came from a local build.
  std::filesystem::path id_dir;
  for (std::filesystem::path dir = current_module.parent_path();
       !dir.empty() && !dir.filename().empty(); dir = dir.parent_path()) {
    if (dir.filename().string() == current_disc_id) {
      id_dir = dir;
      break;
    }
  }
  if (id_dir.empty())
    return {};
  const auto root = id_dir.parent_path();
  if (root.empty())
    return {};
  const auto region_dir = root / disc_id;
  const auto direct = region_dir / name;
  if (std::filesystem::is_regular_file(direct, ec))
    return direct;
  std::size_t seen = 0;
  for (std::filesystem::directory_iterator it(region_dir, ec), end;
       !ec && it != end && seen < 16; it.increment(ec), ++seen) {
    std::error_code type_error;
    if (!it->is_directory(type_error))
      continue;
    const auto nested = it->path() / name;
    if (std::filesystem::is_regular_file(nested, ec))
      return nested;
    // ... and the same one level deeper, for a module built in place.
    const auto built = it->path() / "module-build" / name;
    if (std::filesystem::is_regular_file(built, ec))
      return built;
  }
  return {};
}

} // namespace

void ConfigMenu::RefreshRegions() {
  m_regions.clear();
  m_regions_scanned = true;
  m_regions_game_id = m_hooks.disc_id;
  const std::string &current = m_hooks.disc_id;
  // <product><region><disc>: three product characters, the region letter, two
  // more. Without a six-character disc id there is no catalogue to derive and
  // the combo simply does not appear.
  if (current.size() != 6)
    return;

  const std::string product = current.substr(0, 3);
  const std::string tail = current.substr(4);
  if (UiDebugEnabled())
    std::fprintf(stderr,
                 "[menu-ui] region scan: disc=%s game_root=%s user_dir=%s "
                 "exe_dir=%s module=%s\n",
                 current.c_str(),
                 m_hooks.game_root.empty() ? "-"
                                           : m_hooks.game_root.string().c_str(),
                 m_hooks.user_directory.string().c_str(),
                 m_hooks.executable_directory.string().c_str(),
                 m_hooks.module_path.empty() ? "-"
                                             : m_hooks.module_path.c_str());
  // The three regions this title shipped in, always listed so an uninstalled
  // one reads as "not installed" rather than silently missing, plus the
  // running region's letter if it happens to be a fourth.
  std::vector<char> letters = {'P', 'E', 'J'};
  if (std::find(letters.begin(), letters.end(), current[3]) == letters.end())
    letters.push_back(current[3]);

  for (char letter : letters) {
    RegionChoice choice;
    choice.disc_id = product + letter + tail;
    choice.label = RegionLabel(letter);
    choice.game_root = FindGameRoot(choice.disc_id, m_hooks.game_root,
                                     m_hooks.user_directory);
    choice.module = FindModule(choice.disc_id, current,
                               m_hooks.executable_directory,
                               m_hooks.user_directory, m_hooks.module_path);
    if (UiDebugEnabled())
      std::fprintf(stderr,
                   "[menu-ui] region=%s root=%s module=%s installed=%d\n",
                   choice.disc_id.c_str(),
                   choice.game_root.empty() ? "-"
                                            : choice.game_root.string().c_str(),
                   choice.module.empty() ? "-"
                                         : choice.module.string().c_str(),
                   static_cast<int>(!choice.game_root.empty() &&
                                    !choice.module.empty()));
    m_regions.push_back(std::move(choice));
  }
}

namespace {

// Where a player's disc images actually live. Documents is the launcher's
// default; Downloads is where a browser puts them, which on this machine (and
// most others) is where they end up.
std::vector<std::filesystem::path> ImageDirectories() {
  std::vector<std::filesystem::path> directories;
#if defined(_WIN32)
  const char *home = std::getenv("USERPROFILE");
#else
  const char *home = std::getenv("HOME");
#endif
  if (!home)
    return directories;
  directories.push_back(std::filesystem::path(home) / "Documents");
  directories.push_back(std::filesystem::path(home) / "Downloads");
  return directories;
}

// sys/boot.bin holds everything this tab needs to name a version: the six-byte
// disc id at offset 0 and the game title at 0x20. Reading those costs one open
// and one read; InspectGame() would additionally SHA-256 the whole files/
// archive, which is why the old scan made this tab look frozen.
bool ReadBootIdentity(const std::filesystem::path &root, std::string *disc_id,
                      std::string *name) {
  std::ifstream boot(root / "sys" / "boot.bin", std::ios::binary);
  if (!boot)
    return false;
  std::array<char, 0x60> header{};
  boot.read(header.data(), header.size());
  if (boot.gcount() != static_cast<std::streamsize>(header.size()))
    return false;
  std::string id(header.data(), 6);
  if (id.size() != 6 ||
      !std::all_of(id.begin(), id.end(),
                   [](unsigned char c) { return std::isalnum(c) != 0; }))
    return false;
  std::string title(header.data() + 0x20, 0x40);
  if (const auto terminator = title.find('\0'); terminator != std::string::npos)
    title.resize(terminator);
  while (!title.empty() && std::isspace(static_cast<unsigned char>(title.back())))
    title.pop_back();
  if (title.empty())
    title = id;
  *disc_id = std::move(id);
  *name = std::move(title);
  return true;
}

// The launcher is the binary that carries Dolphin's disc reader, so it is what
// `--extract` has to run. It sits beside the runner in every packaged layout;
// the extra names cover the dev tree and the Windows/macOS builds.
std::filesystem::path LauncherBeside(const std::filesystem::path &directory) {
  static const char *kNames[] = {"Beach Spikers", "Beach Spikers.exe",
                                 "moderngekko-launcher",
                                 "moderngekko-launcher.exe", "beach-spikers"};
  std::error_code ec;
  for (const char *name : kNames) {
    const auto candidate = directory / name;
    if (std::filesystem::is_regular_file(candidate, ec))
      return candidate;
  }
  return {};
}

} // namespace

void ConfigMenu::ScanVersions() {
  m_versions.clear();
  m_versions_scanned = true;
  m_versions_game_id = m_hooks.disc_id;
  const auto consider = [&](const std::filesystem::path &root) {
    std::string disc_id;
    std::string name;
    if (!ReadBootIdentity(root, &disc_id, &name))
      return;
    // One row per disc id: the running root is considered first, so a second
    // directory holding the same version does not show up as a duplicate.
    for (const auto &existing : m_versions) {
      if (existing.disc_id == disc_id)
        return;
    }
    GameVersionEntry entry;
    entry.disc_id = std::move(disc_id);
    entry.name = std::move(name);
    entry.game_root = root;
    entry.module = FindModule(entry.disc_id, m_hooks.disc_id,
                              m_hooks.executable_directory,
                              m_hooks.user_directory, m_hooks.module_path);
    m_versions.push_back(std::move(entry));
  };

  // The running game belongs in the list even when it lives outside
  // <user>/games (dev tree, or a --game argument).
  if (!m_hooks.game_root.empty())
    consider(m_hooks.game_root);
  std::error_code ec;
  const auto games = m_hooks.user_directory / "games";
  for (std::filesystem::directory_iterator it(games, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code type_error;
    if (!it->is_directory(type_error))
      continue;
    consider(it->path());
  }
  std::sort(m_versions.begin(), m_versions.end(),
            [](const GameVersionEntry &a, const GameVersionEntry &b) {
              return a.disc_id < b.disc_id;
            });
}

void ConfigMenu::ScanInstallImages() {
  m_install_images.clear();
  m_install_images_scanned = true;
  // Same rule as the launcher's Browse list: disc images live in a shallow
  // folder, so cap the walk instead of scanning a whole drive.
  for (const auto &directory : ImageDirectories()) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec))
      continue;
    std::filesystem::recursive_directory_iterator it(
        directory, std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
      if (it.depth() > 4)
        it.disable_recursion_pending();
      std::error_code file_error;
      if (it->is_regular_file(file_error)) {
        std::string extension = it->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) {
                         return static_cast<char>(std::tolower(c));
                       });
        if (extension == ".iso" || extension == ".wbfs" || extension == ".rvz")
          m_install_images.push_back(it->path());
      }
      it.increment(ec);
      if (ec)
        ec.clear();
    }
  }
  std::sort(m_install_images.begin(), m_install_images.end());
}

void ConfigMenu::InstallVersion(const std::filesystem::path &image) {
  if (!m_install)
    m_install = std::make_shared<InstallState>();
  if (m_install->running.load())
    return;
  const auto launcher = LauncherBeside(m_hooks.executable_directory);
  if (launcher.empty()) {
    SetStatus("no launcher next to the game - extract from the launcher window");
    std::fprintf(stderr,
                 "[menu] install: no launcher found in %s\n",
                 m_hooks.executable_directory.string().c_str());
    return;
  }
  // Quoted for both shells: the paths routinely contain spaces ("Beach
  // Spikers", "Beach Spikers - Virtua Beach Volleyball (USA).rvz").
  const std::string command =
      "\"" + launcher.string() + "\" --extract \"" + image.string() + "\"";
  auto state = m_install;
  {
    std::lock_guard lock(state->mutex);
    state->image = image.filename().string();
  }
  state->finished.store(false);
  state->running.store(true);
  std::fprintf(stderr, "[menu] install: %s\n", command.c_str());
  SetStatus("extracting " + image.filename().string() + " - this takes a few minutes");
  // Detached on purpose: extraction can outlive a quit request, and the state
  // it writes is owned by a shared_ptr, not by this object.
  std::thread([state, command] {
    const int rc = std::system(command.c_str());
    state->exit_code = rc;
    state->running.store(false);
    state->finished.store(true);
  }).detach();
}

std::string ConfigMenu::RegionSwitchGameRoot() const {
  std::lock_guard lock(m_region_mutex);
  return m_region_game_root;
}

std::string ConfigMenu::RegionSwitchModule() const {
  std::lock_guard lock(m_region_mutex);
  return m_region_module;
}

void ConfigMenu::DrawSystemTab() {
  ImGui::Text("Game: %s [%s]", m_hooks.game_name.c_str(),
              m_hooks.disc_id.c_str());
  ImGui::TextWrapped("User directory: %s",
                     m_hooks.user_directory.string().c_str());
  // The module path is a 150-character hash directory; wrapped it costs three
  // lines of the panel to say nothing a tooltip cannot say on hover.
  const std::string module_name =
      m_hooks.module_path.empty() ?
          "(auto)" :
          std::filesystem::path(m_hooks.module_path).filename().string();
  ImGui::Text("Module: %s", module_name.c_str());
  if (!m_hooks.module_path.empty() && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", m_hooks.module_path.c_str());
  ImGui::Spacing();
  ImGui::Separator();

  // --- GameCube system language -----------------------------------------
  // EXI_DeviceIPL writes MAIN_GC_LANGUAGE into the emulated SRAM settings
  // block during boot, so this is a restart-only setting: the disc has
  // already read the old value by the time any menu can open.
  ConfigResult &cfg = *m_hooks.config;
  ImGui::TextDisabled("GameCube system language");
  const auto &languages = SupportedLanguages();
  int language_current = 0;
  for (int i = 0; i < static_cast<int>(languages.size()); ++i) {
    if (languages[static_cast<std::size_t>(i)].text == cfg.language) {
      language_current = i;
      break;
    }
  }
  if (ImGui::BeginCombo("Language", languages[language_current].text)) {
    for (int i = 0; i < static_cast<int>(languages.size()); ++i) {
      const auto &option = languages[static_cast<std::size_t>(i)];
      if (ImGui::Selectable(option.text, i == language_current)) {
        cfg.language = option.text;
        Save("config");
        std::fprintf(stderr, "[menu] language saved: %s (restart required)\n",
                     option.text);
        SetStatus(std::string("language saved: ") + option.text +
                  " — needs a restart to reach the game");
      }
      LogItemRect("item", std::string("language/") + option.text);
      if (i == language_current)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  LogItemRect("widget", "language");
  Description("Written to the emulated console's SRAM at boot: restart to "
              "apply. NTSC discs usually ship English only.");

  ImGui::Spacing();
  ImGui::Separator();

  // --- Region ------------------------------------------------------------
  // The disc id's fourth character is the region letter, so the catalogue is
  // derived from whatever is running instead of hardcoded. Switching is a
  // restart: the other region's disc is a different game root and a different
  // recompiled module, and both only get loaded by the argv replay in RunMain.
  if (m_hooks.disc_id != m_regions_game_id || !m_regions_scanned)
    RefreshRegions();
  if (!m_regions.empty()) {
    ImGui::TextDisabled("Region");
    std::string current_label = RegionLabel(
        m_hooks.disc_id.size() == 4 ? m_hooks.disc_id[3] : '?');
    for (const auto &choice : m_regions) {
      if (choice.disc_id == m_hooks.disc_id) {
        current_label = choice.label;
        break;
      }
    }
    if (ImGui::BeginCombo("Region", current_label.c_str())) {
      for (const auto &choice : m_regions) {
        const bool installed = !choice.game_root.empty() && !choice.module.empty();
        const bool selected = choice.disc_id == m_hooks.disc_id;
        // An uninstalled region is a disabled row, not a selection that would
        // boot-fail: the game root and the module are both required to start.
        if (!installed) {
          ImGui::Selectable(
              (choice.label + "  (" + choice.disc_id + ", not installed)")
                  .c_str(),
              false, ImGuiSelectableFlags_Disabled);
        } else if (ImGui::Selectable(
                       (choice.label + "  (" + choice.disc_id + ")").c_str(),
                       selected)) {
          if (!selected) {
            std::lock_guard lock(m_region_mutex);
            m_region_game_root = choice.game_root.string();
            m_region_module = choice.module.string();
            m_region_switch.store(true);
            // Both flags: WantsRestart() gates the execv block, and a restart
            // without the region flag would replay the original argv.
            m_restart.store(true);
            std::fprintf(stderr,
                         "[menu] region switch requested: %s -> %s\n",
                         m_hooks.disc_id.c_str(), choice.disc_id.c_str());
            SetStatus("switching to " + choice.label + "…");
            if (m_runtime)
              m_runtime->RequestStop();
          }
        }
        LogItemRect("item", "region/" + choice.disc_id);
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    LogItemRect("widget", "region");
    ImGui::TextDisabled("Restarts; savestates are per-region.");
  }

  ImGui::Spacing();
  ImGui::Separator();

  // --- Installed versions ------------------------------------------------
  // The combo above is the catalogue of retail regions; this is what is on
  // disk. They are not the same list: a second revision or a dev extract is a
  // version the catalogue cannot predict, and it should still be switchable.
  if (m_install && m_install->finished.exchange(false))
    m_versions_scanned = false;
  if (!m_versions_scanned || m_versions_game_id != m_hooks.disc_id)
    ScanVersions();
  ImGui::TextDisabled("Installed versions");
  ImGui::SameLine();
  if (ImGui::SmallButton("Rescan")) {
    m_versions_scanned = false;
    m_install_images_scanned = false;
  }
  // A table, not a scrolling child box: with two or three extractions (the
  // normal case) a scrollbar was decoration that made the list look cut off, and
  // the rows read better when the action column lines up.
  if (ImGui::BeginTable("##installed_versions", 2,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch,
                            0.70f);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch,
                            0.30f);
    for (const auto &entry : m_versions) {
      ImGui::PushID(entry.disc_id.c_str());
      const bool running = entry.disc_id == m_hooks.disc_id;
      const char region_letter =
          entry.disc_id.size() == 6 ? entry.disc_id[3] : '?';
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%s [%s - %s]", entry.name.c_str(), entry.disc_id.c_str(),
                  RegionLabel(region_letter).c_str());
      ImGui::TableSetColumnIndex(1);
      if (running) {
        ImGui::TextDisabled("running");
      } else if (entry.module.empty()) {
        ImGui::TextDisabled("no module - build it in the launcher");
      } else if (ImGui::SmallButton("Switch")) {
        // Rect logged like every other actionable widget in this tab: the
        // harness asserts controls fit the panel, and a click test needs the
        // real coordinates instead of eyeballed ones.
        std::lock_guard lock(m_region_mutex);
        m_region_game_root = entry.game_root.string();
        m_region_module = entry.module.string();
        m_region_switch.store(true);
        m_restart.store(true);
        std::fprintf(stderr,
                     "[menu] version switch requested: %s -> %s\n",
                     m_hooks.disc_id.c_str(), entry.disc_id.c_str());
        SetStatus("switching to " + entry.name + " (" + entry.disc_id + ")…");
        if (m_runtime)
          m_runtime->RequestStop();
      }
      if (!running && !entry.module.empty())
        LogItemRect("widget", "switch-" + entry.disc_id);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // Installing needs Dolphin's disc reader, which only the launcher links, so
  // the row spawns the launcher's `--extract` mode in the background.
  ImGui::Spacing();
  ImGui::TextDisabled("Install another version");
  if (m_install && m_install->running.load()) {
    std::string image;
    {
      std::lock_guard lock(m_install->mutex);
      image = m_install->image;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Extracting %s…", image.c_str());
    ImGui::PopStyleColor();
  } else {
    if (!m_install_images_scanned)
      ScanInstallImages();
    if (m_install_images.empty()) {
      ImGui::TextDisabled("No ISO, WBFS or RVZ found in Documents or Downloads.");
    } else if (ImGui::BeginTable("##install_images", 2,
                                 ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Image", ImGuiTableColumnFlags_WidthStretch,
                              0.70f);
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch,
                              0.30f);
      for (const auto &image : m_install_images) {
        ImGui::PushID(image.string().c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(image.filename().string().c_str());
        ImGui::TableSetColumnIndex(1);
        if (ImGui::SmallButton("Extract"))
          InstallVersion(image);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }

  ImGui::Spacing();
  ImGui::Separator();

  if (ImGui::Button("Save config")) {
    std::fprintf(stderr, "[menu] button: Save config\n");
    Save("config");
  }
  LogItemRect("widget", "saveconfig");
  if (ImGui::Button("Save && restart")) {
    std::fprintf(stderr, "[menu] button: Save && restart\n");
    Save("config");
    m_restart.store(true);
    if (m_runtime)
      m_runtime->RequestStop();
  }
  LogItemRect("widget", "restart");
  ImGui::SameLine();
  if (ImGui::Button("Quit without saving")) {
    std::fprintf(stderr, "[menu] button: Quit without saving\n");
    if (m_runtime)
      m_runtime->RequestStop();
  }
  LogItemRect("widget", "quit");
  ImGui::Spacing();
  ImGui::TextDisabled("F1 or ESC closes this menu and resumes the game.");
}

// Savestates and machine-level actions live on their own tab: the menu window
// is height-clamped (it has to fit inside whatever backbuffer the game is
// running at), and keeping all of this under System pushed the buttons past the
// bottom edge where they could not be clicked at all.
void ConfigMenu::DrawStatesTab() {
  ConfigResult &cfg = *m_hooks.config;

  // --- Savestates ---------------------------------------------------------
  ImGui::TextDisabled("Savestates");
  int slot = std::clamp(cfg.state_slot, 1, 10);
  if (ImGui::SliderInt("Slot", &slot, 1, 10, "%d",
                       ImGuiSliderFlags_AlwaysClamp)) {
    cfg.state_slot = slot;
    // Only persist when the drag ends: SliderInt reports a change every
    // frame while the mouse is down, and writing config.ini at 60 Hz is a
    // silly way to wear out an SSD.
  }
  if (ImGui::IsItemDeactivatedAfterEdit())
    Save("config");
  LogItemRect("widget", "slot");
  const std::string slot_info = State::GetInfoStringOfSlot(slot, false);
  ImGui::SameLine();
  ImGui::TextDisabled("%s", slot_info == "Empty" ? "empty" : slot_info.c_str());

  if (ImGui::Button("Save state")) {
    std::fprintf(stderr, "[menu] button: Save state (slot %d)\n", slot);
    Hide();
    QueueHostAction(HostAction::SaveState);
    SetStatus("saving state to slot " + std::to_string(slot) + "…");
  }
  LogItemRect("widget", "savestate");
  ImGui::SameLine();
  if (ImGui::Button("Load state")) {
    std::fprintf(stderr, "[menu] button: Load state (slot %d)\n", slot);
    Hide();
    QueueHostAction(HostAction::LoadState);
    SetStatus("loading state from slot " + std::to_string(slot) + "…");
  }
  LogItemRect("widget", "loadstate");
  ImGui::TextWrapped(
      "Both close the menu and run after emulation resumes — a savestate is "
      "written by the CPU thread, and doing that while the menu holds the "
      "core paused is how you get a hang instead of a save.");

  ImGui::Spacing();
  ImGui::Separator();

  // --- Reset / screenshot -------------------------------------------------
  ImGui::TextDisabled("Machine");
  if (ImGui::Button("Reset game")) {
    std::fprintf(stderr, "[menu] button: Reset game\n");
    Hide();
    QueueHostAction(HostAction::Reset);
    SetStatus("resetting the console");
  }
  LogItemRect("widget", "reset");
  ImGui::SameLine();
  if (ImGui::Button("Screenshot")) {
    std::fprintf(stderr, "[menu] button: Screenshot\n");
    Hide();
    QueueHostAction(HostAction::Screenshot);
    SetStatus("screenshot requested");
  }
  LogItemRect("widget", "screenshot");
  ImGui::TextWrapped(
      "Reset is the console's reset button, not a reboot: the disc restarts "
      "from the boot vector. Screenshots land in the user directory under "
      "ScreenShots, one folder per game ID.");
  ImGui::TextDisabled("HD texture folder: %s", TextureDirectory().c_str());
}

void ConfigMenu::RefreshCheats() {
  m_cheats.clear();
  const std::string &id = m_hooks.disc_id;
  m_cheats_ini_path = File::GetUserPath(D_GAMESETTINGS_IDX) + id + ".ini";
  if (id.empty())
    return;

  SConfig &sconfig = SConfig::GetInstance();
  const Common::IniFile global = sconfig.LoadDefaultGameIni();
  const Common::IniFile local = sconfig.LoadLocalGameIni();

  // Same loaders the core uses at boot, so the tab shows exactly what the code
  // handler would run. Names can appear twice once a local copy of a shipped
  // code exists; keep the first so the list does not sprout duplicates.
  std::unordered_set<std::string> seen;
  for (const Gecko::GeckoCode &code : Gecko::LoadCodes(global, local)) {
    if (!seen.insert(code.name).second)
      continue;
    m_cheats.push_back({code.name, false, code.enabled});
  }
  for (const ActionReplay::ARCode &code : ActionReplay::LoadCodes(global, local)) {
    if (!seen.insert(code.name).second)
      continue;
    m_cheats.push_back({code.name, true, code.enabled});
  }
  std::fprintf(stderr, "[menu] cheats: %zu code(s) for %s (%s)\n", m_cheats.size(),
               id.c_str(), m_cheats_ini_path.c_str());
  // Index -> name, so a harness (or a bug report) can bind a widget id to a
  // code without guessing the order of an INI it cannot see.
  if (UiDebugEnabled())
  {
    for (std::size_t i = 0; i < m_cheats.size(); ++i)
    {
      std::fprintf(stderr, "[menu] cheat %zu = %s (%s)\n", i, m_cheats[i].name.c_str(),
                   m_cheats[i].action_replay ? "ar" : "gecko");
    }
  }
}

void ConfigMenu::ApplyCheatsLive() {
  ConfigResult &cfg = *m_hooks.config;
  const std::string &id = m_hooks.disc_id;

  // Gates the code lists AND the code handler install, so it goes first.
  Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS, cfg.enable_cheats);
  if (id.empty()) {
    SetStatus("no disc id — cheats are stored per game");
    return;
  }

  SConfig &sconfig = SConfig::GetInstance();
  Common::IniFile global = sconfig.LoadDefaultGameIni();
  Common::IniFile local = sconfig.LoadLocalGameIni();
  std::vector<Gecko::GeckoCode> gecko = Gecko::LoadCodes(global, local);
  std::vector<ActionReplay::ARCode> action_replay =
      ActionReplay::LoadCodes(global, local);

  // The tab is the authoritative toggle state for this session; the ini is just
  // where it gets stored.
  for (Gecko::GeckoCode &code : gecko) {
    for (const CheatListEntry &entry : m_cheats) {
      if (!entry.action_replay && entry.name == code.name)
        code.enabled = entry.enabled;
    }
  }
  for (ActionReplay::ARCode &code : action_replay) {
    for (const CheatListEntry &entry : m_cheats) {
      if (entry.action_replay && entry.name == code.name)
        code.enabled = entry.enabled;
    }
  }

  // Only the Enabled/Disabled *lists* are written back: SaveCodes skips the
  // body of a code that is not user_defined, so the codes shipped in
  // Sys/GameSettings stay the only copy on disk.
  Gecko::SaveCodes(local, gecko);
  ActionReplay::SaveCodes(&local, action_replay);
  const std::string path =
      File::GetUserPath(D_GAMESETTINGS_IDX) + id + ".ini";
  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path(path).parent_path(), ec);
  const bool written = local.Save(path);

  // No CPUThreadGuard here on purpose: these two only swap the active lists.
  // Gecko marks the code handler uninstalled and the next emulated frame
  // installs it, which is why the setting is safe to change while paused.
  const u16 revision = sconfig.GetRevision();
  Gecko::SetActiveCodes(gecko, id, revision);
  ActionReplay::ApplyCodes(action_replay, id, revision);

  const auto count_on = [](const auto &codes) {
    return std::count_if(codes.begin(), codes.end(),
                         [](const auto &code) { return code.enabled; });
  };
  const auto gecko_on = count_on(gecko);
  const auto ar_on = count_on(action_replay);
  std::fprintf(stderr,
               "[menu] cheats applied: gecko=%zu/%zu ar=%zu/%zu enabled=%d "
               "file=%s%s\n",
               static_cast<std::size_t>(gecko_on), gecko.size(),
               static_cast<std::size_t>(ar_on), action_replay.size(),
               cfg.enable_cheats ? 1 : 0, path.c_str(),
               written ? "" : " (write failed)");
  if (!written)
    SetStatus("couldn't write " + path);
}

void ConfigMenu::DrawCheatsTab() {
  ConfigResult &cfg = *m_hooks.config;
  const std::string &id = m_hooks.disc_id;

  // The header and the master switch are pinned to the top and the buttons to
  // the bottom; only the code list itself scrolls. The panel is height-clamped
  // to the game window, and on the build's own 720x492 backbuffer the previous
  // "everything in one child" layout put the last row at y=443 and Rescan at
  // y=470 in a panel that ends at 482 -- both outside the window's clip rect,
  // so ImGui reported hover=0 forever and neither could be clicked. A list that
  // grows with the player's INI needs a scrollbar, not more vertical room.
  ImGui::TextDisabled("Gecko / Action Replay codes for %s",
                      id.empty() ? "(no disc)" : id.c_str());
  ImGui::Spacing();

  if (ImGui::Checkbox("Enable cheats", &cfg.enable_cheats)) {
    Save("config");
    ApplyCheatsLive();
    SetStatus(cfg.enable_cheats
                  ? "cheats enabled — ticked codes run every frame"
                  : "cheats disabled — nothing is injected");
  }
  LogItemRect("widget", "enable-cheats");
  ImGui::SameLine();
  ImGui::TextDisabled("master switch");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Codes are executed by Dolphin's code handler inside the game, not by "
        "this menu: a ticked code applies on the next emulated frame and keeps "
        "writing its values every frame. Unticking it stops the writes, but "
        "whatever it already changed stays until the game rewrites it -- reset "
        "if you want a clean slate.");
  }

  if (id.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("Open a disc image first; codes live in a per-game "
                       "file, so there is nothing to list yet.");
    return;
  }

  // One read per disc id, then only when the player asks for a rescan: the
  // ini is small, but re-parsing it every frame at 60 Hz is rude.
  if (!m_cheats_scanned || m_cheats_game_id != id) {
    m_cheats_game_id = id;
    m_cheats_scanned = true;
    RefreshCheats();
  }

  const std::size_t enabled_count = static_cast<std::size_t>(std::count_if(
      m_cheats.begin(), m_cheats.end(),
      [](const CheatListEntry &entry) { return entry.enabled; }));
  ImGui::SameLine();
  ImGui::TextDisabled("· %zu of %zu on", enabled_count, m_cheats.size());

  ImGui::Spacing();
  ImGui::Separator();

  // Reserve the footer before opening the child, so the buttons and the file
  // path keep their place no matter how many codes the INI holds.
  //
  // The panel's bottom band is where the paused game shows through, and
  // menu-shot requires every tab to leave it byte-identical: whatever is there
  // is the game plus the panel tint, so one tab painting text into it breaks
  // the cross-tab comparison. ImGui's window padding clears only a few of those
  // pixels, so the footer needs an explicit margin on top of its two rows --
  // without it the `File:` line landed at y=461..478 against a band of
  // 470..480 and Cheats was the only tab that spilled.
  const float kBottomClearance = 12.0f;
  const float footer_height = kBottomClearance +
                              ImGui::GetFrameHeightWithSpacing() +
                              ImGui::GetTextLineHeightWithSpacing();
  ImGui::BeginChild("cheats-body", ImVec2(0.0f, -footer_height), ImGuiChildFlags_None);
  // Denser rows inside the list only: a 19 px checkbox at the default padding
  // plus 4 px of spacing made thirteen codes taller than the panel. The text
  // itself is unchanged -- only the box padding and the gap shrink.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 1.0f));

  if (m_cheats.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("No codes for this disc yet.");
    if (ImGui::Button("Create codes file")) {
      std::error_code ec;
      const std::filesystem::path path(m_cheats_ini_path);
      std::filesystem::create_directories(path.parent_path(), ec);
      if (!std::filesystem::exists(path)) {
        std::ofstream file(path);
        file << "# " << id << " — Gecko / Action Replay codes\n"
             << "\n[Gecko]\n"
             << "$Example Code\n"
             << "# replace these lines with real codes, then Rescan\n";
      }
      RefreshCheats();
      SetStatus(ec ? "can't create " + path.string() + ": " + ec.message()
                   : "codes file ready — paste codes under [Gecko]");
      std::fprintf(stderr, "[menu] cheat file: %s (%s)\n",
                   m_cheats_ini_path.c_str(),
                   ec ? ec.message().c_str() : "ready");
    }
    LogItemRect("widget", "create-cheats");
  } else {
    bool ar_heading_drawn = false;
    bool gecko_heading_drawn = false;
    for (std::size_t i = 0; i < m_cheats.size(); ++i) {
      CheatListEntry &entry = m_cheats[i];
      if (entry.action_replay) {
        if (!ar_heading_drawn) {
          ar_heading_drawn = true;
          ImGui::TextDisabled("Action Replay");
        }
      } else if (!gecko_heading_drawn) {
        gecko_heading_drawn = true;
        ImGui::TextDisabled("Gecko");
      }
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Checkbox(entry.name.c_str(), &entry.enabled)) {
        std::fprintf(stderr, "[menu] cheat: %s %s\n",
                     entry.enabled ? "on" : "off", entry.name.c_str());
        ApplyCheatsLive();
        SetStatus((entry.enabled ? "enabled " : "disabled ") + entry.name);
      }
      // Only rows that are actually on screen get a rect: the harness clicks
      // item centres, and a rect clipped out of view would aim a click at
      // whatever happens to be under those coordinates.
      if (ImGui::IsItemVisible())
        LogItemRect("widget", "cheat-" + std::to_string(i));
      ImGui::PopID();
    }
  }
  ImGui::PopStyleVar(2);
  ImGui::EndChild();

  if (ImGui::Button("Rescan")) {
    RefreshCheats();
    SetStatus(std::to_string(m_cheats.size()) + " code(s) loaded");
  }
  LogItemRect("widget", "rescan-cheats");
  ImGui::SameLine();
  ImGui::TextDisabled("%zu enabled", enabled_count);
  ImGui::TextDisabled("File: %s", m_cheats_ini_path.c_str());
}

void ConfigMenu::DrawTexturesTab() {
  ConfigResult &cfg = *m_hooks.config;
  ImGui::TextDisabled("Custom HD texture packs (PNG / DDS)");
  ImGui::Spacing();

  if (ImGui::Checkbox("Load HD texture packs", &cfg.hires_textures)) {
    Save("config");
    ApplyTexturesLive();
    std::fprintf(stderr, "[menu] textures: hires=%d cache=%d\n",
                 cfg.hires_textures ? 1 : 0,
                 cfg.cache_hires_textures ? 1 : 0);
    SetStatus(cfg.hires_textures
                  ? "HD textures enabled — the cache reloads on the next frame"
                  : "HD textures disabled — back to the disc's own textures");
  }
  LogItemRect("widget", "hires");
  Description("Replaces the disc's own textures with higher-resolution packs "
              "found in the folder below. Applies immediately.");

  ImGui::BeginDisabled(!cfg.hires_textures);
  if (ImGui::Checkbox("Cache decoded textures in RAM",
                      &cfg.cache_hires_textures)) {
    Save("config");
    ApplyTexturesLive();
    std::fprintf(stderr, "[menu] textures: hires=%d cache=%d\n",
                 cfg.hires_textures ? 1 : 0,
                 cfg.cache_hires_textures ? 1 : 0);
    SetStatus(cfg.cache_hires_textures
                  ? "texture caching enabled — watch your RAM"
                  : "texture caching disabled");
  }
  LogItemRect("widget", "hirecache");
  Description("Keeps decoded textures in RAM instead of re-reading them from "
              "disk. A large pack can hold several gigabytes, and this machine "
              "also has to feed the game, so leave it off unless your packs "
              "are small. Applies immediately.");
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::Separator();

  const std::string directory = TextureDirectory();
  ImGui::TextWrapped("Folder: %s", directory.c_str());
  if (!m_textures_scanned) {
    m_textures_scanned = true;
    RefreshTexturePacks();
  }
  if (ImGui::Button("Rescan folder")) {
    RefreshTexturePacks();
    SetStatus(std::to_string(m_texture_files) + " HD texture files found");
  }
  LogItemRect("widget", "rescan-textures");
  ImGui::SameLine();
  if (ImGui::Button("Create folder")) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    RefreshTexturePacks();
    std::fprintf(stderr, "[menu] texture folder: %s (%s)\n", directory.c_str(),
                 ec ? ec.message().c_str() : "ready");
    SetStatus(ec ? "can't create " + directory + ": " + ec.message()
                 : "folder ready — drop tex1_*.png packs in there");
  }
  LogItemRect("widget", "create-textures");
  ImGui::SameLine();
  ImGui::TextDisabled("%zu file%s", m_texture_files,
                      m_texture_files == 1 ? "" : "s");

  ImGui::Spacing();
  ImGui::TextDisabled(
      "Packs are named tex1_<size>x<size>_<hash>_<format>.png, in this folder "
      "or in a subfolder. The folder is only re-read when the loader setting "
      "flips, so after adding files toggle \"Load HD texture packs\" off and "
      "on (or restart).");
  if (!m_texture_scan_dir.empty() && m_texture_scan_dir != directory)
    ImGui::TextDisabled("Actually reading: %s", m_texture_scan_dir.c_str());
}

std::string ConfigMenu::TextureDirectory() const {
  std::string game_id = SConfig::GetInstance().GetGameID();
  if (game_id.empty())
    game_id = m_hooks.disc_id;
  return File::GetUserPath(D_HIRESTEXTURES_IDX) + game_id;
}

void ConfigMenu::RefreshTexturePacks() {
  m_texture_files = 0;
  m_texture_scan_dir.clear();
  std::string game_id = SConfig::GetInstance().GetGameID();
  if (game_id.empty())
    game_id = m_hooks.disc_id;
  const std::string root = File::GetUserPath(D_HIRESTEXTURES_IDX);
  // Ask Dolphin which folders it will actually read: <root>/<game id>, the
  // region-free <root>/<first three chars>, plus any "all" / per-game
  // directories listed in a .txt there.
  const std::set<std::string> directories =
      GetTextureDirectoriesWithGameId(root, game_id);
  if (directories.empty()) {
    m_texture_scan_dir = root + game_id;
    return;
  }
  m_texture_scan_dir = *directories.begin();
  for (const std::string &directory : directories) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(directory, ec), end;
         it != end && !ec; it.increment(ec)) {
      if (!it->is_regular_file(ec))
        continue;
      const std::string extension = it->path().extension().string();
      if (extension == ".png" || extension == ".dds")
        ++m_texture_files;
    }
  }
}

void ConfigMenu::RefreshDevices() {
  m_choices.clear();
  m_choices.push_back({"(none)", ""});

  const auto devices = g_controller_interface.GetAllDevices();
  std::vector<std::shared_ptr<ciface::Core::Device>> keyboards;
  std::unordered_map<std::string, int> name_count;
  for (const auto &device : devices) {
    if (device && !device->Inputs().empty())
      ++name_count[device->GetName()];
  }
  std::unordered_set<std::string> seen_values;
  for (const auto &device : devices) {
    if (!device || device->Inputs().empty())
      continue;
    if (InputLooksLikeKeyboard(*device)) {
      keyboards.push_back(device);
      continue;
    }
    const std::string qualified = device->GetQualifiedName();
    if (!seen_values.insert(qualified).second)
      continue;
    std::string display = device->GetName();
    if (name_count[device->GetName()] > 1)
      display += " [" + qualified + "]";
    m_choices.push_back({std::move(display), qualified});
  }
  for (const auto &keyboard : keyboards) {
    const std::string qualified = keyboard->GetQualifiedName();
    m_choices.push_back({"Keyboard (" + keyboard->GetName() + ")",
                         "Keyboard|" + qualified});
  }
}

void ConfigMenu::Save(const std::string &what) {
  std::string error;
  if (SaveConfig(m_hooks.user_directory, *m_hooks.config, &error))
    SetStatus(what + " saved");
  else
    SetStatus("save failed: " + error);
}

void ConfigMenu::SetStatus(std::string text) {
  std::lock_guard lock(m_status_mutex);
  m_status = std::move(text);
}

} // namespace moderngekko::frontend
