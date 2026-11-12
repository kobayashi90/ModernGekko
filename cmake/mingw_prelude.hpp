// Precompiled-in prelude for mingw/clang Windows builds.
//
// The vendored Dolphin tree was written against MSVC, whose STL pulls these in
// transitively; libc++ with llvm-mingw does not, so a handful of headers use
// std::vector/std::string without including them (CustomTextureData.h is the
// first one a mingw build hits). This tree is vendored and re-synced, so the
// includes are carried here instead of patching hundreds of files we do not own.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
