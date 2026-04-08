find_package(Threads REQUIRED)

option(EASYRAW_ENABLE_LIBRAW "Enable LibRaw support for RAF decoding in easy_raw" ON)

if(EASYRAW_ENABLE_LIBRAW)
    find_path(LIBRAW_INCLUDE_DIR libraw/libraw.h REQUIRED)
    find_library(LIBRAW_LIBRARY NAMES raw libraw libraw_r REQUIRED)
endif()

add_executable(easy_raw
    raw_decode.c
    raw_process.c
    main.c
)

target_link_libraries(easy_raw PRIVATE crossos m Threads::Threads)

if(EASYRAW_ENABLE_LIBRAW)
    target_include_directories(easy_raw PRIVATE ${LIBRAW_INCLUDE_DIR})
    target_link_libraries(easy_raw PRIVATE ${LIBRAW_LIBRARY})
    target_compile_definitions(easy_raw PRIVATE EASYRAW_HAS_LIBRAW=1)
    message(STATUS "easy_raw: LibRaw enabled (${LIBRAW_LIBRARY})")
else()
    message(STATUS "easy_raw: LibRaw disabled, using dcraw fallback for RAF")
endif()

set_target_properties(easy_raw PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
