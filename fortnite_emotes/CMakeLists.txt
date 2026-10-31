cmake_minimum_required(VERSION 3.21)
project(fortnite_emotes LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only (Win32 + Direct3D 11).")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

file(GLOB_RECURSE EMOTES_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

add_library(emotes SHARED ${EMOTES_SOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${EMOTES_SOURCES})

target_include_directories(emotes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(emotes PRIVATE emotes_imgui emotes_minhook winmm windowscodecs ole32 shell32)
target_compile_definitions(emotes PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

if(MSVC)
    target_compile_options(emotes PRIVATE /utf-8 /W4 /permissive- /MP /wd4100)
    set_property(TARGET emotes PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(emotes PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)
