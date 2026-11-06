# Xbox kernel HLE (owner: Kernel HLE). Usable from the top-level build
# (add_subdirectory(src/hle/kernel)) or standalone for its unit tests:
#   cmake -S src/hle/kernel -B out/kernel -G Ninja && ninja -C out/kernel -j3
#   out/kernel/hp2_kernel_tests
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(hp2_kernel CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  find_program(CCACHE ccache)
  if(CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
  endif()
  set(HP2_STANDALONE ON)
endif()

get_filename_component(HP2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(X86RECOMP_SRC "${HP2_ROOT}/third_party/x86recomp/src" CACHE PATH "x86recomp src/ (for runtime/ctx.h)")

add_library(hp2_kernel STATIC
  kernel.cpp memory.cpp time.cpp sync.cpp thread.cpp vfs.cpp file.cpp misc.cpp
  runtime_glue.cpp crash.cpp help.cpp)
target_include_directories(hp2_kernel PUBLIC "${HP2_ROOT}/src" "${X86RECOMP_SRC}")
target_compile_options(hp2_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-multichar)
# Exports register themselves from static initialisers: keep every object.
if(APPLE)
  target_link_options(hp2_kernel INTERFACE "-Wl,-force_load,$<TARGET_FILE:hp2_kernel>")
endif()

if(HP2_STANDALONE)
  # Unit tests run on a fake host, without hp2_runtime: no runtime glue.
  target_compile_definitions(hp2_kernel PRIVATE HP2K_NO_RUNTIME_GLUE=1)
  add_subdirectory(../xinput xinput)
  add_executable(hp2_kernel_tests tests/kernel_tests.cpp)
  target_link_libraries(hp2_kernel_tests PRIVATE hp2_kernel hp2_xinput)
  if(TARGET hp2_xinput_sdl)
    # SDL backend tests on virtual gamepads (no device needed).
    target_link_libraries(hp2_kernel_tests PRIVATE hp2_xinput_sdl)
    target_compile_definitions(hp2_kernel_tests PRIVATE HP2_TEST_SDL=1)
  endif()
endif()
