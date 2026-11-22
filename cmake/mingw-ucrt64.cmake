# SPDX-License-Identifier: MIT
# Cross-compiling for 64-bit Windows with MinGW-w64 on the UCRT, the C library MSVC builds
# use too (tools/check-windows).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32ucrt-gcc)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32ucrt-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32ucrt)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
