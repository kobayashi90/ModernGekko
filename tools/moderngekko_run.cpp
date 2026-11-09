#include "cache_affinity.hpp"
#include "config_menu.hpp"
#include "frontend_config.hpp"
#include "dol_patch.hpp"
#include "moderngekko/game.hpp"
#include "moderngekko/runtime.hpp"
#include "netplay_session.hpp"

#include "Common/Config/Config.h"
#include "Common/StringUtil.h"
#include "Core/Config/CheatSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "VideoCommon/VideoConfig.h"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <unistd.h>
#endif

#if defined(_WIN32)
// For the affinity/priority setup in main(). WIN32_LEAN_AND_MEAN keeps the
// winsock and GDI surface out of a translation unit that only needs the
// processor-topology and process calls.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
#ifndef MODERNGEKKO_RUNNER_NAME
#define MODERNGEKKO_RUNNER_NAME "moderngekko-run"
#endif
#ifndef MODERNGEKKO_LAUNCHER_FILENAME
#define MODERNGEKKO_LAUNCHER_FILENAME "ModernGekko"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

volatile std::sig_atomic_t s_stop_requested = 0;

void HandleStopSignal(int) { s_stop_requested = 1; }

void Usage() {
  std::cerr << "usage: " MODERNGEKKO_RUNNER_NAME
               " [--game <extracted-root>] [--module <path>]\n"
               "       [--user-dir <path>] [--title <text>] [--load-state <path>]\n"
               "       [--graphics <backend>] [--audio <backend>]\n"
               "       [--mods <directory>] [--no-mods]\n"
               "       [--wayland] [-X11] [--headless] [--allow-interpreter]\n"
               "       [--netplay-host | --netplay-join <host>] "
               "[--netplay-port <port>]\n"
               "       [--nickname <name>] [--buffer <auto|1-20>] "
               "[--controller <device>]...\n"
               "       F1 opens the settings menu (resolution, input, "
               "audio); ESC or F1 closes it.\n"
               "       With no --game, boots the path in "
               "<user-dir>/default-game.txt.\n";
}

std::filesystem::path
ReadDefaultGame(const std::filesystem::path &user_directory,
                const std::filesystem::path &executable_directory) {
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const std::filesystem::path default_file =
      executable_directory / "default-game.txt";
#else
  const std::filesystem::path default_file =
      user_directory / "default-game.txt";
#endif
  std::ifstream file(default_file);
  std::string path;
  std::getline(file, path);
  if (!path.empty() && path.back() == '\r')
    path.pop_back();
  // A missing file (or an empty first line) yields an empty string, and
  // std::filesystem::path("") is a RELATIVE path -- resolving it would return
  // the executable directory and the "no game configured" hand-over would never
  // trigger. First run then died on "invalid game: missing sys/main.dol"
  // instead of asking for a disc. Empty means empty.
  if (path.empty())
    return {};
  std::filesystem::path result(path);
  if (result.is_relative())
    result = executable_directory / result;
  return result;
}

std::filesystem::path DefaultUserDirectory() {
#ifdef MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS
#if defined(_WIN32)
  if (const char *user_profile = std::getenv("USERPROFILE"))
    return std::filesystem::path(user_profile) / "Documents" /
           MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char *home = std::getenv("HOME"))
    return std::filesystem::path(home) / "Documents" /
           MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
#if defined(_WIN32)
  if (const char *local_app_data = std::getenv("LOCALAPPDATA"))
    return std::filesystem::path(local_app_data) /
           MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char *xdg = std::getenv("XDG_DATA_HOME"))
    return std::filesystem::path(xdg) / MODERNGEKKO_USER_DIRECTORY_NAME;
  if (const char *home = std::getenv("HOME"))
    return std::filesystem::path(home) / ".local" / "share" /
           MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
}

// The packaged build ships the launcher beside the runner. With no game
// configured there is nothing to run, so start the launcher and let it ask for
// a disc image instead of printing a usage dump at somebody who double-clicked
// an icon.
bool HandOverToLauncher(const std::filesystem::path &executable_directory) {
  std::filesystem::path launcher =
      executable_directory / MODERNGEKKO_LAUNCHER_FILENAME;
#if defined(_WIN32)
  launcher += ".exe";
#endif
  std::error_code ec;
  if (!std::filesystem::is_regular_file(launcher, ec))
    return false;
  std::fprintf(stderr,
               "[run] no game configured; starting the launcher (%s)\n",
               launcher.string().c_str());
#if defined(_WIN32)
  return std::system(("\"" + launcher.string() + "\"").c_str()) == 0;
#else
  const pid_t pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    // exec, not a nested child: the launcher replaces this process image, so
    // whatever started the runner (shell, desktop entry) does not end up with
    // two live processes.
    execl(launcher.c_str(), launcher.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  return true;
#endif
}

std::string LibrarySuffix() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

std::filesystem::path ExecutableDirectory(const char *argv0) {
  std::error_code ec;
#if defined(__linux__)
  const std::filesystem::path proc_executable =
      std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return proc_executable.parent_path();
  ec.clear();
#endif
  const std::filesystem::path executable =
      std::filesystem::weakly_canonical(argv0, ec);
  return ec ? std::filesystem::current_path() : executable.parent_path();
}
} // namespace

int RunMain(int argc, char **argv) {
#if defined(_WIN32)
  if (!std::getenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD"))
    _putenv_s("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1");
#else
  setenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1", 0);
#endif
  moderngekko::RuntimeConfig config;
  config.user_directory = DefaultUserDirectory();
  const std::filesystem::path executable_directory =
      ExecutableDirectory(argv[0]);
#ifdef MODERNGEKKO_DEFAULT_WINDOW_TITLE
  config.window_title = MODERNGEKKO_DEFAULT_WINDOW_TITLE;
#endif
  std::filesystem::path module_path;
  bool use_default_mods = true;
  std::optional<moderngekko::frontend::NetplayRole> netplay_role;
  std::string netplay_address;
  std::optional<std::uint16_t> netplay_port;
  std::string netplay_nickname;
  std::string netplay_buffer;
  std::vector<std::string> netplay_controllers;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value = [&](const char *option) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << option << " requires a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--game")
      config.game_root = value("--game");
    else if (arg == "--module")
      module_path = value("--module");
    else if (arg == "--user-dir")
      config.user_directory = value("--user-dir");
    else if (arg == "--title")
      config.window_title = value("--title");
    else if (arg == "--load-state")
    {
      // Checked here rather than left to the boot path: a state that is not
      // there would otherwise boot to the title screen and look like it worked.
      std::filesystem::path state = StringToPath(value("--load-state"));
      if (!std::filesystem::is_regular_file(state))
      {
        std::cerr << "savestate not found: " << PathToString(state) << '\n';
        return 2;
      }
      config.load_state_path = std::move(state);
    }
    else if (arg == "--graphics")
      config.graphics.backend = value("--graphics");
    else if (arg == "--audio")
      config.audio.backend = value("--audio");
    else if (arg == "--mods")
      config.mod_directories.emplace_back(value("--mods"));
    else if (arg == "--no-mods")
      use_default_mods = false;
    else if (arg == "-X11" || arg == "--x11")
      config.window_system = moderngekko::WindowSystem::X11;
    else if (arg == "--wayland")
      config.window_system = moderngekko::WindowSystem::Wayland;
    else if (arg == "--mute")
      config.audio.mute = true;
    else if (arg == "--automation-dir")
      config.automation.directory = value("--automation-dir");
    else if (arg == "--headless")
      config.headless = true;
    else if (arg == "--allow-interpreter")
      config.allow_interpreter = true;
    else if (arg == "--netplay-host")
      netplay_role = moderngekko::frontend::NetplayRole::Host;
    else if (arg == "--netplay-join") {
      netplay_role = moderngekko::frontend::NetplayRole::Join;
      netplay_address = value("--netplay-join");
    } else if (arg == "--netplay-port") {
      const std::string port_value = value("--netplay-port");
      unsigned int port = 0;
      const auto parsed = std::from_chars(
          port_value.data(), port_value.data() + port_value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != port_value.data() + port_value.size() || port == 0 ||
          port > 65535) {
        std::cerr << "--netplay-port must be between 1 and 65535\n";
        return 2;
      }
      netplay_port = static_cast<std::uint16_t>(port);
    } else if (arg == "--nickname")
      netplay_nickname = value("--nickname");
    else if (arg == "--buffer")
      netplay_buffer = value("--buffer");
    else if (arg == "--controller")
      netplay_controllers.emplace_back(value("--controller"));
    else if (arg == "--help" || arg == "-h") {
      Usage();
      return 0;
    } else {
      std::cerr << "unknown option: " << arg << '\n';
      Usage();
      return 2;
    }
  }
  if (config.game_root.empty())
    config.game_root =
        ReadDefaultGame(config.user_directory, executable_directory);
  if (config.game_root.empty()) {
    // No game configured: this is a first run (or a wiped user directory), not
    // an error. Hand over to the launcher, which asks for a disc image and
    // remembers it -- so double-clicking the emulator binary is a working
    // entry point for somebody who has never seen this build before.
    if (HandOverToLauncher(executable_directory))
      return 0;
    std::cerr << "no game configured; use --game once or create "
              << (config.user_directory / "default-game.txt") << '\n';
    Usage();
    return 2;
  }

  auto frontend_config =
      moderngekko::frontend::LoadConfig(config.user_directory, true);
  if (!frontend_config) {
    std::cerr << "invalid config.ini: " << frontend_config.error << '\n';
    return 2;
  }
  // This build's GL context factory has no Wayland path (GLContext::Create
  // handles X11/Headless/FBDev only), so OGL on a Wayland surface always
  // dies at boot with "Failed to initialize video backend!". Vulkan is the
  // default backend anyway — normalize instead of boot-failing.
  if (config.window_system == moderngekko::WindowSystem::Wayland &&
      frontend_config.graphics_backend == "OGL") {
    std::cerr << "[run] OpenGL cannot initialize on Wayland in this build; "
                 "using Vulkan instead\n";
    frontend_config.graphics_backend = "Vulkan";
  }
  config.graphics.internal_resolution_scale = frontend_config.dolphin_scale;
  if (config.graphics.backend.empty())
    config.graphics.backend = frontend_config.graphics_backend;
  // Same Wayland/OGL guard for an explicit --graphics OGL on the command line.
  if (config.window_system == moderngekko::WindowSystem::Wayland &&
      config.graphics.backend == "OGL") {
    std::cerr << "[run] OpenGL cannot initialize on Wayland in this build; "
                 "using Vulkan instead\n";
    config.graphics.backend = "Vulkan";
  }
  if (config.audio.backend.empty())
    config.audio.backend = frontend_config.audio_backend;
  config.fullscreen = frontend_config.fullscreen;
  config.show_fps_in_title = frontend_config.show_fps_in_title;
  if (use_default_mods) {
    config.mod_directories.push_back(executable_directory / "Mods");
    config.mod_directories.push_back(config.user_directory / "Mods");
  }

  if (!netplay_role && !frontend_config.controller.empty()) {
    std::string controller_message;
    if (!moderngekko::frontend::EnsureControllerConfig(
            config.user_directory, frontend_config.controller,
            &controller_message)) {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::cout << "controller configuration: " << controller_message << '\n';
  }

#ifdef MODERNGEKKO_DOL_PATCH_MANIFEST
  bool dol_changed = false;
  std::string dol_patch_error;
  if (!moderngekko::frontend::ApplyDolPatchManifest(
          config.game_root / "sys" / "main.dol",
          executable_directory / MODERNGEKKO_DOL_PATCH_MANIFEST, &dol_changed,
          &dol_patch_error)) {
    std::cerr << "DOL patching failed: " << dol_patch_error << '\n';
    return 2;
  }
  if (dol_changed)
    std::cout << "Applied native DOL patches\n";
#endif
  const auto inspected = moderngekko::InspectGame(config.game_root);
  if (!inspected) {
    std::cerr << "invalid game: " << inspected.error << '\n';
    return 2;
  }

#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (inspected.metadata->disc_id != MODERNGEKKO_REQUIRED_DISC_ID) {
    std::cerr << "unsupported disc ID: expected "
              << MODERNGEKKO_REQUIRED_DISC_ID << ", got "
              << inspected.metadata->disc_id << '\n';
    return 2;
  }
#endif
#ifdef MODERNGEKKO_REQUIRED_DOL_SHA256
  if (inspected.metadata->dol_sha256 != MODERNGEKKO_REQUIRED_DOL_SHA256) {
    std::cerr << "unsupported main DOL: this release requires its pinned game build\n";
    return 2;
  }
#endif
#ifdef MODERNGEKKO_REQUIRED_REL_SHA256
  if (inspected.metadata->rel_sha256 != MODERNGEKKO_REQUIRED_REL_SHA256) {
    std::cerr << "unsupported _Main.rel: this release requires its pinned game build\n";
    return 2;
  }
#endif
#ifdef MODERNGEKKO_REQUIRED_ASSETS_SHA256
  if (inspected.metadata->assets_sha256 !=
      MODERNGEKKO_REQUIRED_ASSETS_SHA256) {
    std::cerr << "unsupported game assets: this release requires its pinned game build\n";
    return 2;
  }
#endif

  // Compatibility discovery belongs to the runner, never the runtime library.
  if (module_path.empty()) {
    if (const char *env = std::getenv("STATICRECOMP_MODULE"))
      module_path = env;
    else {
      const std::string module_name =
          "g" + inspected.metadata->disc_id + "_recomp" + LibrarySuffix();
      const auto bundled = executable_directory / module_name;
      const auto user_module =
          config.user_directory / "StaticRecompModules" / module_name;
      if (std::filesystem::is_regular_file(bundled))
        module_path = bundled;
      else if (std::filesystem::is_regular_file(user_module))
        module_path = user_module;
    }
  }
  if (!module_path.empty())
    config.module =
        moderngekko::ModuleSource::DynamicPath(std::move(module_path));

#if defined(__linux__) || defined(_WIN32)
  if (!config.headless && config.graphics.backend.empty())
    config.graphics.backend = "Vulkan";
#endif

  if (netplay_role) {
    moderngekko::frontend::NetplayOptions options;
    options.role = *netplay_role;
    options.address = netplay_address.empty() ? frontend_config.netplay_address
                                              : netplay_address;
    options.port = netplay_port.value_or(frontend_config.netplay_port);
    options.nickname = netplay_nickname.empty()
                           ? frontend_config.netplay_nickname
                           : netplay_nickname;
    options.buffer = netplay_buffer.empty() ? frontend_config.netplay_buffer
                                            : netplay_buffer;
    const std::vector<std::string> configured_controllers =
        moderngekko::frontend::ReadConfiguredControllers(config.user_directory);
    options.controllers =
        netplay_controllers.empty()
            ? (configured_controllers.empty() ? frontend_config.controllers
                                              : configured_controllers)
            : netplay_controllers;
    if (options.controllers.empty() && !frontend_config.controller.empty())
      options.controllers.push_back(frontend_config.controller);
    if (options.controllers.empty()) {
      std::cerr << "netplay requires at least one selected controller\n";
      return 2;
    }
    frontend_config.netplay_address = options.address;
    frontend_config.netplay_port = options.port;
    frontend_config.netplay_nickname = options.nickname;
    frontend_config.netplay_buffer = options.buffer;
    frontend_config.controllers = options.controllers;
    frontend_config.controller = options.controllers.front();
    std::string controller_message;
    if (!moderngekko::frontend::EnsureControllerConfig(
            config.user_directory, options.controllers, &controller_message)) {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::string save_error;
    if (!moderngekko::frontend::SaveConfig(config.user_directory,
                                           frontend_config, &save_error)) {
      std::cerr << "configuration: " << save_error << '\n';
      return 2;
    }
    std::cerr << "netplay: runner started\n";
    return moderngekko::frontend::RunNetplayLobby(
        std::move(config), std::move(frontend_config), std::move(options));
  }

  const std::filesystem::path user_directory = config.user_directory;
  auto created = moderngekko::Runtime::Create(std::move(config));
  if (!created) {
    std::cerr << "initialization failed: " << created.error->message << '\n';
    return 1;
  }
  std::cout << "audio backend: " << created.runtime->GetConfig().audio.backend
            << '\n';

  // Frontend-owned settings that the runtime does not map itself. Layers
  // exist now that the runtime initialized the config system, and the video
  // backend only starts at Run(), so this lands before anything reads it.
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_VOLUME,
                           frontend_config.mute ? 0 : frontend_config.volume);
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_MUTED, frontend_config.mute);
  Config::SetBaseOrCurrent(Config::GFX_VSYNC, frontend_config.vsync);
  // Display/texture settings from the F1 menu. These have to be in place
  // before Run() because VideoConfig::LoadConfig() snapshots them into
  // g_ActiveConfig at backend start; applying them later would mean the first
  // frame renders with the wrong aspect ratio and without the texture pack.
  // Guarded: a -1 from either table means "not one of ours" (a hand-edited
  // config.ini), and writing that through would put a garbage language byte in
  // the emulated SRAM or an out-of-range aspect enum in the core.
  const int language_value =
      moderngekko::frontend::GameCubeLanguageValue(frontend_config.language);
  if (language_value >= 0)
    Config::SetBaseOrCurrent(Config::MAIN_GC_LANGUAGE, language_value);
  const int aspect_value =
      moderngekko::frontend::AspectRatioMode(frontend_config.aspect_ratio);
  Config::SetBaseOrCurrent(
      Config::GFX_ASPECT_RATIO,
      static_cast<AspectMode>(aspect_value < 0 ? 0 : aspect_value));
  Config::SetBaseOrCurrent(Config::GFX_WIDESCREEN_HACK,
                           frontend_config.widescreen_hack);
  Config::SetBaseOrCurrent(Config::GFX_HIRES_TEXTURES,
                           frontend_config.hires_textures);
  Config::SetBaseOrCurrent(Config::GFX_CACHE_HIRES_TEXTURES,
                           frontend_config.cache_hires_textures);
  // Cheats: PatchEngine::LoadPatches() runs during core boot and reads this
  // through Config::AreCheatsEnabled(), so it has to be set before Run().
  Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS,
                           frontend_config.enable_cheats);
  std::cerr << "[run] config: language=" << frontend_config.language
            << " aspect=" << frontend_config.aspect_ratio
            << " widescreen=" << (frontend_config.widescreen_hack ? 1 : 0)
            << " hires=" << (frontend_config.hires_textures ? 1 : 0)
            << " cache_hires=" << (frontend_config.cache_hires_textures ? 1 : 0)
            << '\n';

  moderngekko::frontend::ConfigMenu config_menu;
  {
    moderngekko::frontend::ConfigMenu::Hooks hooks;
    hooks.runtime = created.runtime.get();
    hooks.user_directory = user_directory;
    hooks.config = &frontend_config;
    const moderngekko::GameMetadata &metadata =
        created.runtime->GetGameMetadata();
    hooks.game_name = metadata.game_name;
    hooks.disc_id = metadata.disc_id;
    hooks.module_path = created.runtime->GetConfig().module.path.string();
    // The System tab's region combo needs both: the running disc's extraction
    // to find its siblings, and the executable's directory to find a module
    // shipped next to the binary (the installed/AppImage layout).
    // Read from the runtime, NOT from config: Runtime::Create(std::move(config))
    // at the top of RunMain already moved config out, so config.game_root is
    // empty here -- which made every region's game root unresolvable and the
    // combo report "not installed" for all three.
    hooks.game_root = created.runtime->GetConfig().game_root;
    hooks.executable_directory = executable_directory;
    // OGL has no Wayland GL context path in this build; the menu must not
    // offer it when the window is a Wayland surface.
    hooks.allow_ogl =
        config.window_system != moderngekko::WindowSystem::Wayland;
    config_menu.Start(std::move(hooks));
  }

  std::signal(SIGINT, HandleStopSignal);
  std::signal(SIGTERM, HandleStopSignal);
  std::jthread signal_watcher([&](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      if (s_stop_requested) {
        s_stop_requested = 0;
        created.runtime->RequestStop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  const moderngekko::RuntimeRunResult result = created.runtime->Run();
  config_menu.Stop();
  signal_watcher.request_stop();
  std::cerr << "[run] Run() returned; WantsRestart="
            << (config_menu.WantsRestart() ? 1 : 0) << '\n';
  if (result.error) {
    std::cerr << "runtime failed: " << result.error->message << '\n';
    return 1;
  }
  if (config_menu.WantsRestart()) {
#if defined(__linux__)
    // A region switch rewrites the disc and module before the replay: a plain
    // restart replays argv unchanged and would boot the region we are leaving.
    // Savestates belong to one region, so --load-state is dropped rather than
    // pointed at a state that was written for a different game.
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc) + 2);
    for (int i = 0; i < argc; ++i)
      args.emplace_back(argv[i]);
    if (config_menu.WantsRegionSwitch()) {
      const std::string game = config_menu.RegionSwitchGameRoot();
      const std::string module = config_menu.RegionSwitchModule();
      std::vector<std::string> rebuilt;
      rebuilt.reserve(args.size() + 2);
      bool have_game = false;
      bool have_module = false;
      for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "--game" && i + 1 < args.size()) {
          rebuilt.push_back("--game");
          rebuilt.push_back(game);
          have_game = true;
          ++i;
        } else if (arg == "--module" && i + 1 < args.size()) {
          rebuilt.push_back("--module");
          rebuilt.push_back(module);
          have_module = true;
          ++i;
        } else if (arg == "--load-state" && i + 1 < args.size()) {
          ++i;
        } else {
          rebuilt.push_back(arg);
        }
      }
      // Appended when the original launch relied on auto-resolution, so the
      // new region's module does not fall back to the one already on disk.
      if (!have_game) {
        rebuilt.push_back("--game");
        rebuilt.push_back(game);
      }
      if (!have_module) {
        rebuilt.push_back("--module");
        rebuilt.push_back(module);
      }
      args.erase(args.begin() + 1, args.end());
      args.insert(args.end(), rebuilt.begin(), rebuilt.end());
      std::cerr << "[run] region switch: --game " << game << " --module "
                << module << '\n';
    }
    // Pointers only now: c_str() into a vector that still grows would dangle.
    std::vector<char *> replay;
    replay.reserve(args.size() + 1);
    for (auto &arg : args)
      replay.push_back(arg.data());
    replay.push_back(nullptr);
    std::cout.flush();
    std::cerr.flush();
    execv("/proc/self/exe", replay.data());
    std::cerr << "restart failed: " << std::strerror(errno) << '\n';
    return 1;
#else
    std::cerr << "restart is not supported on this platform\n";
    return 0;
#endif
  }
  return 0;
}

#if defined(_WIN32)
// Optionally confine the process to the cores that share the largest L3.
//
// The emulated CPU is a single serial instruction stream, so nothing here is
// about parallelism -- core usage stays at 1.00 either way. It is about cache
// residency. A recompiled module is one dispatch switch spanning the whole
// game, which lives or dies on staying in L3, and on a chip with 3D V-Cache on
// one CCD only, Windows migrating the thread between dies makes it repeatedly
// lose its working set. Measured on a 9950X3D: 55.41 -> 70.42 fps, +27%.
//
// This applies to every Dolphin thread, not only the emulated CPU thread, so it
// is opt-in. Set MODERNGEKKO_CACHE_AFFINITY=1 to enable it after benchmarking
// the target machine.
void PinProcessToLargestCache() {
  if (!moderngekko::frontend::AffinityEnabled(
          std::getenv("MODERNGEKKO_CACHE_AFFINITY")))
    return;

  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationCache, nullptr, &length);
  if (length == 0)
    return;
  std::vector<char> buffer(length);
  if (!GetLogicalProcessorInformationEx(
          RelationCache,
          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length))
    return;

  const moderngekko::frontend::CacheDomain domain =
      moderngekko::frontend::LargestSharedCache(buffer.data(), length);
  if (!domain)
    return;

  if (moderngekko::frontend::ApplyCacheDomain(GetCurrentProcess(), domain).affinity_set) {
    std::cout << "[perf] pinned to the cores sharing the largest L3 (" << (domain.size >> 20)
              << " MB), mask 0x" << std::hex << static_cast<unsigned long long>(domain.mask)
              << std::dec << '\n';
  }
}
#endif

int main(int argc, char **argv) {
  try {
#if defined(_WIN32)
    PinProcessToLargestCache();
#endif
    return RunMain(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "fatal error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "fatal error: unknown exception\n";
  }
  return 1;
}
