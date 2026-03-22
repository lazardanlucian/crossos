add_executable(hello_world main.c)

target_link_libraries(hello_world PRIVATE crossos)

# Copy the binary to the build root for convenience
set_target_properties(hello_world PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
