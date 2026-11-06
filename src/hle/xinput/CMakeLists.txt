# XInput HLE (owner: Kernel HLE): hp2_xinput (game-facing, no SDL) and
# hp2_xinput_sdl (SDL3 gamepad/keyboard backend).
add_library(hp2_xinput STATIC xinput.cpp pad_script.cpp input_filter.cpp camera.cpp)
target_link_libraries(hp2_xinput PUBLIC hp2_kernel)
target_compile_options(hp2_xinput PRIVATE -Wall -Wextra -Wno-unused-parameter)

find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_library(hp2_xinput_sdl STATIC xinput_sdl.cpp)
  target_link_libraries(hp2_xinput_sdl PUBLIC hp2_xinput SDL3::SDL3)
endif()
