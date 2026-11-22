# SPDX-License-Identifier: MIT
add_library(rc_hle STATIC
    modules.c
    advapi32/advapi32.c
    dinput/dinput.c
    dmusic/dmusic.c
    dmusic/dls.c
    dmusic/player.c
    dsound/dsound.c
    shell32/shell32.c
    wsock/wsock.c
    dshow/dshow.c
    dplay/dplay.c
    dplay/net.c
    dplay/dpnet.c
    kernel32/process.c
    kernel32/memory.c
    kernel32/misc.c
    kernel32/locale.c
    kernel32/file.c
    kernel32/profile.c
    kernel32/sync.c
    kernel32/version.c
    kernel32/ole32.c
    user32/user32.c
    user32/window.c
    user32/input.c
    user32/uidraw.c
    user32/menu.c
    user32/controls.c
    user32/dialog.c
    user32/mdi.c
    comctl32/comctl32.c
    comdlg32/comdlg32.c
    gdi32/gdi32.c
    gdi32/dc.c
    gdi32/text.c
    gdi32/objects.c
    gdi32/draw.c
    gdi32/region.c
    gdi32/palette.c
    ddraw/ddraw.c
    ddraw/d3d.c
    frames.c
    mss32/mss32.c
    mss32/rib.c
    winmm/winmm.c
    winmm/mci.c
    msvcrt/crt.c
    msvcrt/format.c
    msvcrt/stdio.c
    msvcrt/eh.c
    msvcrt/string.c
)
target_include_directories(rc_hle PUBLIC include PRIVATE . "${PROJECT_SOURCE_DIR}/third_party/pl_mpeg")
target_link_libraries(rc_hle PUBLIC rc_runtime rc_platform)
if(WIN32)
    target_link_libraries(rc_hle PRIVATE ws2_32) # dplay: sockets
endif()

if(RC_BUILD_TESTS)
    # MSVCRT's exception handling end to end, on hand-made frames (with kernel32's dispatcher).
    add_executable(rc_hle_tests tests/test_eh.c)
    target_include_directories(rc_hle_tests PRIVATE "${PROJECT_SOURCE_DIR}/runtime/tests")
    target_link_libraries(rc_hle_tests PRIVATE rc_hle)
    add_test(NAME hle COMMAND rc_hle_tests)
endif()
