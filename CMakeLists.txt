cmake_minimum_required(VERSION 3.29)
project(GLAInjector C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "" FORCE)

# libASPL
add_subdirectory(third_party/libASPL)

# L-Acoustics avdecc
set(BUILD_AVDECC_CONTROLLER ON CACHE BOOL "" FORCE)
set(BUILD_AVDECC_INTERFACE_MAC ON CACHE BOOL "" FORCE)
set(BUILD_AVDECC_INTERFACE_PCAP OFF CACHE BOOL "" FORCE)
set(BUILD_AVDECC_LIB_SHARED_CXX OFF CACHE BOOL "" FORCE)
set(BUILD_AVDECC_LIB_STATIC_RT_SHARED ON CACHE BOOL "" FORCE)
set(BUILD_AVDECC_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_AVDECC_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_AVDECC_FEATURE_JSON OFF CACHE BOOL "" FORCE)
set(BUILD_AVDECC_DOC OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/avdecc)

# JUCE (for companion app)
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

enable_testing()

add_subdirectory(common)
add_subdirectory(driver)
add_subdirectory(app)
add_subdirectory(tests)
