# Polish: optional enhancements over the lifted game (widescreen, the TV 2002 look, ...).
# Linked whole-archive: HP2_OVERRIDEs register from static initialisers.
enable_language(OBJCXX)
add_library(hp2_polish STATIC widescreen.cpp skip.cpp postfx/postfx.mm texpack/texpack.mm)
target_include_directories(hp2_polish PRIVATE "${CMAKE_SOURCE_DIR}/src" "${X86RECOMP_DIR}/src")
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(hp2_polish PRIVATE hp2_runtime SDL3::SDL3 "-framework Metal" "-framework Foundation"
  "-framework CoreGraphics" "-framework ImageIO")
target_compile_options(hp2_polish PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc -Wall>)
# hp2_d3d8_set_widescreen / _postfx / _texture_hook (C++, mangled) are weak: resolved when
# the D3D8 HLE provides them.
target_link_options(hp2_polish INTERFACE "-Wl,-U,_hp2_d3d8_set_widescreen" "-Wl,-U,_hp2_d3d8_set_postfx"
  "-Wl,-U,__Z25hp2_d3d8_set_texture_hookPFbRK14Hp2TextureInfoPjS2_PPKhE")
