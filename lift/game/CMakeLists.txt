# libhp2_game.a: the lifted game code (lift/lift.sh builds it once per lift,
# under the `heavy` lock, and publishes it to .cache/HP2Cache/lift/<lift_id>/).
#
#   cmake -S lift/game -B <dir> -G Ninja -DHP2_GEN_DIR=<generated> -DX86RECOMP_DIR=<x86recomp>
cmake_minimum_required(VERSION 3.25)
project(hp2_game LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(CCACHE ccache)
if(CCACHE AND DEFINED ENV{HP2_CACHE})  # lift/lift.sh exports HP2_CACHE
  # Keyed by content: the same generated TU in another lift hits.
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env
      CCACHE_DIR=$ENV{HP2_CACHE}/ccache CCACHE_BASEDIR=${HP2_GEN_DIR}
      CCACHE_NOHASHDIR=1 ${CCACHE})
endif()

file(GLOB _gen "${HP2_GEN_DIR}/*.cpp")
if(NOT _gen)
  message(FATAL_ERROR "no generated code in ${HP2_GEN_DIR}")
endif()
add_library(hp2_game STATIC ${_gen})
target_include_directories(hp2_game PRIVATE "${X86RECOMP_DIR}/src")
# -O3: in-level frames 6-8% faster than -O2 (3.59 -> 3.35 ms unpaced, M1 Pro,
# 2026-10-31), phase-3 goldens bit-identical. Otherwise as x86recomp's
# recomp_add_lifted: always optimised; guest exceptions
# (GuestResume, the kernel's thread exit) unwind through lifted frames;
# exact x87 doubles; frame records for fault reports on Apple hosts.
target_compile_options(hp2_game PRIVATE -O3 -g0 -fno-strict-aliasing -fnon-call-exceptions
                       -ffp-contract=off -fno-fast-math
                       $<$<PLATFORM_ID:Darwin>:-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer>)
