add_library(gla_driver MODULE
    entry_point.cpp)

set_target_properties(gla_driver PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "driver"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    OUTPUT_NAME "GLAInjector"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

target_include_directories(gla_driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gla_driver PRIVATE
    libASPL
    gla_common
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework AudioToolbox")
target_link_options(gla_driver PRIVATE
    "-Wl,-exported_symbol,_GLAInjector_Create")
target_compile_options(gla_driver PRIVATE -Wall -Wextra)

# Install target (requires sudo)
install(DIRECTORY ${CMAKE_BINARY_DIR}/output/GLAInjector.driver
    DESTINATION /Library/Audio/Plug-Ins/HAL)
