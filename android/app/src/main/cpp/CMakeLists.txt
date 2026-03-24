cmake_minimum_required(VERSION 3.22)
project(crossos_android_hello LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CROSSOS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROSSOS_BUILD_TESTS OFF CACHE BOOL "" FORCE)

if(DEFINED CROSSOS_ANDROID_APP)
    message(STATUS "Configuring Android flavor: ${CROSSOS_ANDROID_APP}")
endif()

# Path from android/app/src/main/cpp -> repository root
set(CROSSOS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../)
add_subdirectory(${CROSSOS_ROOT} ${CMAKE_BINARY_DIR}/crossos-build)

add_library(crossos_disc_burner SHARED
    android_main.c
    ${CROSSOS_ROOT}/examples/disc_burner/iso_image.c
    ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

target_include_directories(crossos_disc_burner PRIVATE
    ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue
    ${CROSSOS_ROOT}/examples/disc_burner
)

target_link_libraries(crossos_disc_burner PRIVATE
    crossos
    android
    log
)

add_library(crossos_hello_world SHARED
    android_hello_main.c
    ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

target_include_directories(crossos_hello_world PRIVATE
    ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue
)

target_link_libraries(crossos_hello_world PRIVATE
    crossos
    android
    log
)

add_library(crossos_film_scanner SHARED
    android_scanner_main.c
    ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

target_include_directories(crossos_film_scanner PRIVATE
    ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue
)

target_link_libraries(crossos_film_scanner PRIVATE
    crossos
    android
    log
)
