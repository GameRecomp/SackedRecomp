# SPDX-License-Identifier: MIT
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/rc_version.h" @ONLY)

# TropicoRecomp's runtime (shared with it) and SHA-256 for picking the lifted builds by hash.
add_library(rc_runtime STATIC
    src/sha256.c
    src/no_lifted.c
    src/mem.c
    src/log.c
    src/dispatch.c
    src/x87.c
    src/imports.c
    src/vm.c
    src/hostfs.c
    src/process.c
    src/clock.c
    src/resource.c
    src/thread.c
    src/native.c
    src/web.c
    src/fault.c
    src/codepage.c
)
target_include_directories(rc_runtime PUBLIC include)

# This project's game shell (rc_game_main): finds the install, picks the lifted builds by
# hash, starts the platform and the HLE layer and runs the program.
add_library(rc_shell STATIC src/game.c)
target_include_directories(rc_shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(rc_shell PUBLIC rc_runtime rc_hle rc_platform)
if(EMSCRIPTEN)
    # The runtime's JavaScript (rc/web.h).
    target_link_options(rc_runtime INTERFACE "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/web/library_rc.js")
elseif(NOT WIN32)
    # Guest threads run on POSIX threads on macOS (see src/thread.c).
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(rc_runtime PUBLIC Threads::Threads)
endif()
option(RC_THREADS_PTHREAD "Run guest threads on POSIX threads also where ucontext is available" OFF)
if(RC_THREADS_PTHREAD)
    target_compile_definitions(rc_runtime PRIVATE RC_THREADS_PTHREAD)
endif()
if(NOT MSVC)
    target_link_libraries(rc_runtime PUBLIC m)
endif()

if(RC_BUILD_TESTS)
    add_executable(rc_runtime_tests tests/test_main.c tests/test_mem.c tests/test_cpu.c tests/test_x87.c tests/test_vm.c tests/test_thread.c tests/test_load.c tests/test_unwind.c)
    target_link_libraries(rc_runtime_tests PRIVATE rc_runtime)
    add_test(NAME runtime COMMAND rc_runtime_tests)
endif()
