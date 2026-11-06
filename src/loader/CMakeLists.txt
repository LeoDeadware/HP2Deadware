# hp2: the executable. Links the published lift this worktree pins
# (HP2_LIFT_DIR/lib/libhp2_game.a) and, when present, the HLE libraries
# (whole-archive: their exports and HP2_OVERRIDEs register from static
# initialisers and override the runtime's weak fallbacks).
set(HP2_KERNEL_DIR "${CMAKE_SOURCE_DIR}/src/hle/kernel" CACHE PATH
    "Kernel HLE sources (src/hle/kernel, or a checkout of its branch)")
set(_game_lib "${HP2_LIFT_DIR}/lib/libhp2_game.a")
if(NOT EXISTS "${_game_lib}")
  message(WARNING "no published lift at ${HP2_LIFT_DIR} (lift/LIFT_ID=${HP2_LIFT_ID}); "
                  "run lift/lift.sh --pin. The hp2 executable is not configured.")
  return()
endif()
add_executable(hp2 main.cpp)
target_link_libraries(hp2 PRIVATE hp2_runtime "${_game_lib}")
# Kernel HLE hooks the runtime/loader use when present (weak: crash reports,
# --help text); resolved by hp2_kernel when it is linked.
target_link_options(hp2 PRIVATE "-Wl,-U,__Z14hp2_crash_notePKc" "-Wl,-U,__Z16hp2_crash_reportPKc"
                    "-Wl,-U,__Z13hp2_help_textv")
if(EXISTS "${HP2_KERNEL_DIR}/CMakeLists.txt")
  set(X86RECOMP_SRC "${X86RECOMP_DIR}/src" CACHE PATH "" FORCE)
  add_subdirectory("${HP2_KERNEL_DIR}" "${CMAKE_BINARY_DIR}/hle_kernel")
  # This tree's runtime header, also when the kernel comes from elsewhere.
  target_include_directories(hp2_kernel PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(hp2 PRIVATE hp2_kernel)
  message(STATUS "hp2: kernel HLE from ${HP2_KERNEL_DIR}")
  # XInput HLE (game-facing part; the SDL backend joins with the frame loop).
  if(EXISTS "${HP2_KERNEL_DIR}/../xinput/CMakeLists.txt")
    add_subdirectory("${HP2_KERNEL_DIR}/../xinput" "${CMAKE_BINARY_DIR}/hle_xinput")
    target_link_libraries(hp2 PRIVATE hp2_xinput)
    target_compile_definitions(hp2 PRIVATE HP2_HAVE_XINPUT=1)
  endif()
  # D3D8 HLE (Render): needs the kernel's vblank pacer.
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/hle/d3d8/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/hle/d3d8" "${CMAKE_BINARY_DIR}/hle_d3d8")
    target_link_libraries(hp2 PRIVATE hp2_d3d8)
  endif()
  # DirectSound HLE (Audio): needs the kernel (NtSetEvent for stream packets).
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/hle/dsound/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/hle/dsound" "${CMAKE_BINARY_DIR}/hle_dsound")
    target_link_libraries(hp2 PRIVATE hp2_dsound)
  endif()
  # Polish (opt-in enhancements: HP2_WIDESCREEN, ...).
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/polish/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/polish" "${CMAKE_BINARY_DIR}/polish")
    target_link_libraries(hp2 PRIVATE "-Wl,-force_load,$<TARGET_FILE:hp2_polish>" hp2_polish)
  endif()
else()
  message(STATUS "hp2: no kernel HLE (runtime stops at the first kernel call)")
endif()
