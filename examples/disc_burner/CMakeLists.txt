add_executable(disc_burner main.c)

target_link_libraries(disc_burner PRIVATE crossos)

set_target_properties(disc_burner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
