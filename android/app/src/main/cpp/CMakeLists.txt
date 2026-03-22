cmake_minimum_required(VERSION 3.22)
project(crossos_android_hello LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CROSSOS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROSSOS_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Path from android/app/src/main/cpp -> repository root
set(CROSSOS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../)
add_subdirectory(${CROSSOS_ROOT} ${CMAKE_BINARY_DIR}/crossos-build)

add_library(crossos_hello SHARED
    android_main.c
    ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

target_include_directories(crossos_hello PRIVATE
    ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue
)

target_link_libraries(crossos_hello PRIVATE
    crossos
    android
    log
)
