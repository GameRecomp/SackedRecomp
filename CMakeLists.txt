# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RC_VERSION)
string(STRIP "${RC_VERSION}" RC_VERSION)
project(sackedrecomp VERSION ${RC_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RC_BUILD_TESTS "Build the unit tests" ON)
option(RC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(RC_WITH_SDL3 AUTO CACHE STRING "Use SDL3 for the platform layer (ON, OFF, AUTO)")
set_property(CACHE RC_WITH_SDL3 PROPERTY STRINGS ON OFF AUTO)

# Short git hash for logs and the window title.
set(RC_GIT_HASH "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                  OUTPUT_VARIABLE RC_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _git_rc)
  if(NOT _git_rc EQUAL 0)
    set(RC_GIT_HASH "unknown")
  endif()
endif()

# Warning flags for every target of this project, the runtime, HLE and platform code shared
# with TropicoRecomp and the generated C included (rc_warnings is kept for targets that name it).
add_library(rc_warnings INTERFACE)
if(MSVC)
  add_compile_options(/W4)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
  if(RC_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
  if(RC_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

if(RC_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(runtime)
add_subdirectory(platform)
add_subdirectory(hle)

include(cmake/lift.cmake)
add_subdirectory(games/sacked)
add_subdirectory(games/crazyschool)
