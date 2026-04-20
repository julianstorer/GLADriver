add_executable(test_ring_buffer test_ring_buffer.cpp)
target_link_libraries(test_ring_buffer PRIVATE gla_common)
target_include_directories(test_ring_buffer PRIVATE
    ${CMAKE_SOURCE_DIR}/common)
add_test(NAME ring_buffer COMMAND test_ring_buffer)

add_executable(test_ipc_protocol test_ipc_protocol.cpp)
target_link_libraries(test_ipc_protocol PRIVATE gla_common)
target_include_directories(test_ipc_protocol PRIVATE
    ${CMAKE_SOURCE_DIR}/common)
add_test(NAME ipc_protocol COMMAND test_ipc_protocol)
