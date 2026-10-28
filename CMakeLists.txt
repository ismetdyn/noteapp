cmake_minimum_required(VERSION 3.28)
project(noteapp VERSION 0.1.0 LANGUAGES CXX)

option(NOTEAPP_BUILD_GUI "notes-gui (Qt 6) derlensin" ON)
option(NOTEAPP_BUILD_TESTS "Testler derlensin" ON)

# C++20 modülleri kullanılmıyor; tarama yalnız derlemeyi yavaşlatır.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(cmake/Dependencies.cmake)
include(cmake/EmbedResources.cmake)

# Dil standardı ve uyarılar yalnız bizim hedeflerimize uygulanır; üçüncü parti kodun uyarıları build'i kırmamalı.
function(noteapp_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

add_subdirectory(src/core)
add_subdirectory(src/detect)
add_subdirectory(src/sync)
add_subdirectory(src/tui)
if(NOTEAPP_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

if(NOTEAPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
