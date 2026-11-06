# D3D8 HLE (owner: Render): the D3D section functions on Metal.
# The overrides register from static initialisers: link whole-archive.
enable_language(OBJCXX)
add_library(hp2_d3d8 STATIC
  d3d8.cpp
  vsh.cpp
  psh.cpp
  textures.cpp
  movies.cpp
  window.mm
  video_settings.cpp
  gpu_metal.mm)
target_include_directories(hp2_d3d8 PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(hp2_d3d8 PUBLIC hp2_runtime hp2_kernel hp2_xinput_sdl SDL3::SDL3
  "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework CoreGraphics"
  "-framework ImageIO")
target_compile_options(hp2_d3d8 PRIVATE -Wall -Wno-unused-function
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
if(APPLE)
  target_link_options(hp2_d3d8 INTERFACE "-Wl,-force_load,$<TARGET_FILE:hp2_d3d8>")
endif()

add_executable(hp2_d3d8_video_settings_test tests/video_settings_test.cpp video_settings.cpp)
target_include_directories(hp2_d3d8_video_settings_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(COMMAND add_test)
  add_test(NAME d3d8_video_settings COMMAND hp2_d3d8_video_settings_test)
endif()
