add_executable(test_monitor main.cpp)

target_link_libraries(test_monitor PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation")

target_compile_options(test_monitor PRIVATE -Wall -Wextra)
