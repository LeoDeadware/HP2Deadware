cmake_minimum_required(VERSION 3.25)
project(HP2Port LANGUAGES C CXX)

# Two layers (CLAUDE.md "Build architecture"):
# - libhp2_game.a: lifted game code, built once per lift and published to
#   .cache/HP2Cache/lift/<lift_id>/ (lift/lift.sh); this build only links it.
# - hp2_runtime + HLE libraries + the `hp2` executable: rebuilt in seconds.
#
#   cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build out -j3

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Untracked caches live in the main checkout (worktrees share them): $HP2_CACHE, else
# <main checkout>/.cache/HP2Cache (tools/main_checkout.sh; no home paths in the repo).
if(DEFINED ENV{HP2_CACHE})
  set(HP2_CACHE_DIR "$ENV{HP2_CACHE}")
else()
  execute_process(COMMAND "${CMAKE_SOURCE_DIR}/tools/main_checkout.sh"
                  OUTPUT_VARIABLE _hp2_main OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT _hp2_main)
    set(_hp2_main "${CMAKE_SOURCE_DIR}")
  endif()
  set(HP2_CACHE_DIR "${_hp2_main}/.cache/HP2Cache")
endif()

# Shared ccache (CCACHE_BASEDIR = this worktree, so worktrees share hits).
find_program(CCACHE ccache)
if(CCACHE)
  set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env
      CCACHE_DIR=${HP2_CACHE_DIR}/ccache CCACHE_BASEDIR=${CMAKE_SOURCE_DIR}
      CCACHE_NOHASHDIR=1 ${CCACHE})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_C_COMPILER_LAUNCHER})
endif()

# No /Users/ paths in our binaries (__FILE__ in asserts and logs): the DMG's build kit ships our
# libraries, and tests/scripts/dmg_no_game_code.sh rejects any home or build path (ROTK 2026-11-06).
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=" "-ffile-prefix-map=$ENV{HOME}/=~/")

include(cmake/x86recomp.cmake)

# The published lift this worktree is pinned to (lift/LIFT_ID).
file(STRINGS "${CMAKE_SOURCE_DIR}/lift/LIFT_ID" _lift_line REGEX "^LIFT_ID=")
string(REGEX REPLACE "^LIFT_ID=" "" HP2_LIFT_ID "${_lift_line}")
set(HP2_LIFT_ROOT "${HP2_CACHE_DIR}/lift" CACHE PATH "")
# HP2_LIFT_DIR overrides the pin (e.g. -DHP2_LIFT_DIR=/path/to/a/lift); not
# cached, so a new lift/LIFT_ID takes effect on the next configure.
if(NOT HP2_LIFT_DIR)
  set(HP2_LIFT_DIR "${HP2_LIFT_ROOT}/${HP2_LIFT_ID}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/lift/LIFT_ID")

add_subdirectory(src/runtime)
add_subdirectory(src/loader)
