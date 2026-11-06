# The parts of x86recomp HP2Port uses, as static libraries: the guest image
# loader (PE/XBE) + logging, the x87/SSE models, and the lifter tool. Its
# Win32 HLE, SDL and graphics stack are not built (Xbox HLE is ours).
#
# FetchContent sources (Zydis, SoftFloat) come from third_party/deps, which
# tools/bootstrap.sh fills at the pinned commits (no network at configure).
include(FetchContent)
set(X86RECOMP_DIR "${CMAKE_SOURCE_DIR}/third_party/x86recomp" CACHE PATH "")
set(HP2_DEPS_DIR "${CMAKE_SOURCE_DIR}/third_party/deps" CACHE PATH
    "FetchContent sources to use (<name>-src)" FORCE)
foreach(_dep zydis softfloat)
  string(TOUPPER ${_dep} _DEP)
  if(EXISTS "${HP2_DEPS_DIR}/${_dep}-src")
    set(FETCHCONTENT_SOURCE_DIR_${_DEP} "${HP2_DEPS_DIR}/${_dep}-src" CACHE PATH "" FORCE)
  endif()
endforeach()

if(APPLE)
  set(RECOMP_FAULT_FRAME_FLAGS -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
      CACHE INTERNAL "")
else()
  set(RECOMP_FAULT_FRAME_FLAGS "" CACHE INTERNAL "")
endif()
set(RECOMP_X87_FP_FLAGS -ffp-contract=off -fno-fast-math CACHE INTERNAL "")
set(RECOMP_ROOT "${X86RECOMP_DIR}" CACHE INTERNAL "")

add_library(recomp_image STATIC
  ${X86RECOMP_DIR}/src/runtime/guest_memory.cpp
  ${X86RECOMP_DIR}/src/runtime/pe_image.cpp
  ${X86RECOMP_DIR}/src/runtime/xbe_image.cpp
  ${X86RECOMP_DIR}/src/runtime/unwind.cpp
  ${X86RECOMP_DIR}/src/runtime/log.cpp)
target_include_directories(recomp_image PUBLIC ${X86RECOMP_DIR}/src)
target_compile_features(recomp_image PUBLIC cxx_std_20)

include(${X86RECOMP_DIR}/cmake/softfloat.cmake)
add_library(recomp_x87 STATIC ${X86RECOMP_DIR}/src/runtime/x87.cpp
                              ${X86RECOMP_DIR}/src/runtime/sse.cpp)
target_include_directories(recomp_x87 PUBLIC ${X86RECOMP_DIR}/src)
target_compile_features(recomp_x87 PUBLIC cxx_std_20)
target_link_libraries(recomp_x87 PUBLIC softfloat recomp_image)
target_compile_options(recomp_x87 PRIVATE -O2 -fno-stack-protector
                       ${RECOMP_X87_FP_FLAGS} ${RECOMP_FAULT_FRAME_FLAGS})
target_compile_options(softfloat PRIVATE -O2)

add_subdirectory(${X86RECOMP_DIR}/tools/lifter ${CMAKE_BINARY_DIR}/x86recomp_lifter)
