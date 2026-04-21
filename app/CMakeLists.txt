juce_add_gui_app(gla_app
    PRODUCT_NAME "GLA Injector"
    BUNDLE_ID    "com.greenlight.gla-app"
    COMPANY_NAME "GreenLight Audio"
    VERSION      "0.1.0")

target_sources(gla_app PRIVATE
    Main.cpp
    MainComponent.cpp
    AppBackend.cpp
    avdecc_controller.cpp
    usb_bridge_monitor.cpp
    gla_ipc_server.cpp)

target_include_directories(gla_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(gla_app PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:gla_app,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:gla_app,JUCE_VERSION>")

target_link_libraries(gla_app PRIVATE
    gla_common
    la_avdecc_controller_static
    juce::juce_gui_basics
    juce::juce_events
    juce::juce_core
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework AudioToolbox")

target_compile_options(gla_app PRIVATE -Wall -Wextra)
