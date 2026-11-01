# Modül başına tek test ikilisi; ctest adı modül adıdır (ctest -R <modül>).
function(noteapp_add_test module)
    add_executable(${module}_tests ${ARGN})
    target_link_libraries(${module}_tests PRIVATE doctest::doctest_with_main)
    # cli gibi kütüphanesiz modüller yalnız ikiliyi dışarıdan çalıştırır.
    if(TARGET noteapp_${module})
        target_link_libraries(${module}_tests PRIVATE noteapp_${module})
    endif()
    # "support/test_support.hpp" gibi ortak yardımcılar tests/ köküne göre eklenir.
    target_include_directories(${module}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests)
    noteapp_target_defaults(${module}_tests)
    add_test(NAME ${module} COMMAND ${module}_tests)
    # Varsayılan olarak hiçbir test kullanıcının gerçek config/log/state dizinine yazmasın; kendi NOTEAPP_HOME'unu
    # kuran testler bunu süreç içinde ezer.
    set_property(TEST ${module} APPEND PROPERTY ENVIRONMENT "NOTEAPP_HOME=${CMAKE_BINARY_DIR}/test-home")
endfunction()

# Testlerde gerçek editör yerine çalışan yardımcı (core ve cli testleri kullanır).
add_executable(fake_editor support/fake_editor.cpp)
noteapp_target_defaults(fake_editor)
if(WIN32)
    target_link_libraries(fake_editor PRIVATE shell32)
endif()

add_subdirectory(graph)
add_subdirectory(core)
add_subdirectory(detect)
add_subdirectory(search)
add_subdirectory(sync)
add_subdirectory(cli)
add_subdirectory(tui)
add_subdirectory(pdf)
if(NOTEAPP_BUILD_GUI)
    add_subdirectory(gui)
endif()
# Perf paketi (T35) yalnız Release'te: Debug süreleri anlamsız ve günlük ctest hızlı kalmalı.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_subdirectory(perf)
endif()
