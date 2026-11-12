#pragma once

#include "pgo_support.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace moderngekko::port
{
namespace fs = std::filesystem;

struct BuildOptions
{
  std::string toolchain = "auto";
  // Explicit compiler executable (or a bare name on PATH). The launcher probes
  // the compiler it is about to use and passes the exact path, so a package that
  // ships its own clang does not depend on PATH resolution order and a player
  // who points at their own compiler gets that one instead.
  std::string compiler;
  // Explicit Ninja (or any build program). CMake otherwise searches PATH, and
  // on Windows the bundled Toolchain/bin entry does not always survive into the
  // child's environment block -- configure then fails with "unable to find a
  // build program corresponding to Ninja", which is what a shipped Windows
  // package hit on its first real run.
  std::string ninja;
  // Python for the module template's ABI table generation. Passed to CMake as
  // Python3_EXECUTABLE for the same reason ninja is pinned: a packaged Python
  // has to be reachable without relying on PATH surviving into a grandchild.
  std::string python;
  std::string opt_level;
  std::string backend;
  fs::path output;
  std::vector<std::string> runner_arguments;
  pgo::PgoBuildOptions pgo;
  pgo::ManifestFacts pgo_facts;
  bool make_active = true;
  // Skip ThinLTO. The on-demand build inside the launcher uses this: the LTO
  // link is the slowest and most fragile step, and a user building once wants
  // a game, not the last few percent.
  bool no_lto = false;
};

struct PgoRunOptions
{
  fs::path profile_dir;
  fs::path llvm_profdata;
  bool keep_work = false;
};

struct CommandLine
{
  std::string command;
  fs::path root;
  BuildOptions build;
  PgoRunOptions pgo_run;
  std::string error;
  bool usage = false;

  explicit operator bool() const { return error.empty() && !usage; }
};

inline bool IsKnownCommand(std::string_view command)
{
  return command == "inspect" || command == "build" || command == "run" || command == "pgo-run";
}

inline CommandLine ParseCommandLine(int argc, const char* const* argv,
                                    std::string_view default_backend)
{
  CommandLine parsed;
  parsed.build.backend = std::string(default_backend);
  if (argc < 3)
  {
    parsed.usage = true;
    return parsed;
  }
  parsed.command = argv[1];
  parsed.root = fs::path(argv[2]);
  if (!IsKnownCommand(parsed.command))
  {
    parsed.usage = true;
    return parsed;
  }

  const bool forwards_runner_arguments =
      parsed.command == "run" || parsed.command == "pgo-run";
  const bool is_pgo_run = parsed.command == "pgo-run";
  bool runner_arguments = false;
  for (int i = 3; i < argc; ++i)
  {
    const std::string argument = argv[i];
    if (runner_arguments)
      parsed.build.runner_arguments.push_back(argument);
    else if (argument == "--")
      runner_arguments = true;
    else if (argument == "--toolchain" && i + 1 < argc)
      parsed.build.toolchain = argv[++i];
    else if (argument == "--compiler" && i + 1 < argc)
      parsed.build.compiler = argv[++i];
    else if (argument == "--ninja" && i + 1 < argc)
      parsed.build.ninja = argv[++i];
    else if (argument == "--python" && i + 1 < argc)
      parsed.build.python = argv[++i];
    else if (argument == "--backend" && i + 1 < argc)
      parsed.build.backend = argv[++i];
    else if (argument == "--opt-level" && i + 1 < argc)
      parsed.build.opt_level = argv[++i];
    else if (argument == "--output" && i + 1 < argc)
      parsed.build.output = fs::path(argv[++i]);
    else if (argument == "--no-lto")
      parsed.build.no_lto = true;
    else if (argument == "--profile-dir" && i + 1 < argc && is_pgo_run)
      parsed.pgo_run.profile_dir = fs::path(argv[++i]);
    else if (argument == "--llvm-profdata" && i + 1 < argc && is_pgo_run)
      parsed.pgo_run.llvm_profdata = fs::path(argv[++i]);
    else if (argument == "--keep-work" && is_pgo_run)
      parsed.pgo_run.keep_work = true;
    else if (forwards_runner_arguments)
      parsed.build.runner_arguments.push_back(argument);
    else
    {
      parsed.error = "unknown or incomplete option: " + argument;
      return parsed;
    }
  }

  if (parsed.build.backend != "c" && parsed.build.backend != "llvm")
  {
    parsed.error = "unknown backend: " + parsed.build.backend;
    return parsed;
  }
  if (!parsed.build.opt_level.empty() &&
      (parsed.build.opt_level.size() != 1 || parsed.build.opt_level[0] < '0' ||
       parsed.build.opt_level[0] > '3'))
  {
    parsed.error = "opt level must be 0, 1, 2, or 3: " + parsed.build.opt_level;
    return parsed;
  }
  return parsed;
}
}  // namespace moderngekko::port
