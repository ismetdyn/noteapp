cmake_minimum_required(VERSION 3.28)
project(noteapp VERSION 0.1.0 LANGUAGES CXX)

option(NOTEAPP_BUILD_GUI "notes-gui (Qt 6) derlensin" ON)
option(NOTEAPP_BUILD_TESTS "Testler derlensin" ON)

include(cmake/Dependencies.cmake)

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

if(NOTEAPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
