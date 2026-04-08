cmake_minimum_required(VERSION 3.21)
project(film_scanner C)

add_executable(film_scanner main.c)
target_link_libraries(film_scanner PRIVATE crossos)

set_target_properties(film_scanner PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
