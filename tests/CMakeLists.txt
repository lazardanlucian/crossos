add_executable(test_types test_types.c)

target_link_libraries(test_types PRIVATE crossos)

add_test(NAME CrossOS_Types COMMAND test_types)
