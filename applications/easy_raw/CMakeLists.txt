add_executable(easy_raw
    raw_decode.c
    raw_process.c
    main.c
)

target_link_libraries(easy_raw PRIVATE crossos m)

set_target_properties(easy_raw PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
