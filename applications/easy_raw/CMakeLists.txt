find_package(Threads REQUIRED)

option(EASYRAW_ENABLE_LIBRAW "Enable LibRaw support for RAF decoding in easy_raw" ON)

if(EASYRAW_ENABLE_LIBRAW)
    # Use pkg-config first — it resolves all transitive deps (libjpeg, liblcms2, OpenMP, stdc++, …).
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND AND NOT CMAKE_CROSSCOMPILING)
        pkg_check_modules(LIBRAW REQUIRED libraw)
    else()
        # Manual detection for cross-compile (MinGW) or when pkg-config is unavailable.
        # The MinGW toolchain adds MINGW_EXTRA_SYSROOT to CMAKE_FIND_ROOT_PATH so headers
        # and libs placed under deps/mingw64/ are found automatically in ONLY mode.
        find_path(LIBRAW_INCLUDE_DIR libraw/libraw.h REQUIRED)
        set(_easyraw_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a .lib)
        find_library(LIBRAW_LIB NAMES raw libraw libraw_r REQUIRED)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${_easyraw_saved_suffixes})
        set(LIBRAW_INCLUDE_DIRS ${LIBRAW_INCLUDE_DIR})

        # Transitive deps for a static LibRaw built without JPEG/LCMS.
        set(LIBRAW_LIBRARIES ${LIBRAW_LIB})
        if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
            # MinGW cross-compile: need C++ runtime, zlib, winsock
            list(APPEND LIBRAW_LIBRARIES stdc++ z ws2_32)
        else()
            list(APPEND LIBRAW_LIBRARIES stdc++)
        endif()
    endif()
endif()

add_executable(easy_raw
    raw_decode.c
    raw_process.c
    main.c
)

target_link_libraries(easy_raw PRIVATE crossos m Threads::Threads)

if(EASYRAW_ENABLE_LIBRAW)
    target_include_directories(easy_raw PRIVATE ${LIBRAW_INCLUDE_DIRS})
    target_link_libraries(easy_raw PRIVATE ${LIBRAW_LIBRARIES})
    target_compile_definitions(easy_raw PRIVATE EASYRAW_HAS_LIBRAW=1)
    message(STATUS "easy_raw: LibRaw enabled (${LIBRAW_LIBRARIES})")
else()
    message(STATUS "easy_raw: LibRaw disabled, using dcraw fallback for RAF")
endif()

set_target_properties(easy_raw PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
