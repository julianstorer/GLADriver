add_executable(gla_daemon
    main.cpp
    avdecc_controller.cpp
    usb_bridge_monitor.cpp
    gla_ipc_server.cpp)

target_include_directories(gla_daemon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gla_daemon PRIVATE
    gla_common
    la_avdecc_controller_static
    "-framework CoreAudio"
    "-framework CoreFoundation")
target_compile_options(gla_daemon PRIVATE -Wall -Wextra)

install(TARGETS gla_daemon DESTINATION /usr/local/bin)
install(FILES plist/com.greenlight.gla-daemon.plist
    DESTINATION /Library/LaunchAgents)
