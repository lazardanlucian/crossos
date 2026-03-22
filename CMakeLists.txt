cmake_minimum_required(VERSION 3.21)

project(crossos
    VERSION   0.1.0
    LANGUAGES C
    DESCRIPTION "Cross-platform display driver / input SDK"
)

# ── C standard ──────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Options ──────────────────────────────────────────────────────────────
option(CROSSOS_BUILD_EXAMPLES "Build example applications" ON)
option(CROSSOS_BUILD_TESTS    "Build the test suite"       ON)

# ── Source files ─────────────────────────────────────────────────────────
set(CROSSOS_SOURCES
    src/core/init.c
    src/core/file.c
    src/core/dialog.c
    src/core/web.c
    src/core/audio.c
    src/core/draw.c
    src/core/ui.c
    src/core/optical.c
    src/core/web_backend_fallback.c
)

if(WIN32)
    list(APPEND CROSSOS_SOURCES
        src/platform/windows/window_win32.c
        src/platform/windows/input_win32.c
    )
elseif(ANDROID)
    list(APPEND CROSSOS_SOURCES
        src/platform/android/window_android.c
        src/platform/android/input_android.c
    )
elseif(UNIX)
    list(APPEND CROSSOS_SOURCES
        src/platform/linux/window_x11.c
        src/platform/linux/input_x11.c
    )
endif()

# ── Library target ────────────────────────────────────────────────────────
add_library(crossos STATIC ${CROSSOS_SOURCES})

target_include_directories(crossos
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# ── Platform-specific dependencies ───────────────────────────────────────
if(WIN32)
    target_link_libraries(crossos PUBLIC user32 gdi32)
    target_link_libraries(crossos PUBLIC winmm)

elseif(ANDROID)
    find_library(ANDROID_LIB      android)
    find_library(LOG_LIB          log)

    target_include_directories(crossos PRIVATE
        ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue
    )
    target_link_libraries(crossos PUBLIC ${ANDROID_LIB} ${LOG_LIB})

elseif(UNIX)
    find_package(X11 REQUIRED)

    if(NOT X11_FOUND)
        message(FATAL_ERROR
            "X11 not found. Install libx11-dev and libxi-dev, then retry.")
    endif()

    target_include_directories(crossos PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(crossos PUBLIC ${X11_LIBRARIES})

    # XInput2 (multi-touch)
    if(X11_Xi_FOUND)
        target_include_directories(crossos PRIVATE ${X11_Xi_INCLUDE_PATH})
        target_link_libraries(crossos PUBLIC ${X11_Xi_LIB})
        target_compile_definitions(crossos PRIVATE HAVE_XINPUT2=1)
    else()
        message(WARNING
            "XInput2 not found (libxi-dev). Touch support will be limited.")
    endif()

    # Xrandr (multi-monitor)
    if(X11_Xrandr_FOUND)
        target_link_libraries(crossos PUBLIC ${X11_Xrandr_LIB})
    endif()

    # ALSA PCM streaming
    find_library(ALSA_LIB asound)
    if(ALSA_LIB)
        target_link_libraries(crossos PUBLIC ${ALSA_LIB})
        target_compile_definitions(crossos PRIVATE CROSSOS_HAS_ALSA=1)
    else()
        message(WARNING "libasound not found. PCM audio streaming unavailable on Linux.")
    endif()

    # pthreads (used by ALSA streaming thread)
    find_package(Threads REQUIRED)
    target_link_libraries(crossos PUBLIC Threads::Threads)
endif()

# Optional HTTP backend
find_package(CURL QUIET)
if(CURL_FOUND)
    target_sources(crossos PRIVATE src/core/web_backend_curl.c)
    target_link_libraries(crossos PUBLIC CURL::libcurl)
    target_compile_definitions(crossos PRIVATE CROSSOS_HAS_CURL=1)
else()
    message(WARNING "libcurl not found. crossos_http_* supports only plain http:// URLs on non-Windows without libcurl.")
endif()

# ── Compiler warnings ────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(crossos PRIVATE /W4 /WX)
else()
    target_compile_options(crossos PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter   # common in platform stubs
    )
endif()

# ── Sub-directories ───────────────────────────────────────────────────────
if(CROSSOS_BUILD_EXAMPLES)
    add_subdirectory(examples/hello_world)
    add_subdirectory(examples/disc_burner)
endif()

if(CROSSOS_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install rules ─────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS crossos
    EXPORT  crossosTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/crossos
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT crossosTargets
    FILE      crossosTargets.cmake
    NAMESPACE crossos::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/crossos
)
