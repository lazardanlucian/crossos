# cmake/toolchain-mingw64.cmake
#
# CMake cross-compilation toolchain for targeting 64-bit Windows from Linux
# using the mingw-w64 toolchain.
#
# Usage:
#   cmake -B build-win \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win

set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_VERSION 10)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Prefer x86_64-w64-mingw32 binaries; override via environment if needed.
set(TOOLCHAIN_PREFIX "x86_64-w64-mingw32")

find_program(CMAKE_C_COMPILER   NAMES ${TOOLCHAIN_PREFIX}-gcc-posix
                                       ${TOOLCHAIN_PREFIX}-gcc
             REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${TOOLCHAIN_PREFIX}-g++-posix
                                       ${TOOLCHAIN_PREFIX}-g++
             REQUIRED)
find_program(CMAKE_RC_COMPILER  NAMES ${TOOLCHAIN_PREFIX}-windres REQUIRED)

set(CMAKE_FIND_ROOT_PATH
    /usr/${TOOLCHAIN_PREFIX}
    /usr/lib/gcc/${TOOLCHAIN_PREFIX}
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Ensure the correct subsystem for GUI / console builds
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc")
