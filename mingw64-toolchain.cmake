# mingw64-toolchain.cmake — Cross-compile for Win64 from Linux

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER /opt/msys2-cross/bin/x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER /opt/msys2-cross/bin/x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER /opt/msys2-cross/bin/x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /opt/msys2-cross/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
