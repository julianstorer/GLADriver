add_library(gla_common STATIC
    gla_socket.cpp)

target_include_directories(gla_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(gla_common PRIVATE -Wall -Wextra)
