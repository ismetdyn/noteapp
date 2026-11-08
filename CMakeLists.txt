cmake_minimum_required(VERSION 3.28)
project(noteapp VERSION 0.1.1 LANGUAGES CXX)

option(NOTEAPP_BUILD_GUI "notes-gui (SDL3 + Dear ImGui) derlensin" ON)
option(NOTEAPP_BUILD_TESTS "Testler derlensin" ON)
option(NOTEAPP_BUILD_FUZZ "libFuzzer hedefleri (clang, ASan/UBSan; tests/graph/fuzz)" OFF)

if(NOTEAPP_BUILD_FUZZ)
    # Tüm hedefler aynı sanitizer'la: kütüphanede kaçan taşma hedefte görünsün. Kapsam izleme yalnız motorda.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()

# C++20 modülleri kullanılmıyor; tarama yalnız derlemeyi yavaşlatır.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(cmake/Dependencies.cmake)
include(cmake/EmbedResources.cmake)
if(MINGW)
    include(cmake/toolchains/mingw-delay-load.cmake)
endif()

# Dil standardı ve uyarılar yalnız bizim hedeflerimize uygulanır; üçüncü parti kodun uyarıları build'i kırmamalı.
function(noteapp_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    # libstdc++, libgcc, winpthreads statik: ikili MinGW DLL'i olmadan kopyala-çalıştır açılır.
    if(MINGW)
        target_link_options(${target} PRIVATE -static "-L${NOTEAPP_DELAY_LOAD_DIR}")
    endif()
endfunction()

add_subdirectory(src/graph)
add_subdirectory(src/core)
add_subdirectory(src/detect)
add_subdirectory(src/search)
add_subdirectory(src/sync)
add_subdirectory(src/app)
add_subdirectory(src/pdf)
add_subdirectory(src/cli)
add_subdirectory(src/edit)
add_subdirectory(src/tui)
if(NOTEAPP_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

# Windows: üç exe Explorer'da ve görev çubuğunda aynı uygulama ikonunu taşır (resources/icons,
# scripts/generate-app-icon.sh). .rc mutlak yolla üretilir: windres göreli yolu derleme dizininden çözerdi.
if(WIN32)
    enable_language(RC)
    set(noteapp_icon "${PROJECT_SOURCE_DIR}/resources/icons/noteapp.ico")
    set(noteapp_icon_rc "${PROJECT_BINARY_DIR}/noteapp-icon.rc")
    file(CONFIGURE OUTPUT "${noteapp_icon_rc}" CONTENT "1 ICON \"@noteapp_icon@\"\n" @ONLY)
    foreach(exe IN ITEMS note notes notes-gui)
        if(TARGET ${exe})
            target_sources(${exe} PRIVATE "${noteapp_icon_rc}")
            # .rc'nin içeriği değişmese de .ico değişince yeniden derlensin; kaynak özelliği hedefin dizinine yazılır.
            set_source_files_properties("${noteapp_icon_rc}" TARGET_DIRECTORY ${exe}
                PROPERTIES OBJECT_DEPENDS "${noteapp_icon}")
        endif()
    endforeach()
endif()

if(NOTEAPP_BUILD_FUZZ)
    add_subdirectory(tests/graph/fuzz)
endif()

if(NOTEAPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
