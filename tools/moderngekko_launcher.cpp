#include "frontend_config.hpp"
#include "dol_patch.hpp"
#include "launcher_savestates.hpp"
#include "moderngekko/game.hpp"
#include "netplay_session.hpp"

#include "DiscIO/DiscExtractor.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <memory>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace
{
#ifndef MODERNGEKKO_FRONTEND_NAME
#define MODERNGEKKO_FRONTEND_NAME "ModernGekko"
#endif

#ifndef MODERNGEKKO_RUNNER_FILENAME
#define MODERNGEKKO_RUNNER_FILENAME "moderngekko-run"
#endif

#ifndef MODERNGEKKO_FRONTEND_VERSION
#define MODERNGEKKO_FRONTEND_VERSION "1.0"
#endif

// Reverse-DNS application id used for Wayland's app_id: it is what the desktop
// uses to group the window under one icon, so it must not be the frontend name.
#ifndef MODERNGEKKO_APP_ID
#define MODERNGEKKO_APP_ID "io.github.beachspikers"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

#ifndef MODERNGEKKO_LOG_FILENAME
#define MODERNGEKKO_LOG_FILENAME "ModernGekko.log"
#endif

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
constexpr std::string_view CONTROLLER_PROFILE_NAME = "GCPadNew.ini";
constexpr std::string_view GENERATED_PROFILE_LABEL = "GameCube profile";
#else
constexpr std::string_view CONTROLLER_PROFILE_NAME = "WiimoteNew.ini";
constexpr std::string_view GENERATED_PROFILE_LABEL = "sideways profile";
#endif

struct ExtractionState
{
  std::atomic<bool> running{false};
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> total{1};
  std::mutex mutex;
  std::string status;
  std::string error;
  std::optional<fs::path> finished_game;
};

struct DialogState
{
  std::mutex mutex;
  std::optional<fs::path> selected;
  std::string error;
};

struct ControllerOption
{
  std::string label;
  std::string device;
};

std::string RequiredGameError(const moderngekko::GameMetadata& metadata)
{
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (metadata.disc_id != MODERNGEKKO_REQUIRED_DISC_ID)
    return "expected disc ID " MODERNGEKKO_REQUIRED_DISC_ID ", found " + metadata.disc_id;
#endif
#ifdef MODERNGEKKO_REQUIRED_DOL_SHA256
  if (metadata.dol_sha256 != MODERNGEKKO_REQUIRED_DOL_SHA256)
    return "main DOL does not match the pinned release";
#endif
#ifdef MODERNGEKKO_REQUIRED_REL_SHA256
  if (metadata.rel_sha256 != MODERNGEKKO_REQUIRED_REL_SHA256)
    return "_Main.rel does not match the pinned release";
#endif
#ifdef MODERNGEKKO_REQUIRED_ASSETS_SHA256
  if (metadata.assets_sha256 != MODERNGEKKO_REQUIRED_ASSETS_SHA256)
    return "game assets do not match the pinned release";
#endif
  return {};
}

std::vector<ControllerOption> EnumerateControllers()
{
  std::vector<ControllerOption> result;
  std::unordered_map<std::string, int> device_ids;
  int count = 0;
  // Test hook: pretend no gamepad is connected, so the keyboard-only path can
  // be exercised on a machine that has one plugged in.
  const bool ignore_gamepads = SDL_getenv("MODERNGEKKO_LAUNCHER_IGNORE_GAMEPADS") != nullptr;
  SDL_JoystickID* joystick_ids = ignore_gamepads ? nullptr : SDL_GetJoysticks(&count);
  if (ignore_gamepads)
    count = 0;
  for (int i = 0; i < count; ++i)
  {
    const SDL_JoystickID joystick_id = joystick_ids[i];
    const bool gamepad = SDL_IsGamepad(joystick_id);
    const char* name_value =
        gamepad ? SDL_GetGamepadNameForID(joystick_id) : SDL_GetJoystickNameForID(joystick_id);
    const std::string name = name_value && *name_value ? name_value : "Unknown Controller";
    const int device_id = device_ids[name]++;
    if (!gamepad)
      continue;
    ControllerOption option;
    option.label = device_id == 0 ? name : name + " (" + std::to_string(device_id + 1) + ")";
    option.device = "SDL/" + std::to_string(device_id) + "/" + name;
    result.emplace_back(std::move(option));
  }
  if (joystick_ids)
    SDL_free(joystick_ids);
  // Keyboard is always offered last, so a real gamepad still wins the default
  // selection when one is plugged in. The bare "Keyboard" spec is resolved by
  // the runner, which owns the controller interface: enumerating keyboards here
  // means initialising Dolphin's input stack in the launcher, and that turned
  // into an unstable (sometimes crashing) startup.
  {
    ControllerOption keyboard;
    keyboard.label = "Keyboard (keyboard as pad)";
    keyboard.device = "Keyboard";
    result.emplace_back(std::move(keyboard));
  }
  return result;
}

int FindController(const std::vector<ControllerOption>& controllers, std::string_view device)
{
  const auto found = std::ranges::find(controllers, device, &ControllerOption::device);
  return found == controllers.end() ? -1 : static_cast<int>(found - controllers.begin());
}

fs::path DefaultUserDirectory()
{
#ifdef MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS
#if defined(_WIN32)
  if (const char* user_profile = std::getenv("USERPROFILE"))
    return fs::path(user_profile) / "Documents" / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / "Documents" / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
#if defined(_WIN32)
  if (const char* local_app_data = std::getenv("LOCALAPPDATA"))
    return fs::path(local_app_data) / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char* xdg = std::getenv("XDG_DATA_HOME"))
    return fs::path(xdg) / MODERNGEKKO_USER_DIRECTORY_NAME;
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / ".local/share" / MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
}

fs::path DocumentsDirectory()
{
#if defined(_WIN32)
  if (const char* user_profile = std::getenv("USERPROFILE"))
    return fs::path(user_profile) / "Documents";
#endif
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / "Documents";
  return fs::current_path();
}

fs::path ExecutableDirectory(const char* argv0)
{
  std::error_code ec;
#if defined(__linux__)
  const fs::path proc_executable = fs::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return proc_executable.parent_path();
  ec.clear();
#endif
  const fs::path executable = fs::weakly_canonical(argv0, ec);
  return ec ? fs::current_path() : executable.parent_path();
}

fs::path SiblingExecutable(const fs::path& release_directory, std::string name)
{
#if defined(_WIN32)
  name += ".exe";
#endif
  const fs::path sibling = release_directory / name;
  return fs::is_regular_file(sibling) ? sibling : fs::path(name);
}

bool ApplyBundledDolPatch(const fs::path& game_root, const fs::path& release_directory,
                          bool* changed, std::string* error)
{
#ifdef MODERNGEKKO_DOL_PATCH_MANIFEST
  if (game_root.empty() || !fs::is_directory(game_root))
  {
    *changed = false;
    return true;
  }
  return moderngekko::frontend::ApplyDolPatchManifest(
      game_root / "sys" / "main.dol", release_directory / MODERNGEKKO_DOL_PATCH_MANIFEST,
      changed, error);
#else
  *changed = false;
  return true;
#endif
}

// The launcher's UI cannot be driven from a test on this box (no Xvfb, and a
// real window would land on someone's desktop), so the first-run flow is
// verified from its log lines instead: MODERNGEKKO_LAUNCHER_DEBUG=1 makes the
// state transitions of the setup card visible from stderr.
bool LauncherDebugEnabled()
{
  static const bool enabled = [] {
    const char* value = std::getenv("MODERNGEKKO_LAUNCHER_DEBUG");
    return value && *value && *value != '0';
  }();
  return enabled;
}

void LauncherLog(const char* format, ...)
{
  if (!LauncherDebugEnabled())
    return;
  std::va_list args;
  va_start(args, format);
  std::fputs("[launcher] ", stderr);
  std::vfprintf(stderr, format, args);
  std::fputc('\n', stderr);
  va_end(args);
}

fs::path DefaultGameFile(const fs::path& user_directory, const fs::path& release_directory)
{
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  return release_directory / "default-game.txt";
#else
  return user_directory / "default-game.txt";
#endif
}

fs::path ReadDefaultGame(const fs::path& user_directory, const fs::path& release_directory)
{
  std::ifstream file(DefaultGameFile(user_directory, release_directory));
  std::string value;
  std::getline(file, value);
  if (!value.empty() && value.back() == '\r')
    value.pop_back();
  // Empty means "nothing written yet": path("") is relative and resolving it
  // returns the release directory, which then gets inspected as if it were a
  // game. The runner had the same bug and died on "missing sys/main.dol".
  if (value.empty())
    return {};
  fs::path game(value);
  if (game.is_relative())
    game = release_directory / game;
  return game;
}

// Where ExtractDisc puts a game, and therefore where a previous run's extraction
// can still be found. default-game.txt is the record of that, but it is a small
// file in a user directory: it survives until someone deletes it, moves the
// build, or runs the runner from a shell with --game (which never writes it).
// Losing it used to make the launcher ask for the ISO again even though the
// extracted game was sitting right there, so look for it before believing there
// is no game.
// Identity-only inspection: reads sys/boot.bin and nothing else.
//
// InspectGame() also hashes the whole files/ archive (hundreds of megabytes)
// to fill assets_sha256. The launcher needs a name and a disc id for the window
// and for picking a module, so paying that cost -- at startup and again for
// every listed extraction -- is pure waste. The pinned-build checks still use
// the full inspection when the release actually pins hashes.
std::optional<moderngekko::GameMetadata> InspectGameIdentity(const fs::path& game_root)
{
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(game_root, ec);
  if (ec || !fs::is_directory(root, ec))
    return std::nullopt;
  const fs::path boot_path = root / "sys" / "boot.bin";
  const fs::path dol_path = root / "sys" / "main.dol";
  if (!fs::is_regular_file(boot_path, ec) || !fs::is_regular_file(dol_path, ec))
    return std::nullopt;

  std::ifstream boot(boot_path, std::ios::binary);
  if (!boot)
    return std::nullopt;
  std::array<char, 0x60> header{};
  boot.read(header.data(), header.size());
  if (boot.gcount() != static_cast<std::streamsize>(header.size()))
    return std::nullopt;

  std::string disc_id(header.data(), 6);
  if (disc_id.size() != 6 ||
      !std::all_of(disc_id.begin(), disc_id.end(),
                   [](unsigned char c) { return std::isalnum(c) != 0; }))
    return std::nullopt;

  std::string name(header.data() + 0x20, 0x40);
  if (const auto terminator = name.find('\0'); terminator != std::string::npos)
    name.resize(terminator);
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
    name.pop_back();
  if (name.empty())
    name = disc_id;

  const auto read_be32 = [&](std::size_t offset)
  {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(header[offset])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(header[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(header[offset + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(header[offset + 3]));
  };
  const std::uint32_t wii_magic = read_be32(0x18);
  const std::uint32_t gc_magic = read_be32(0x1c);

  moderngekko::GameMetadata metadata;
  metadata.root = root;
  metadata.main_dol = dol_path;
  metadata.game_name = std::move(name);
  metadata.disc_id = std::move(disc_id);
  metadata.platform = wii_magic == 0x5d1c9ea3 ? moderngekko::GamePlatform::Wii
                                              : moderngekko::GamePlatform::GameCube;
  return metadata;
}

// One extracted game version found under <user>/games.
struct ExtractedGame
{
  fs::path root;
  std::string disc_id;
  std::string name;
};

std::vector<ExtractedGame> ListExtractedGames(const fs::path& user_directory)
{
  std::vector<ExtractedGame> games;
  std::error_code ec;
  const fs::path games_root = user_directory / "games";
  for (fs::directory_iterator it(games_root, ec), end; !ec && it != end; it.increment(ec))
  {
    if (!it->is_directory(ec))
      continue;
    const auto identity = InspectGameIdentity(it->path());
    if (!identity)
      continue;
    games.push_back({identity->root, identity->disc_id, identity->game_name});
  }
  std::ranges::sort(games, {}, &ExtractedGame::disc_id);
  return games;
}

// Human-readable region from the disc ID's fourth character (Nintendo's
// convention: <product><region><disc><version>). The launcher deals in disc
// IDs everywhere, but "GBSP8P" means nothing to a player who owns one disc and
// has to pick which version the module should be built for.
std::string RegionLabel(const std::string& disc_id)
{
  if (disc_id.size() < 4)
    return "unknown region";
  switch (disc_id[3])
  {
  case 'E':
    return "NTSC-U";
  case 'J':
    return "NTSC-J";
  case 'P':
    return "PAL";
  case 'D':
    return "PAL (Germany)";
  case 'F':
    return "PAL (France)";
  case 'I':
    return "PAL (Italy)";
  case 'S':
    return "PAL (Spain)";
  case 'K':
    return "NTSC-K";
  case 'R':
    return "PAL (Russia)";
  default:
    return "region " + std::string(1, disc_id[3]);
  }
}

std::string GameVersionLabel(const ExtractedGame& game)
{
  return game.name + "  [" + game.disc_id + " - " + RegionLabel(game.disc_id) + "]";
}

fs::path DiscoverExtractedGame(const fs::path& user_directory,
                               const fs::path& release_directory)
{
  std::error_code ec;
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const fs::path games_directory = release_directory / "Game";
  if (fs::is_directory(games_directory, ec) && moderngekko::InspectGame(games_directory))
    return games_directory;
#else
  const fs::path games_directory = user_directory / "games";
  if (fs::is_directory(games_directory, ec))
  {
    std::vector<std::pair<fs::file_time_type, fs::path>> candidates;
    for (const auto& entry : fs::directory_iterator(games_directory, ec))
    {
      if (!entry.is_directory(ec))
        continue;
      std::error_code time_error;
      const fs::file_time_type written = fs::last_write_time(entry.path(), time_error);
      candidates.emplace_back(time_error ? fs::file_time_type{} : written, entry.path());
    }
    // Newest first: with several extractions, the most recent one is what the
    // player last used. Inspection rejects half-extracted or wrong-disc trees.
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [written, path] : candidates)
    {
      if (moderngekko::InspectGame(path))
      {
        LauncherLog("found an extracted game without default-game.txt: %s",
                    path.string().c_str());
        return path;
      }
    }
  }
#endif
  return {};
}

bool WriteDefaultGame(const fs::path& user_directory, const fs::path& release_directory,
                      const fs::path& game, std::string* error)
{
  const fs::path destination = DefaultGameFile(user_directory, release_directory);
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  std::ofstream file(destination, std::ios::trunc);
  if (!file)
  {
    if (error)
      *error = "can't save default-game.txt";
    return false;
  }
  fs::path stored = game;
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const fs::path relative = fs::relative(game, release_directory, ec);
  if (!ec && !relative.empty())
    stored = relative;
#endif
  file << stored.string() << '\n';
  return true;
}

std::vector<fs::path> FindDiscImages()
{
  std::vector<fs::path> images;
  std::error_code ec;
  const fs::path documents = DocumentsDirectory();
  if (!fs::is_directory(documents, ec))
    return images;
  fs::recursive_directory_iterator iterator(documents,
                                            fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (iterator != end)
  {
    if (iterator.depth() > 4)
      iterator.disable_recursion_pending();
    if (iterator->is_regular_file(ec))
    {
      std::string extension = iterator->path().extension().string();
      std::ranges::transform(extension, extension.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (extension == ".wbfs" || extension == ".iso" || extension == ".rvz")
        images.push_back(iterator->path());
    }
    iterator.increment(ec);
    if (ec)
      ec.clear();
  }
  std::ranges::sort(images);
  return images;
}

std::optional<fs::path> PrepareDisc(const fs::path& image, const fs::path& user_directory,
                                    const fs::path& release_directory, ExtractionState* state,
                                    std::string* error)
{
  std::unique_ptr<DiscIO::Volume> source = DiscIO::CreateVolume(image.string());
  if (!source)
  {
    *error = "Dolphin rejected the selected disc image";
    return std::nullopt;
  }
  const std::string source_id = source->GetGameID(source->GetGamePartition());
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (source_id == MODERNGEKKO_REQUIRED_DISC_ID)
    return image;
#endif
#if defined(MODERNGEKKO_DISC_PREPARER_FILENAME) && \
    defined(MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID) && defined(MODERNGEKKO_REQUIRED_DISC_ID)
  if (source_id != MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID)
  {
    *error = "expected clean " MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID " or patched "
             MODERNGEKKO_REQUIRED_DISC_ID "; selected " + source_id;
    return std::nullopt;
  }
  const fs::path output = user_directory / "Setup" / "SonicRidersTE-2.4.6.1.iso";
  std::error_code ec;
  fs::create_directories(output.parent_path(), ec);
  if (ec)
  {
    *error = "can't create setup directory: " + ec.message();
    return std::nullopt;
  }
  {
    std::lock_guard lock(state->mutex);
    state->status = "Applying Tournament Edition 2.4.6.1";
  }
  const fs::path preparer =
      SiblingExecutable(release_directory, MODERNGEKKO_DISC_PREPARER_FILENAME);
  const std::array<std::string, 5> storage = {
      preparer.string(), "--input", image.string(), "--output", output.string()};
  std::array<const char*, 6> arguments{};
  for (std::size_t i = 0; i < storage.size(); ++i)
    arguments[i] = storage[i].c_str();
  SDL_Process* process = SDL_CreateProcess(arguments.data(), false);
  if (!process)
  {
    *error = "can't start Tournament Edition patcher: " + std::string(SDL_GetError());
    return std::nullopt;
  }
  int exit_code = 1;
  const bool waited = SDL_WaitProcess(process, true, &exit_code);
  SDL_DestroyProcess(process);
  if (!waited || exit_code != 0)
  {
    *error = "Tournament Edition patching failed";
    return std::nullopt;
  }
  return output;
#else
  // No patched-release pinning for this title: accept the disc as-is.
  return image;
#endif
}

bool ExtractDisc(const fs::path& image, const fs::path& user_directory,
                 const fs::path& release_directory, ExtractionState* state)
{
  auto fail = [&](std::string message)
  {
    std::lock_guard lock(state->mutex);
    state->error = std::move(message);
    state->running = false;
    return false;
  };

  {
    std::lock_guard lock(state->mutex);
    state->status = "Opening " + image.filename().string();
  }
  std::string preparation_error;
  const std::optional<fs::path> prepared =
      PrepareDisc(image, user_directory, release_directory, state, &preparation_error);
  if (!prepared)
    return fail(std::move(preparation_error));
  std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(prepared->string());
  if (!volume)
    return fail("Dolphin rejected the prepared disc image");

  const DiscIO::Partition partition = volume->GetGamePartition();
  const DiscIO::FileSystem* filesystem = volume->GetFileSystem(partition);
  if (!filesystem || !filesystem->IsValid())
    return fail("Dolphin could not read the game partition filesystem");

  std::string disc_id = volume->GetGameID(partition);
  if (disc_id.size() != 6)
    return fail("the selected image has an invalid disc ID");
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (disc_id != MODERNGEKKO_REQUIRED_DISC_ID)
    return fail("this frontend requires disc ID " MODERNGEKKO_REQUIRED_DISC_ID "; selected " +
                disc_id);
#endif
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const fs::path games_directory = release_directory;
  const fs::path output = games_directory / "Game";
#else
  const fs::path games_directory = user_directory / "games";
  const fs::path output = games_directory / disc_id;
#endif
  std::error_code ec;
  if (fs::is_directory(output, ec))
  {
    bool changed = false;
    std::string patch_error;
    if (!ApplyBundledDolPatch(output, release_directory, &changed, &patch_error))
      return fail("existing extraction patching failed: " + patch_error);
  }
  const auto existing = moderngekko::InspectGame(output);
  if (existing && RequiredGameError(*existing.metadata).empty())
  {
    std::string error;
    if (!WriteDefaultGame(user_directory, release_directory, output, &error))
      return fail(error);
    std::lock_guard lock(state->mutex);
    state->finished_game = output;
    state->status = "Using existing extraction";
    state->running = false;
    return true;
  }

  const fs::path staging = games_directory / (disc_id + ".extracting");
  fs::remove_all(staging, ec);
  fs::create_directories(staging / "files", ec);
  if (ec)
    return fail("can't create extraction directory: " + ec.message());

  {
    std::lock_guard lock(state->mutex);
    state->status = "Extracting system data";
  }
  if (!DiscIO::ExportSystemData(*volume, partition, staging.string()))
  {
    fs::remove_all(staging, ec);
    return fail("Dolphin failed while extracting the disc system data");
  }

  state->total = std::max(1u, filesystem->GetRoot().GetTotalChildren());
  {
    std::lock_guard lock(state->mutex);
    state->status = "Extracting game files";
  }
  DiscIO::ExportDirectory(*volume, partition, filesystem->GetRoot(), true, "",
                          (staging / "files").string(),
                          [state](const std::string& path)
                          {
                            ++state->completed;
                            std::lock_guard lock(state->mutex);
                            state->status = "Extracting " + path;
                            return false;
                          });

  bool dol_changed = false;
  std::string dol_patch_error;
  if (!ApplyBundledDolPatch(staging, release_directory, &dol_changed, &dol_patch_error))
  {
    fs::remove_all(staging, ec);
    return fail("extracted DOL patching failed: " + dol_patch_error);
  }

  const auto inspected = moderngekko::InspectGame(staging);
  if (!inspected)
  {
    fs::remove_all(staging, ec);
    return fail("extracted game validation failed: " + inspected.error);
  }
  if (const std::string identity_error = RequiredGameError(*inspected.metadata);
      !identity_error.empty())
  {
    fs::remove_all(staging, ec);
    return fail("extracted game validation failed: " + identity_error);
  }

  fs::remove_all(output, ec);
  ec.clear();
  fs::rename(staging, output, ec);
  if (ec)
    return fail("can't publish extracted game: " + ec.message());
  std::string error;
  if (!WriteDefaultGame(user_directory, release_directory, output, &error))
    return fail(error);

  {
    std::lock_guard lock(state->mutex);
    state->finished_game = output;
    state->status = "Extraction complete";
  }
  state->running = false;
  return true;
}

void SDLCALL FileDialogCallback(void* userdata, const char* const* filelist, int)
{
  auto* state = static_cast<DialogState*>(userdata);
  std::lock_guard lock(state->mutex);
  if (!filelist)
    state->error = SDL_GetError();
  else if (filelist[0])
    state->selected = filelist[0];
}

// One place for the disc-image filter list and the starting folder, shared by
// the first-run card and the settings page. SDL3's dialog is asynchronous, so
// the result lands in DialogState::selected and is picked up by the main loop.
void ShowDiscImageDialog(SDL_Window* window, DialogState* dialog)
{
  static constexpr SDL_DialogFileFilter filters[] = {
      {"Disc images", "iso;wbfs;rvz"},
      {"ISO", "iso"},
      {"WBFS", "wbfs"},
      {"RVZ", "rvz"}};
  const std::string documents = DocumentsDirectory().string();
  SDL_ShowOpenFileDialog(FileDialogCallback, dialog, window, filters,
                         static_cast<int>(std::size(filters)), documents.c_str(), false);
}

fs::path SiblingTool(const char* argv0, const char* name)
{
  std::error_code ec;
  const fs::path self = fs::weakly_canonical(argv0, ec);
  fs::path tool = name;
#if defined(_WIN32)
  tool += ".exe";
#endif
  const fs::path sibling = self.parent_path() / tool;
  return fs::is_regular_file(sibling, ec) ? sibling : fs::path{};
}

// Compile the game module from the extracted disc with the bundled port tool.
//
// This is what makes a toolchain-only package usable: the module is generated
// from the player's own dump, so nothing derived from the game has to be
// redistributed. Returns false and fills *error when the tool is missing or the
// build reports failure; the full compiler output goes to log_path.
// A package may ship its own compiler/cmake/ninja for players who have no
// development tools installed. Put those directories first on PATH so the port
// tool (which shells out to `cmake` and the compiler by name) finds them, and
// the system toolchain still wins only if nothing was bundled.
bool UseBundledToolchain(const fs::path& install_dir)
{
  std::vector<fs::path> dirs;
  for (const char* relative :
       {"Toolchain/bin", "Toolchain/mingw/bin", "Toolchain/cmake/bin", "Toolchain/python"})
  {
    std::error_code ec;
    const fs::path dir = install_dir / relative;
    if (fs::is_directory(dir, ec))
      dirs.push_back(dir);
  }
  if (dirs.empty())
    return false;

#if defined(_WIN32)
  constexpr char separator = ';';
  const char* const variable = "PATH";
#else
  constexpr char separator = ':';
  const char* const variable = "PATH";
#endif
  std::string prefix;
  for (const auto& dir : dirs)
  {
    prefix += dir.string();
    prefix += separator;
  }
  const char* const existing = std::getenv(variable);
  const std::string combined = prefix + (existing ? existing : "");
#if defined(_WIN32)
  // _putenv_s only updates the CRT's copy of the environment; child processes
  // inherit the Win32 process block, which needs SetEnvironmentVariable. With
  // only _putenv_s the launched moderngekko-port (and the cmake it spawns) did
  // not see Toolchain/bin, so CMake failed with "unable to find a build program
  // corresponding to Ninja".
  SetEnvironmentVariableA(variable, combined.c_str());
  _putenv_s(variable, combined.c_str());
#else
  setenv(variable, combined.c_str(), 1);
#endif
  std::fprintf(stderr, "[launcher] using bundled toolchain: %s\n", prefix.c_str());
  return true;
}

// A compiler the module build can actually use, and the version line that
// proves it. A package that ships its own clang used to be trusted blindly: the
// build started, failed twenty seconds later in CMake, and the player had no way
// to tell a broken toolchain from a broken disc. Probing first turns that into a
// named problem with a fix ("pick another compiler") before the build runs.
struct CompilerProbe
{
  fs::path path;
  std::string version;
  std::string error;
  bool ok() const { return !version.empty(); }
};

CompilerProbe ProbeCompiler(const fs::path& compiler)
{
  CompilerProbe probe;
  probe.path = compiler;
  if (compiler.empty())
  {
    probe.error = "no compiler chosen";
    return probe;
  }
  std::error_code ec;
  const fs::path output = fs::temp_directory_path(ec) / "moderngekko-compiler-probe.txt";
  if (ec)
  {
    probe.error = "no temporary directory to probe the compiler in";
    return probe;
  }
  SDL_IOStream* log = SDL_IOFromFile(output.string().c_str(), "w");
  if (!log)
  {
    probe.error = "cannot write " + output.string();
    return probe;
  }
  // SDL_CreateProcess instead of a shell: a path with spaces ("C:\Program
  // Files\...") or a missing file then shows up as an error string instead of
  // an empty read that looks like "the compiler is not installed".
  std::vector<std::string> storage = {compiler.string(), "--version"};
  std::vector<const char*> arguments;
  arguments.reserve(storage.size() + 1);
  for (const std::string& argument : storage)
    arguments.push_back(argument.c_str());
  arguments.push_back(nullptr);
  SDL_PropertiesID properties = SDL_CreateProperties();
  SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, arguments.data());
  SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
  SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log);
  SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
  SDL_Process* process = SDL_CreateProcessWithProperties(properties);
  SDL_DestroyProperties(properties);
  SDL_CloseIO(log);
  if (!process)
  {
    probe.error = "cannot run " + compiler.string() + ": " + SDL_GetError();
    return probe;
  }
  int exit_code = 1;
  SDL_WaitProcess(process, true, &exit_code);
  SDL_DestroyProcess(process);

  std::ifstream version_file(output);
  std::string line;
  while (std::getline(version_file, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line.empty())
      continue;
    probe.version = line;
    break;
  }
  fs::remove(output, ec);
  if (probe.version.empty())
    probe.error = compiler.string() + " did not report a version (exit " +
                  std::to_string(exit_code) + ")";
  return probe;
}

// The compiler a package ships, if any. llvm-mingw is the Windows bundle; the
// other paths cover a Linux/macOS bundle built the same way.
fs::path BundledCompiler(const fs::path& install_dir)
{
  std::error_code ec;
  for (const char* relative : {"Toolchain/mingw/bin/clang", "Toolchain/bin/clang"})
  {
    fs::path candidate = install_dir / relative;
#if defined(_WIN32)
    candidate += ".exe";
#endif
    if (fs::is_regular_file(candidate, ec))
      return candidate;
  }
  return {};
}

bool BuildModuleFromDisc(const fs::path& port_tool, const fs::path& game_root,
                         const fs::path& work_dir, const fs::path& final_path,
                         const fs::path& log_path, bool bundled_toolchain,
                         const std::string& compiler, std::string* error)
{
  std::error_code ec;
  fs::create_directories(work_dir, ec);
  fs::create_directories(final_path.parent_path(), ec);
  fs::create_directories(log_path.parent_path(), ec);
  SDL_IOStream* log = SDL_IOFromFile(log_path.string().c_str(), "w");
  if (!log)
  {
    *error = "cannot open " + log_path.string() + ": " + SDL_GetError();
    return false;
  }
  // --output is the artifact *directory*: the tool puts the library under
  // <dir>/<disc>/<dol-hash>/, so it is moved into place afterwards.
  // Optimisation matters here: this build produces the binary the player
  // actually runs. -O2 is the level the tuned builds were measured at, with
  // ThinLTO on Linux/macOS and the extra FP-safe passes moderngekko-port adds;
  // -O3 only ever looked faster because the flags were not what they claimed
  // to be.
  std::vector<std::string> storage = {port_tool.string(), "build", game_root.string(),
                                      "--output", work_dir.string(), "--opt-level", "2"};
#if defined(_WIN32)
  // ThinLTO's link is the one step that hung on Windows; a plain lld link
  // finishes there. Linux/macOS keep LTO because it links reliably and buys
  // real frame time.
  storage.emplace_back("--no-lto");
#endif
  // The exact compiler that was probed in the launcher. Passing the path makes
  // the build independent of PATH order: on Windows the bundle used to be found
  // only because its directory was prepended to PATH, and a system clang in a
  // different bitness or a bare `cl` would win as soon as that prefix was wrong.
  if (!compiler.empty())
  {
    storage.emplace_back("--compiler");
    storage.emplace_back(compiler);
  }
  else if (bundled_toolchain)
  {
    storage.emplace_back("--toolchain");
    storage.emplace_back("clang");
  }
  // SDL wants a NULL-terminated argv. A fixed array sized to the *longest*
  // command line is one entry short the moment the bundled-toolchain flags are
  // appended, and the extra slot then reads whatever follows the array on the
  // stack -- which surfaced on Windows as "unknown or incomplete option: s".
  std::vector<const char*> arguments;
  arguments.reserve(storage.size() + 1);
  for (const std::string& argument : storage)
    arguments.push_back(argument.c_str());
  arguments.push_back(nullptr);
  const SDL_PropertiesID properties = SDL_CreateProperties();
  SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, arguments.data());
  SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
  SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log);
  SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
  SDL_Process* process = SDL_CreateProcessWithProperties(properties);
  SDL_DestroyProperties(properties);
  SDL_CloseIO(log);
  if (!process)
  {
    *error = std::string("cannot start ") + port_tool.string() + ": " + SDL_GetError();
    return false;
  }
  int exit_code = 1;
  const bool waited = SDL_WaitProcess(process, true, &exit_code);
  SDL_DestroyProcess(process);
  if (!waited || exit_code != 0)
  {
    *error = "the module build failed; see " + log_path.string();
    return false;
  }

  // Pick the freshest library with the expected name under the work directory.
  const std::string wanted = final_path.filename().string();
  fs::path newest;
  fs::file_time_type newest_time{};
  for (fs::recursive_directory_iterator it(work_dir, ec), end; it != end && !ec; it.increment(ec))
  {
    if (!it->is_regular_file(ec) || it->path().filename() != wanted)
      continue;
    const auto stamp = it->last_write_time(ec);
    if (newest.empty() || stamp > newest_time)
    {
      newest = it->path();
      newest_time = stamp;
    }
  }
  if (newest.empty())
  {
    *error = "the build finished but produced no " + wanted + "; see " + log_path.string();
    return false;
  }

  fs::remove(final_path, ec);
  fs::copy_file(newest, final_path, fs::copy_options::overwrite_existing, ec);
  if (ec || !fs::is_regular_file(final_path, ec))
  {
    *error = "cannot copy the built module to " + final_path.string() + ": " + ec.message();
    return false;
  }
  return true;
}

fs::path SiblingRunner(const char* argv0)
{
  std::error_code ec;
  const fs::path self = fs::weakly_canonical(argv0, ec);
  fs::path runner = MODERNGEKKO_RUNNER_FILENAME;
#if defined(_WIN32)
  runner += ".exe";
#endif
  const fs::path sibling = self.parent_path() / runner;
  return fs::is_regular_file(sibling) ? sibling : runner;
}
} // namespace

int main(int argc, char** argv)
{
  bool use_wayland = false;
  std::optional<fs::path> extract_only;
  for (int i = 1; i < argc; ++i)
  {
    if (std::string_view(argv[i]) == "-X11" || std::string_view(argv[i]) == "--x11")
      use_wayland = false;
    else if (std::string_view(argv[i]) == "--wayland")
      use_wayland = true;
    else if (std::string_view(argv[i]) == "--extract" && i + 1 < argc)
      extract_only = argv[++i];
  }

  const fs::path user_directory = DefaultUserDirectory();
  const fs::path release_directory = ExecutableDirectory(argv[0]);
  if (extract_only)
  {
    ExtractionState extraction;
    extraction.running = true;
    const bool success =
        ExtractDisc(*extract_only, user_directory, release_directory, &extraction);
    std::lock_guard lock(extraction.mutex);
    if (!success)
      std::cerr << "extraction failed: " << extraction.error << '\n';
    else
      std::cout << extraction.status << ": " << *extraction.finished_game << '\n';
    return success ? 0 : 1;
  }

  auto config = moderngekko::frontend::LoadConfig(user_directory, true);
  if (!config)
  {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "Invalid " MODERNGEKKO_FRONTEND_NAME " config.ini",
                             config.error.c_str(), nullptr);
    return 2;
  }

#if defined(__linux__)
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, use_wayland ? "wayland" : "x11");
#endif
  // Wayland (and X11's WM_CLASS) take the window's application identity from
  // SDL's metadata, which otherwise defaults to the executable's name. Setting
  // it here is what keeps the taskbar/dock entry reading the game's name
  // instead of the frontend's.
  SDL_SetAppMetadata(MODERNGEKKO_FRONTEND_NAME, MODERNGEKKO_FRONTEND_VERSION,
                     MODERNGEKKO_APP_ID);
  SDL_setenv_unsafe("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1", 0);
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    return 1;

  const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  // Wide enough for the themed padding and for the game-version list; the
  // window is resizable so a long path or a small screen still fits.
  SDL_Window* window = SDL_CreateWindow(MODERNGEKKO_FRONTEND_NAME, static_cast<int>(980 * scale),
                                        static_cast<int>(650 * scale),
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window)
  {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_Renderer* renderer = SDL_CreateRenderer(window, "vulkan");
  if (!renderer)
    renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer)
  {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().ScaleAllSizes(scale);
  ImGui::GetStyle().FontScaleDpi = scale;
  // Same palette as the in-game F1 overlay, so the launcher and the settings
  // menu do not look like two different programs.
  {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f * scale;
    style.FrameRounding = 6.0f * scale;
    style.GrabRounding = 6.0f * scale;
    style.TabRounding = 6.0f * scale;
    style.ScrollbarRounding = 8.0f * scale;
    style.WindowPadding = ImVec2(18.0f * scale, 16.0f * scale);
    style.FramePadding = ImVec2(10.0f * scale, 6.0f * scale);
    style.ItemSpacing = ImVec2(10.0f * scale, 8.0f * scale);
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.085f, 0.115f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.24f, 0.28f, 0.36f, 0.65f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.145f, 0.17f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.25f, 0.32f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.31f, 0.39f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.17f, 0.21f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.31f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.39f, 0.50f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.19f, 0.25f, 0.33f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.33f, 0.43f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.29f, 0.39f, 0.51f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.80f, 0.67f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.80f, 0.67f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.90f, 0.78f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.28f, 0.33f, 0.42f, 0.70f);
    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.58f, 0.62f, 0.70f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.12f, 0.16f, 0.98f);
  }
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  std::vector<fs::path> images = FindDiscImages();
  std::optional<fs::path> selected_image = images.empty() ? std::nullopt : std::optional(images[0]);
  // A disc image opened from a file manager ("Open With ...") arrives as an
  // argument, and it should preselect itself: the player picked that exact
  // file, so making them find it again in the dialog would be absurd. It only
  // preselects -- Start still has to be pressed, because launching a game on
  // top of whatever is already running is not something to do unasked.
  for (int i = 1; i < argc; ++i)
  {
    if (argv[i][0] == '-')
      continue;
    std::error_code file_error;
    const fs::path& argument = argv[i];
    if (!fs::is_regular_file(argument, file_error))
      continue;
    fs::path canonical = fs::weakly_canonical(argument, file_error);
    selected_image = file_error ? fs::path(argument) : std::move(canonical);
    LauncherLog("preselected disc image from the command line: %s",
                selected_image->string().c_str());
    break;
  }
  fs::path current_game = ReadDefaultGame(user_directory, release_directory);
  if (current_game.empty())
    current_game = DiscoverExtractedGame(user_directory, release_directory);
  bool current_dol_changed = false;
  std::string current_dol_error;
  auto inspect_current = [&](const fs::path& root) -> moderngekko::GameInspectResult
  {
#if defined(MODERNGEKKO_REQUIRED_DOL_SHA256) || defined(MODERNGEKKO_REQUIRED_REL_SHA256) || \
    defined(MODERNGEKKO_REQUIRED_ASSETS_SHA256)
    return moderngekko::InspectGame(root);
#else
    if (auto identity = InspectGameIdentity(root))
      return {std::move(*identity), {}};
    return moderngekko::InspectGame(root);
#endif
  };
  auto current_metadata =
      ApplyBundledDolPatch(current_game, release_directory, &current_dol_changed,
                           &current_dol_error) ?
          inspect_current(current_game) :
          moderngekko::GameInspectResult{{}, current_dol_error};
  if (current_metadata && !RequiredGameError(*current_metadata.metadata).empty())
    current_metadata = {{}, RequiredGameError(*current_metadata.metadata)};

  // Every previously extracted version, so a player can switch region without
  // hunting for the folder. The active game is folded in as well: it can live
  // outside <user>/games (the dev layout, a portable build, or a path the
  // player wrote into default-game.txt), and a version list that omits the
  // running version would be worse than no list at all.
  std::vector<ExtractedGame> extracted_games = ListExtractedGames(user_directory);
  const auto refresh_extracted_games = [&]
  {
    extracted_games = ListExtractedGames(user_directory);
    if (!current_metadata)
      return;
    const std::string& id = current_metadata.metadata->disc_id;
    const bool listed = std::ranges::any_of(
        extracted_games, [&](const ExtractedGame& game) { return game.disc_id == id; });
    if (!listed)
    {
      extracted_games.push_back({current_game, id,
                                 current_metadata.metadata->game_name});
      std::ranges::sort(extracted_games, {}, &ExtractedGame::disc_id);
    }
  };
  refresh_extracted_games();
  // Switching version only swaps which game the Play button targets; it must
  // never start emulation on its own. Same for extracting a second region.
  const auto select_version = [&](const ExtractedGame& game)
  {
    current_game = game.root;
    current_metadata = inspect_current(game.root);
    if (current_metadata && !RequiredGameError(*current_metadata.metadata).empty())
      current_metadata = {{}, RequiredGameError(*current_metadata.metadata)};
    std::string ignored;
    WriteDefaultGame(user_directory, release_directory, current_game, &ignored);
    LauncherLog("selected game version: %s (%s)", game.disc_id.c_str(),
                game.root.string().c_str());
  };
  // Set by the "Extract another version" path: the extraction finishes into the
  // list, not into a launch.
  bool extract_without_launch = false;
  bool show_extract_panel = false;

  const auto& resolutions = moderngekko::frontend::SupportedResolutions();
  const auto& graphics_backends =
      moderngekko::frontend::SupportedGraphicsBackends();
  bool show_fps_in_title = config.show_fps_in_title;
  bool fullscreen = config.fullscreen;
  std::array<char, 31> netplay_nickname{};
  std::array<char, 256> netplay_address{};
  std::snprintf(netplay_nickname.data(), netplay_nickname.size(), "%s",
                config.netplay_nickname.c_str());
  std::snprintf(netplay_address.data(), netplay_address.size(), "%s",
                config.netplay_address.c_str());
  int netplay_port = config.netplay_port;
  bool automatic_buffer = config.netplay_buffer == "auto";
  int manual_buffer = automatic_buffer ? 5 : std::stoi(config.netplay_buffer);
  int resolution_index = 0;
  for (std::size_t i = 0; i < resolutions.size(); ++i)
  {
    if (config.resolution == resolutions[i].text)
      resolution_index = static_cast<int>(i);
  }
  int graphics_backend_index = 0;
  for (std::size_t i = 0; i < graphics_backends.size(); ++i)
  {
    if (config.graphics_backend == graphics_backends[i].value)
      graphics_backend_index = static_cast<int>(i);
  }

  // StateSaves is Dolphin's own state directory, so states written in-game land
  // here without any extra configuration and show up on the next launch.
  //
  // Listed once at startup rather than every frame, so the dropdown does not
  // hit the filesystem on each redraw. A state written while the launcher is
  // open therefore appears on the next launch, which is the case that matters:
  // states are written by the running game, not by the launcher.
  const std::vector<fs::path> launcher_savestates =
      moderngekko::frontend::ListLauncherSavestates(user_directory / "StateSaves");
  // Loading is always explicit. StateSaves is shared by games, so selecting the
  // newest file automatically could try to resume a different title.
  int selected_savestate = -1;

  DialogState dialog;
  std::vector<ControllerOption> controllers = EnumerateControllers();
  std::fprintf(stderr, "[launcher] controllers=%zu\n", controllers.size());
  bool controller_profile_exists = moderngekko::frontend::ControllerConfigExists(user_directory);
  std::vector<std::string> configured_controllers =
      moderngekko::frontend::ReadConfiguredControllers(user_directory);
  std::string selected_controller =
      configured_controllers.empty() ? config.controller : configured_controllers.front();
  int controller_index = FindController(controllers, selected_controller);
  std::string controller_status;
  const auto select_controller = [&](int index)
  {
    std::string message;
    // Keyboard devices are resolved by the runner, which is the process that
    // owns Dolphin's controller interface. The launcher only records the
    // choice; moderngekko-run regenerates GCPadNew.ini from it at startup.
    if (!controllers[index].device.starts_with("Keyboard") &&
        !moderngekko::frontend::GenerateControllerConfig(user_directory,
                                                         controllers[index].device, &message))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(message);
      return false;
    }
    if (controllers[index].device.starts_with("Keyboard"))
      message = "Keyboard bindings are written when the game starts";
    std::string error;
    if (!moderngekko::frontend::SaveConfig(user_directory, resolutions[resolution_index].text,
                                           show_fps_in_title, controllers[index].device, &error))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(error);
      return false;
    }
    selected_controller = controllers[index].device;
    configured_controllers = {selected_controller};
    config.controller = selected_controller;
    config.controllers = configured_controllers;
    config.resolution = resolutions[resolution_index].text;
    config.show_fps_in_title = show_fps_in_title;
    controller_profile_exists = true;
    controller_status = std::move(message);
    return true;
  };
  const auto ensure_controller = [&]
  {
    if (moderngekko::frontend::ControllerConfigExists(user_directory))
    {
      controller_profile_exists = true;
      configured_controllers = moderngekko::frontend::ReadConfiguredControllers(user_directory);
      if (configured_controllers.empty())
      {
        std::lock_guard lock(dialog.mutex);
        dialog.error = std::string(CONTROLLER_PROFILE_NAME) +
                       " has no configured controller device";
        return false;
      }
      selected_controller = configured_controllers.front();
      controller_index = FindController(controllers, selected_controller);
      controller_status = "Using existing " + std::string(CONTROLLER_PROFILE_NAME);
      return true;
    }
    if (controller_index < 0)
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = "Connect a controller or pick the keyboard option before playing";
      return false;
    }
    return select_controller(controller_index);
  };
  const auto refresh_controllers = [&]
  {
    controllers = EnumerateControllers();
    controller_index = FindController(controllers, selected_controller);
    if (!controller_profile_exists)
    {
      if (controller_index < 0 && !controllers.empty())
      {
        controller_index = 0;
        selected_controller = controllers.front().device;
      }
      if (controller_index >= 0)
        select_controller(controller_index);
      else
        controller_status = "No SDL gamepad detected";
    }
    else
    {
      controller_status = "Using existing " + std::string(CONTROLLER_PROFILE_NAME);
    }
  };
  refresh_controllers();
  ExtractionState extraction;
  std::jthread extraction_thread;
  // Kicks off disc extraction. Shared by the first-run card and the settings
  // page so the two cannot drift apart.
  const auto begin_extraction = [&](const fs::path& image)
  {
    extraction.completed = 0;
    extraction.total = 1;
    extraction.running = true;
    {
      std::lock_guard lock(extraction.mutex);
      extraction.error.clear();
      extraction.status.clear();
      extraction.finished_game.reset();
    }
    extraction_thread = std::jthread([image, user_directory, release_directory, &extraction]
                                     {
                                       ExtractDisc(image, user_directory, release_directory,
                                                   &extraction);
                                     });
  };
  enum class LaunchMode
  {
    None,
    Solo,
    Host,
    Join,
  };
  bool done = false;
  // The module gate lives in the launcher window instead of an SDL dialog: a
  // failed SDL_ShowMessageBox used to leave the process exiting silently right
  // after the "build it now" prompt, which looked like the app just closing.
  enum class ModuleGate
  {
    None,
    Ask,
    Building,
    Failed,
  };
  ModuleGate gate = ModuleGate::None;
  bool auto_launch_done = false;
  bool auto_build_done = false;
  LaunchMode pending_launch = LaunchMode::None;
  std::atomic<bool> build_done{false};
  std::atomic<bool> build_ok{false};
  std::string build_error;
  std::chrono::steady_clock::time_point build_start{};
  std::jthread build_thread;
  bool first_run_card = false;
  // Counts the frames the setup card has been drawn for, so its item rects are
  // logged once (a per-frame log would be thousands of identical lines).
  int card_frames = 0;
  LaunchMode launch_mode = LaunchMode::None;
  // Debug aid: MODERNGEKKO_LAUNCHER_SHOT=<prefix> writes the first rendered
  // frames to <prefix>-<n>.bmp. The setup card only ever appears in a GUI
  // session, so without this there is no way to check its wording or layout
  // short of asking someone to look: the offscreen video driver still renders
  // for real, and SDL hands the pixels straight back.
  const char* shot_prefix = SDL_getenv("MODERNGEKKO_LAUNCHER_SHOT");
  int shot_index = 0;
  const auto dump_frame = [&]
  {
    if (!shot_prefix || shot_index >= 3)
      return;
    SDL_Surface* frame = SDL_RenderReadPixels(renderer, nullptr);
    if (!frame)
      return;
    const std::string path =
        std::string(shot_prefix) + "-" + std::to_string(shot_index) + ".bmp";
    if (SDL_SaveBMP(frame, path.c_str()))
      ++shot_index;
    else
      SDL_Log("SDL_SaveBMP(%s) failed: %s", path.c_str(), SDL_GetError());
    SDL_DestroySurface(frame);
  };
  // Sibling tool + where a built module goes. `share/dolphin` beside the tool
  // tree is populated by packaging; without it the port falls back to the path
  // it was built in, which only exists on the machine that built the release.
  const fs::path exe_dir = SiblingRunner(argv[0]).parent_path();
  const fs::path port_tool = SiblingTool(argv[0], "moderngekko-port");
  const fs::path modules_dir = user_directory / "StaticRecompModules";

  // Which compiler the on-demand module build will use, and what it says about
  // itself. Resolved once at startup: the shipped clang first (so a package is
  // self-contained), then whatever config.ini remembers, then the system
  // compiler. Empty means the gate reports the problem instead of starting a
  // build that fails at unit 1.
  std::string compiler_path;
  std::string compiler_version;
  {
    std::vector<fs::path> candidates;
    if (!config.compiler.empty())
      candidates.emplace_back(config.compiler);
    if (const fs::path bundled = BundledCompiler(exe_dir); !bundled.empty())
      candidates.push_back(bundled);
#if defined(_WIN32)
    candidates.emplace_back("cl");
#endif
    candidates.emplace_back("clang");
    candidates.emplace_back("gcc");
    for (const fs::path& candidate : candidates)
    {
      const CompilerProbe probe = ProbeCompiler(candidate);
      if (probe.ok())
      {
        compiler_path = candidate.string();
        compiler_version = probe.version;
        break;
      }
      LauncherLog("compiler rejected: %s", probe.error.c_str());
    }
    LauncherLog("module build compiler: %s (%s)",
                compiler_path.empty() ? "none" : compiler_path.c_str(),
                compiler_version.empty() ? "-" : compiler_version.c_str());
  }

  auto module_suffix = []()
  {
#if defined(_WIN32)
    return std::string(".dll");
#elif defined(__APPLE__)
    return std::string(".dylib");
#else
    return std::string(".so");
#endif
  };

  const auto module_name_for_disc = [&]() -> std::string
  {
    if (!current_metadata)
      return {};
    return "g" + current_metadata.metadata->disc_id + "_recomp" + module_suffix();
  };

  const auto module_present = [&]()
  {
    const std::string name = module_name_for_disc();
    if (name.empty())
      return false;
    std::error_code ec;
    return fs::is_regular_file(exe_dir / name, ec) ||
           fs::is_regular_file(modules_dir / name, ec);
  };

  const auto request_launch = [&](LaunchMode mode)
  {
    if (!ensure_controller())
      return;
    if (module_present())
    {
      launch_mode = mode;
      done = true;
      return;
    }
    pending_launch = mode;
    gate = ModuleGate::Ask;
  };

  while (!done)
  {
    bool controllers_changed = false;
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      if (SDL_getenv("MODERNGEKKO_LAUNCHER_MOUSE_DEBUG") &&
          (event.type == SDL_EVENT_MOUSE_MOTION ||
           event.type == SDL_EVENT_MOUSE_BUTTON_DOWN))
        std::fprintf(stderr, "[sdl-mouse] type=%u x=%.0f y=%.0f which=%u\n",
                     static_cast<unsigned>(event.type), event.motion.x, event.motion.y,
                     static_cast<unsigned>(event.motion.which));
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        done = true;
      if (event.type == SDL_EVENT_JOYSTICK_ADDED || event.type == SDL_EVENT_JOYSTICK_REMOVED ||
          event.type == SDL_EVENT_GAMEPAD_REMAPPED)
      {
        controllers_changed = true;
      }
    }
    if (controllers_changed)
      refresh_controllers();

    {
      std::lock_guard lock(dialog.mutex);
      if (dialog.selected)
      {
        selected_image = std::move(dialog.selected);
        dialog.selected.reset();
      }
    }
    {
      std::lock_guard lock(extraction.mutex);
      if (extraction.finished_game)
      {
        current_game = *extraction.finished_game;
        current_metadata = inspect_current(current_game);
        if (current_metadata && !RequiredGameError(*current_metadata.metadata).empty())
          current_metadata = {{}, RequiredGameError(*current_metadata.metadata)};
        extraction.finished_game.reset();
        std::string ignored;
        WriteDefaultGame(user_directory, release_directory, current_game, &ignored);
        refresh_extracted_games();
        if (extract_without_launch)
        {
          // Keep the window (and the old version's list entry) usable; the
          // player presses Play when they are ready.
          extract_without_launch = false;
          show_extract_panel = false;
          LauncherLog("extracted an additional version without launching: %s",
                      current_game.string().c_str());
        }
        else
        {
          request_launch(LaunchMode::Solo);
        }
      }
    }

    // Test hook: start the normal launch path without a mouse, so the module
    // gate can be exercised from a script. MODERNGEKKO_LAUNCHER_AUTOBUILD also
    // presses "Build it now".
    if (!auto_launch_done && !current_game.empty() &&
        SDL_getenv("MODERNGEKKO_LAUNCHER_AUTOPLAY") != nullptr)
    {
      auto_launch_done = true;
      request_launch(LaunchMode::Solo);
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin(MODERNGEKKO_FRONTEND_NAME " Launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(MODERNGEKKO_FRONTEND_NAME);
    ImGui::Separator();

    // Two columns: everything that decides *what* runs on the left, everything
    // that decides *how* it looks (plus the disc image) on the right. Stacked in
    // one column the settings ran past the bottom of the window, which hid the
    // last rows behind a scrollbar nobody noticed.
    const float column_gap = 22.0f * scale;
    const float column_width =
        (ImGui::GetContentRegionAvail().x - column_gap) * 0.5f;
    ImGui::BeginChild("##launcher_left", ImVec2(column_width, 0),
                      ImGuiChildFlags_None);

    // --- Game version ------------------------------------------------------
    // A build can hold several extracted versions (PAL / NTSC-U / NTSC-J).
    // Each one needs its own recompiled module, so which version is active is
    // not cosmetic -- it decides what Play builds and runs.
    {
      ImGui::TextUnformatted("Game version");
      ImGui::SameLine();
      if (ImGui::SmallButton(show_extract_panel ? "Hide extractor" : "Extract another version..."))
        show_extract_panel = !show_extract_panel;
      ImGui::SameLine();
      ImGui::TextDisabled("%zu extracted", extracted_games.size());

      if (extracted_games.empty())
      {
        ImGui::TextDisabled(
            "No extracted version yet - use the disc image section below.");
      }
      else
      {
        const std::string preview =
            current_metadata ?
                GameVersionLabel({current_game, current_metadata.metadata->disc_id,
                                  current_metadata.metadata->game_name}) :
                "Choose an extracted version";
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##game_version", preview.c_str()))
        {
          for (const ExtractedGame& game : extracted_games)
          {
            const bool selected =
                current_metadata && game.disc_id == current_metadata.metadata->disc_id;
            if (ImGui::Selectable(GameVersionLabel(game).c_str(), selected) &&
                !selected)
            {
              select_version(game);
            }
            if (selected)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        // The extraction path is long enough to run past the window edge, so
        // it lives in a tooltip instead of on the row.
        if (current_metadata && ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", current_game.string().c_str());
      }

      if (show_extract_panel)
      {
        ImGui::Separator();
        ImGui::TextWrapped(
            "Extract another region's disc. It is added to the list above and "
            "stays inactive until you press Play.");
        if (selected_image)
          ImGui::TextWrapped("%s", selected_image->string().c_str());
        else
          ImGui::TextDisabled("No ISO, WBFS, or RVZ selected");
        ImGui::BeginDisabled(extraction.running);
        if (ImGui::Button("Browse for ISO / WBFS / RVZ"))
          ShowDiscImageDialog(window, &dialog);
        if (selected_image)
        {
          ImGui::SameLine();
          if (ImGui::Button("Extract version"))
          {
            extract_without_launch = true;
            begin_extraction(*selected_image);
          }
        }
        ImGui::EndDisabled();
        if (extraction.running)
        {
          const float progress = std::min(
              1.0f, static_cast<float>(extraction.completed.load()) /
                        extraction.total.load());
          ImGui::ProgressBar(progress, ImVec2(-1, 0));
        }
      }
      ImGui::Separator();
    }

    if (current_metadata)
    {
      ImGui::Text("Ready: %s [%s - %s]", current_metadata.metadata->game_name.c_str(),
                  current_metadata.metadata->disc_id.c_str(),
                  RegionLabel(current_metadata.metadata->disc_id).c_str());
      if (!launcher_savestates.empty())
      {
        ImGui::TextUnformatted("Start from savestate");
        const std::string savestate_preview =
            selected_savestate >= 0 ?
                moderngekko::frontend::LauncherSavestateLabel(
                    launcher_savestates[static_cast<std::size_t>(selected_savestate)],
                    selected_savestate == 0) :
                "Normal boot";
        ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##launch_savestate", savestate_preview.c_str()))
        {
          const bool normal_boot_selected = selected_savestate < 0;
          if (ImGui::Selectable("Normal boot", normal_boot_selected))
            selected_savestate = -1;
          if (normal_boot_selected)
            ImGui::SetItemDefaultFocus();

          for (std::size_t i = 0; i < launcher_savestates.size(); ++i)
          {
            const std::string label =
                moderngekko::frontend::LauncherSavestateLabel(launcher_savestates[i], i == 0);
            const bool selected = selected_savestate == static_cast<int>(i);
            if (ImGui::Selectable(label.c_str(), selected))
              selected_savestate = static_cast<int>(i);
            if (selected)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        // Netplay starts every player from the same boot, so a state chosen by
        // one side would desync the session immediately.
        ImGui::TextDisabled("Solo play only.");
        ImGui::Spacing();
      }
      // Share the columns's width: three fixed 180 px buttons overflowed it.
      const float button_width =
          (ImGui::GetContentRegionAvail().x -
           ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
      if (ImGui::Button("Play", ImVec2(button_width, 42 * scale)))
        request_launch(LaunchMode::Solo);
      ImGui::SameLine();
      if (ImGui::Button("Host Netplay", ImVec2(button_width, 42 * scale)))
      {
        if (ensure_controller())
        {
          config.netplay_nickname = netplay_nickname.data();
          config.netplay_address = netplay_address.data();
          config.netplay_port = static_cast<std::uint16_t>(netplay_port);
          config.netplay_buffer = automatic_buffer ? "auto" : std::to_string(manual_buffer);
          config.controllers = configured_controllers.empty()
                                   ? std::vector<std::string>{selected_controller}
                                   : configured_controllers;
          config.controller = config.controllers.front();
          config.resolution = resolutions[resolution_index].text;
          config.show_fps_in_title = show_fps_in_title;
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            request_launch(LaunchMode::Host);
          }
          else
          {
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Join Netplay", ImVec2(button_width, 42 * scale)))
      {
        if (ensure_controller())
        {
          config.netplay_nickname = netplay_nickname.data();
          config.netplay_address = netplay_address.data();
          config.netplay_port = static_cast<std::uint16_t>(netplay_port);
          config.netplay_buffer = automatic_buffer ? "auto" : std::to_string(manual_buffer);
          config.controllers = configured_controllers.empty()
                                   ? std::vector<std::string>{selected_controller}
                                   : configured_controllers;
          config.controller = config.controllers.front();
          config.resolution = resolutions[resolution_index].text;
          config.show_fps_in_title = show_fps_in_title;
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            request_launch(LaunchMode::Join);
          }
          else
          {
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
      }
    }
    else
    {
      ImGui::TextUnformatted("No extracted game is configured yet.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Controller profile");
    const char* controller_preview = controller_index >= 0
                                         ? controllers[controller_index].label.c_str()
                                     : selected_controller.empty() ? "No SDL gamepad detected"
                                                                   : selected_controller.c_str();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##controller", controller_preview))
    {
      for (std::size_t i = 0; i < controllers.size(); ++i)
      {
        const bool selected = controller_index == static_cast<int>(i);
        if (ImGui::Selectable(controllers[i].label.c_str(), selected))
        {
          controller_index = static_cast<int>(i);
          selected_controller = controllers[i].device;
          controller_status =
              controller_profile_exists
                  ? "Existing " + std::string(CONTROLLER_PROFILE_NAME) + " unchanged"
                  : "Ready to generate controller profile";
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::BeginDisabled(controller_index < 0);
    const std::string replace_profile =
        "Replace with generated " + std::string(GENERATED_PROFILE_LABEL);
    if (ImGui::Button(replace_profile.c_str()))
      select_controller(controller_index);
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextUnformatted("Netplay");
    ImGui::SetNextItemWidth(220 * scale);
    ImGui::InputText("Nickname", netplay_nickname.data(), netplay_nickname.size());
    ImGui::SetNextItemWidth(220 * scale);
    ImGui::InputText("Host / IP", netplay_address.data(), netplay_address.size());
    ImGui::SetNextItemWidth(120 * scale);
    ImGui::InputInt("UDP port", &netplay_port);
    netplay_port = std::clamp(netplay_port, 1, 65535);
    ImGui::Checkbox("Automatic input buffer", &automatic_buffer);
    if (!automatic_buffer)
    {
      ImGui::SetNextItemWidth(180 * scale);
      ImGui::SliderInt("Buffer frames", &manual_buffer, 1, 20);
    }
    ImGui::EndChild();
    ImGui::SameLine(0.0f, column_gap);
    ImGui::BeginChild("##launcher_right", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::TextUnformatted("Graphics backend");
    if (ImGui::BeginCombo("##graphics_backend",
                          graphics_backends[graphics_backend_index].text))
    {
      for (std::size_t i = 0; i < graphics_backends.size(); ++i)
      {
        const bool selected = graphics_backend_index == static_cast<int>(i);
        if (ImGui::Selectable(graphics_backends[i].text, selected))
        {
          const std::string previous = config.graphics_backend;
          config.graphics_backend = graphics_backends[i].value;
          config.resolution = resolutions[resolution_index].text;
          config.show_fps_in_title = show_fps_in_title;
          config.controller = selected_controller;
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            graphics_backend_index = static_cast<int>(i);
          }
          else
          {
            config.graphics_backend = previous;
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::Spacing();
    const bool previous_fullscreen = fullscreen;
    if (ImGui::Checkbox("Start in fullscreen", &fullscreen))
    {
      config.fullscreen = fullscreen;
      config.resolution = resolutions[resolution_index].text;
      config.show_fps_in_title = show_fps_in_title;
      config.controller = selected_controller;
      std::string error;
      if (!moderngekko::frontend::SaveConfig(user_directory, config, &error))
      {
        fullscreen = previous_fullscreen;
        config.fullscreen = previous_fullscreen;
        std::lock_guard lock(dialog.mutex);
        dialog.error = std::move(error);
      }
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Internal resolution (Dolphin EFB upscale)");
    if (ImGui::BeginCombo("##resolution", resolutions[resolution_index].text))
    {
      for (std::size_t i = 0; i < resolutions.size(); ++i)
      {
        const bool selected = resolution_index == static_cast<int>(i);
        if (ImGui::Selectable(resolutions[i].text, selected))
        {
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, resolutions[i].text,
                                                show_fps_in_title, selected_controller, &error))
          {
            resolution_index = static_cast<int>(i);
            config.resolution = resolutions[i].text;
          }
          else
          {
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
      }
      ImGui::EndCombo();
    }
    const bool previous_show_fps_in_title = show_fps_in_title;
    if (ImGui::Checkbox("Show FPS in window title", &show_fps_in_title))
    {
      std::string error;
      if (!moderngekko::frontend::SaveConfig(user_directory, resolutions[resolution_index].text,
                                             show_fps_in_title, selected_controller, &error))
      {
        show_fps_in_title = previous_show_fps_in_title;
        std::lock_guard lock(dialog.mutex);
        dialog.error = std::move(error);
      }
      else
      {
        config.show_fps_in_title = show_fps_in_title;
      }
    }
    ImGui::TextUnformatted("Game disc image");
    if (selected_image)
      ImGui::TextWrapped("%s", selected_image->string().c_str());
    else
      ImGui::TextDisabled("No ISO, WBFS, or RVZ selected");

    if (!extraction.running)
    {
      if (ImGui::Button("Browse for ISO / WBFS / RVZ"))
        ShowDiscImageDialog(window, &dialog);
      if (selected_image)
      {
        ImGui::SameLine();
        if (ImGui::Button("Extract and Play"))
          begin_extraction(*selected_image);
      }
    }
    else
    {
      const float progress =
          std::min(1.0f, static_cast<float>(extraction.completed.load()) / extraction.total.load());
      ImGui::ProgressBar(progress, ImVec2(-1, 0));
    }

    {
      std::lock_guard lock(extraction.mutex);
      if (!extraction.status.empty())
        ImGui::TextWrapped("%s", extraction.status.c_str());
      if (!extraction.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", extraction.error.c_str());
    }
    {
      std::lock_guard lock(dialog.mutex);
      if (!dialog.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", dialog.error.c_str());
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("Controller: %s", controller_status.c_str());
    ImGui::End();

    // --- First run: choose the disc image ---------------------------------
    // A freshly unpacked build has no default-game.txt and no extracted game,
    // so this modal is the first thing a new player sees. Nothing else in the
    // window is useful before there is a game: settings without a game cannot
    // be applied to anything.
    //
    // The popup is opened once (latch) but always ended once opened: ImGui
    // keeps a modal in its popup stack until CloseCurrentPopup, and a modal that
    // stops being drawn without being closed keeps swallowing input for the
    // whole window. Extraction can finish without a launch when no gamepad is
    // plugged in, so that case has to close the card explicitly.
    if (!current_metadata && !first_run_card)
    {
      LauncherLog("first-run card opened (no game configured)");
      ImGui::OpenPopup("##first_run");
      first_run_card = true;
      card_frames = 0;
    }
    if (first_run_card)
    {
      const ImGuiViewport* viewport_center = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(
          ImVec2(viewport_center->WorkPos.x + viewport_center->WorkSize.x * 0.5f,
                 viewport_center->WorkPos.y + viewport_center->WorkSize.y * 0.5f),
          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowSize(ImVec2(600 * scale, 0), ImGuiCond_Appearing);
      if (ImGui::BeginPopupModal("##first_run", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings))
      {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 10 * scale));
        // The card's item rectangles are logged for the first few frames so
        // tools/firstrun-ui.sh can assert that every control is laid out inside
        // the window: OCR proves the wording, these rects prove that a button
        // was not pushed off the edge of the card (which is how the in-game
        // menu lost its bottom row before it was split into tabs).
        ++card_frames;
        // Frames 2 and 3 are the settled layout: on the first frame the popup
        // has not been centred yet and ImGui reports the pre-resize window
        // size, which would make every assertion below read stale numbers.
        const bool log_card_items = card_frames == 2 || card_frames == 3;
        const auto log_card_item = [&](const char* label)
        {
          if (!log_card_items)
            return;
          const ImVec2 min = ImGui::GetItemRectMin();
          const ImVec2 max = ImGui::GetItemRectMax();
          LauncherLog("card item: %s x=%.0f y=%.0f w=%.0f h=%.0f", label, min.x,
                      min.y, max.x - min.x, max.y - min.y);
        };
        ImGui::TextUnformatted("Welcome to");
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.9f);
        ImGui::TextUnformatted(MODERNGEKKO_FRONTEND_NAME);
        ImGui::PopFont();
        log_card_item("title");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Choose your game disc image to start. It is extracted once into your "
            "user folder; after that the game launches straight from here and the "
            "original ISO is no longer needed.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Disc image:");
        if (selected_image)
          ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                             selected_image->string().c_str());
        else
          ImGui::TextDisabled("None selected yet");

        if (!images.empty() && !extraction.running)
        {
          ImGui::Spacing();
          ImGui::TextDisabled("Found these, if you would rather pick one of them:");
          log_card_item("found");
          for (std::size_t i = 0; i < images.size() && i < 6; ++i)
          {
            const bool picked = selected_image && *selected_image == images[i];
            if (ImGui::Selectable(images[i].string().c_str(), picked))
              selected_image = images[i];
          }
        }

        ImGui::Spacing();
        if (extraction.running)
        {
          const float progress = std::min(
              1.0f, static_cast<float>(extraction.completed.load()) / extraction.total.load());
          ImGui::ProgressBar(progress, ImVec2(-1, 0));
          std::lock_guard lock(extraction.mutex);
          if (!extraction.status.empty())
            ImGui::TextWrapped("%s", extraction.status.c_str());
        }
        else
        {
          if (ImGui::Button("Choose disc image...", ImVec2(220 * scale, 40 * scale)))
            ShowDiscImageDialog(window, &dialog);
          log_card_item("choose");
          ImGui::SameLine();
          const bool can_start = selected_image.has_value();
          ImGui::BeginDisabled(!can_start);
          if (ImGui::Button("Start", ImVec2(140 * scale, 40 * scale)))
            begin_extraction(*selected_image);
          ImGui::EndDisabled();
          log_card_item("start");
          if (!can_start)
          {
            ImGui::Spacing();
            ImGui::TextDisabled("Pick an ISO, RVZ or WBFS file to continue.");
            log_card_item("hint");
          }
        }

        {
          std::lock_guard lock(extraction.mutex);
          if (!extraction.error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", extraction.error.c_str());
        }
        {
          std::lock_guard lock(dialog.mutex);
          if (!dialog.error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", dialog.error.c_str());
        }
        if (controllers.empty() && !controller_profile_exists)
        {
          ImGui::Spacing();
          ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                             "No gamepad detected. Plug one in before starting - the game "
                             "is played with a controller.");
        }
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Quit", ImVec2(100 * scale, 32 * scale)))
          done = true;
        log_card_item("quit");
        if (log_card_items)
        {
          const ImVec2 card = ImGui::GetWindowSize();
          LauncherLog("card size: w=%.0f h=%.0f window=%.0fx%.0f", card.x, card.y,
                      ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
        }
        // Extraction succeeded but nothing launched (normally: no gamepad), so
        // dismiss the card and let the normal settings window take over.
        if (current_metadata)
        {
          LauncherLog("first-run card closed (a game is now available)");
          ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
      }
      else
      {
        // Closed (or never reopened): stop trying to draw it.
        LauncherLog("first-run card dismissed");
        first_run_card = false;
      }
    }

    // --- module gate -------------------------------------------------------
    // Drawn last so it sits over the launcher window. Keeping this in-window
    // (rather than an SDL message box) is deliberate: the box could fail to
    // appear and the process then exited silently, which read as "the app just
    // closes" right after the prompt.
    if (gate == ModuleGate::Building && build_done.load())
    {
      if (build_ok.load())
      {
        launch_mode = pending_launch;
        gate = ModuleGate::None;
        done = true;
      }
      else
      {
        gate = ModuleGate::Failed;
      }
    }
    if (gate != ModuleGate::None)
    {
      const std::string module_name = module_name_for_disc();
      const ImGuiViewport* gate_viewport = ImGui::GetMainViewport();
      // Dim the launcher behind the gate: without it the settings text shows
      // through and the dialog reads as overlapping layers.
      ImGui::GetBackgroundDrawList()->AddRectFilled(
          gate_viewport->Pos,
          ImVec2(gate_viewport->Pos.x + gate_viewport->Size.x,
                 gate_viewport->Pos.y + gate_viewport->Size.y),
          IM_COL32(0, 0, 0, 150));
      ImGui::SetNextWindowBgAlpha(1.0f);
      ImGui::SetNextWindowPos(
          ImVec2(gate_viewport->GetCenter().x, gate_viewport->GetCenter().y),
          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowSize(ImVec2(680 * scale, 0), ImGuiCond_Always);
      ImGui::Begin("Recompiled game module", nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
      switch (gate)
      {
      case ModuleGate::Ask:
      {
        if (!auto_build_done && SDL_getenv("MODERNGEKKO_LAUNCHER_AUTOBUILD") != nullptr &&
            !port_tool.empty())
        {
          auto_build_done = true;
          gate = ModuleGate::Building;
          build_done.store(false);
          build_ok.store(false);
          build_error.clear();
          build_start = std::chrono::steady_clock::now();
          const fs::path output = modules_dir / module_name;
          const fs::path work_dir = modules_dir / "build";
          const fs::path log_path = user_directory / "Logs" / "module-build.log";
          const bool bundled = UseBundledToolchain(exe_dir);
          LauncherLog("module build started: %s", output.string().c_str());
          build_thread = std::jthread(
              [&, output, work_dir, log_path, bundled]
              {
                std::string error;
                const bool ok = BuildModuleFromDisc(port_tool, current_game, work_dir, output,
                                                    log_path, bundled, compiler_path, &error);
                build_error = std::move(error);
                build_ok.store(ok);
                build_done.store(true);
              });
          break;
        }
        ImGui::TextWrapped(
            "This package ships no compiled game module. The module is generated from your "
            "own disc and cannot be redistributed, so it is built here instead.");
        ImGui::Spacing();
        ImGui::TextWrapped("Build %s now? This compiles the game's code and takes several "
                           "minutes.",
                           module_name.c_str());
        ImGui::Spacing();
        if (port_tool.empty())
        {
          ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                             "moderngekko-port is missing from this install, so it cannot be "
                             "built here.");
          if (ImGui::Button("Close"))
            gate = ModuleGate::None;
        }
        else
        {
          if (ImGui::Button("Build it now", ImVec2(180 * scale, 36 * scale)))
          {
            gate = ModuleGate::Building;
            build_done.store(false);
            build_ok.store(false);
            build_error.clear();
            build_start = std::chrono::steady_clock::now();
            const fs::path output = modules_dir / module_name;
            const fs::path work_dir = modules_dir / "build";
            const fs::path log_path = user_directory / "Logs" / "module-build.log";
            const bool bundled = UseBundledToolchain(exe_dir);
            LauncherLog("module build started: %s", output.string().c_str());
            build_thread = std::jthread(
                [&, output, work_dir, log_path, bundled]
                {
                  std::string error;
                  const bool ok = BuildModuleFromDisc(port_tool, current_game, work_dir, output,
                                                      log_path, bundled, compiler_path, &error);
                  build_error = std::move(error);
                  build_ok.store(ok);
                  build_done.store(true);
                });
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancel", ImVec2(120 * scale, 36 * scale)))
            gate = ModuleGate::None;
          ImGui::Spacing();
          if (compiler_version.empty())
          {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                               "No usable C compiler was found, so the build cannot "
                               "start. Put a clang/gcc on PATH, or point config.ini's "
                               "[Build] compiler= at one.");
          }
          else
          {
            ImGui::TextDisabled("Compiler: %s", compiler_version.c_str());
            ImGui::TextDisabled("%s", compiler_path.c_str());
          }
        }
        break;
      }
      case ModuleGate::Building:
      {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - build_start);
        ImGui::Text("Building %s ... %llds", module_name.c_str(),
                    static_cast<long long>(elapsed.count()));
        ImGui::Spacing();
        ImGui::TextWrapped("The window stays up until the build finishes. Full output: %s",
                           (user_directory / "Logs" / "module-build.log").string().c_str());
        break;
      }
      case ModuleGate::Failed:
      {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "The module build failed.");
        ImGui::Spacing();
        if (!build_error.empty())
          ImGui::TextWrapped("%s", build_error.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Try again", ImVec2(140 * scale, 36 * scale)))
          gate = ModuleGate::Ask;
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120 * scale, 36 * scale)))
          gate = ModuleGate::None;
        break;
      }
      case ModuleGate::None:
        break;
      }
      ImGui::End();
    }

    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    // Before the swap, while the backbuffer still holds this frame.
    dump_frame();
    SDL_RenderPresent(renderer);
  }

  if (extraction_thread.joinable())
    extraction_thread.join();

  int result = 0;
  if (build_thread.joinable())
    build_thread.join();

  if (launch_mode != LaunchMode::None)
  {
    std::string launch_error;
    if (!WriteDefaultGame(user_directory, release_directory, current_game,
                          &launch_error))
    {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed", launch_error.c_str(), window);
      result = 1;
    }
    else
    {
      std::vector<std::string> argument_storage = {SiblingRunner(argv[0]).string(), "--game",
                                                   current_game.string(), "--user-dir",
                                                   user_directory.string()};
      // Solo only: see the note by the dropdown.
      if (launch_mode == LaunchMode::Solo && selected_savestate >= 0 &&
          static_cast<std::size_t>(selected_savestate) < launcher_savestates.size())
      {
        argument_storage.emplace_back("--load-state");
        argument_storage.emplace_back(
            moderngekko::frontend::PathText(
                launcher_savestates[static_cast<std::size_t>(selected_savestate)]));
      }
      if (launch_mode == LaunchMode::Host)
        argument_storage.emplace_back("--netplay-host");
      else if (launch_mode == LaunchMode::Join)
      {
        argument_storage.emplace_back("--netplay-join");
        argument_storage.emplace_back(config.netplay_address);
      }
      if (launch_mode == LaunchMode::Host || launch_mode == LaunchMode::Join)
      {
        argument_storage.emplace_back("--netplay-port");
        argument_storage.emplace_back(std::to_string(config.netplay_port));
        argument_storage.emplace_back("--nickname");
        argument_storage.emplace_back(config.netplay_nickname);
        argument_storage.emplace_back("--buffer");
        argument_storage.emplace_back(config.netplay_buffer);
        for (const std::string& controller : config.controllers)
        {
          argument_storage.emplace_back("--controller");
          argument_storage.emplace_back(controller);
        }
      }
      if (use_wayland)
        argument_storage.emplace_back("--wayland");
#if defined(__linux__)
      else
        argument_storage.emplace_back("-X11");
#endif
      std::vector<const char*> arguments;
      arguments.reserve(argument_storage.size() + 1);
      for (const std::string& argument : argument_storage)
        arguments.push_back(argument.c_str());
      arguments.push_back(nullptr);
      const std::filesystem::path log_path =
          user_directory / "Logs" / MODERNGEKKO_LOG_FILENAME;
      std::error_code log_error;
      std::filesystem::create_directories(log_path.parent_path(), log_error);
      SDL_IOStream* log_stream =
          log_error ? nullptr : SDL_IOFromFile(log_path.string().c_str(), "w");
      SDL_Process* process = nullptr;
      std::string process_error;
      if (log_stream)
      {
        const SDL_PropertiesID properties = SDL_CreateProperties();
        if (properties)
        {
          SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                                 arguments.data());
          SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                                SDL_PROCESS_STDIO_REDIRECT);
          SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log_stream);
          SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN,
                                 true);
          process = SDL_CreateProcessWithProperties(properties);
          SDL_DestroyProperties(properties);
        }
        if (!process)
          process_error = SDL_GetError();
        SDL_CloseIO(log_stream);
      }
      else
      {
        process = SDL_CreateProcess(arguments.data(), false);
        if (!process)
          process_error = SDL_GetError();
      }
      if (!process)
      {
        launch_error = "Could not start " + SiblingRunner(argv[0]).string() + ": " + process_error;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed", launch_error.c_str(),
                                 window);
        result = 1;
      }
      else
      {
        SDL_HideWindow(window);
        int exit_code = 1;
        const bool waited = SDL_WaitProcess(process, true, &exit_code);
        SDL_DestroyProcess(process);
        if (!waited || exit_code != 0)
        {
          SDL_ShowWindow(window);
          if (!waited)
          {
            launch_error =
                "The game process could not be monitored: " + std::string(SDL_GetError());
          }
          else if (launch_mode == LaunchMode::Join)
          {
            switch (static_cast<moderngekko::frontend::NetplayExitCode>(exit_code))
            {
            case moderngekko::frontend::NetplayExitCode::VersionMismatch:
              launch_error = "The host is running an incompatible netplay build. Both "
                             "players must use the same release.";
              break;
            case moderngekko::frontend::NetplayExitCode::CompatibilityMismatch:
              launch_error = "The extracted game or recomp module does not match the "
                             "host. Both players need the same game revision and "
                             "release.";
              break;
            case moderngekko::frontend::NetplayExitCode::RoomFull:
              launch_error = "All four controller slots are already occupied.";
              break;
            case moderngekko::frontend::NetplayExitCode::GameRunning:
              launch_error = "The host has already started the game.";
              break;
            case moderngekko::frontend::NetplayExitCode::ServerFull:
              launch_error = "The netplay server is full.";
              break;
            case moderngekko::frontend::NetplayExitCode::NicknameRejected:
              launch_error = "The nickname was rejected by the host.";
              break;
            default:
              launch_error = "Could not reach the netplay host. Check the host name, UDP "
                             "port, and firewall.";
              break;
            }
          }
          else if (launch_mode == LaunchMode::Host)
          {
            launch_error = "Could not create the netplay session. Check the UDP port "
                           "and firewall settings.";
          }
          else
          {
            launch_error = "The game process exited with code " + std::to_string(exit_code) + ".";
          }
          if (!log_error)
            launch_error += "\n\nDetails were written to:\n" + log_path.string();
          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Session failed", launch_error.c_str(),
                                   window);
          result = 1;
        }
      }
    }
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
