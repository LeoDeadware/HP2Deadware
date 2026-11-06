# hp2_runtime: loader core, dispatch (overrides, kernel/host thunks), guest
# threads, logging. Public API for HLE roles: hp2_runtime.h.
add_library(hp2_runtime STATIC
  hp2_runtime.cpp
  hp2_dispatch.cpp
  hp2_thread.cpp
  hp2_boot.cpp
  hp2_game_fixes.cpp
  # x86recomp's platform layer: fault context decoding and the signal-frame
  # redirect used to leave a fault with a guest exception.
  ${X86RECOMP_DIR}/src/runtime/platform.cpp)
target_include_directories(hp2_runtime PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hp2_runtime PUBLIC recomp_x87 recomp_image)
target_compile_options(hp2_runtime PRIVATE -Wall -Wno-unused-function)

# MMX lane helpers (x86recomp patch) against hand-checked values.
add_executable(hp2_mmx_test tests/mmx_test.cpp)
target_link_libraries(hp2_mmx_test PRIVATE recomp_x87)
