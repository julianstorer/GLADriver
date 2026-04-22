add_executable(test_ring_buffer GLA_test_ring_buffer.cpp)
target_include_directories(test_ring_buffer PRIVATE ${CMAKE_SOURCE_DIR}/common)
add_test(NAME ring_buffer COMMAND test_ring_buffer)

add_executable(test_ipc_protocol GLA_test_ipc_protocol.cpp)
target_include_directories(test_ipc_protocol PRIVATE ${CMAKE_SOURCE_DIR}/common)
add_test(NAME ipc_protocol COMMAND test_ipc_protocol)
