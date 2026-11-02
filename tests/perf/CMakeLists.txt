# T35: SPEC.md §10/2–3 ve modül perf kabulleri. Yalnız Release'te koşar; öbür yapılarda derlenir, çürümesin.
set(perf_sources perf_test.cpp nav_perf_test.cpp editor_latency_test.cpp query_perf_test.cpp graph_perf_test.cpp note_store_perf_test.cpp graph_sync_perf_test.cpp)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    add_library(perf_compile_check OBJECT ${perf_sources} tui_startup_probe.cpp)
    target_link_libraries(perf_compile_check PRIVATE doctest::doctest_with_main noteapp_tui noteapp_graph)
    target_include_directories(perf_compile_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests)
    noteapp_target_defaults(perf_compile_check)
    target_compile_definitions(perf_compile_check PRIVATE
        NOTEAPP_NOTE_EXE="" NOTEAPP_TUI_STARTUP_PROBE="" NOTEAPP_DETECT_SAMPLES_DIR="")
    if(NOTEAPP_BUILD_GUI)
        target_link_libraries(perf_compile_check PRIVATE noteapp_gui)
        target_compile_definitions(perf_compile_check PRIVATE NOTEAPP_PERF_GUI)
    endif()
    return()
endif()
noteapp_add_test(perf ${perf_sources})
target_link_libraries(perf_tests PRIVATE noteapp_tui noteapp_graph)
# Gürültüyü azaltmak için başka testlerle aynı anda koşmaz.
set_tests_properties(perf PROPERTIES RUN_SERIAL TRUE)

# `notes` terminal ister: açılışı taze süreçte koşan vekil ölçülür. Hızlı ekleme gerçek `note` ikilisiyle.
add_executable(tui_startup_probe tui_startup_probe.cpp)
target_link_libraries(tui_startup_probe PRIVATE noteapp_tui)
noteapp_target_defaults(tui_startup_probe)
add_dependencies(perf_tests tui_startup_probe note)
target_compile_definitions(perf_tests PRIVATE
    NOTEAPP_NOTE_EXE="$<TARGET_FILE:note>"
    NOTEAPP_TUI_STARTUP_PROBE="$<TARGET_FILE:tui_startup_probe>"
    NOTEAPP_DETECT_SAMPLES_DIR="${PROJECT_SOURCE_DIR}/tests/detect/samples")

if(NOTEAPP_BUILD_GUI)
    target_link_libraries(perf_tests PRIVATE noteapp_gui)
    target_compile_definitions(perf_tests PRIVATE NOTEAPP_PERF_GUI)
    # 1 MB not ölçümü gerçek çizim katmanını başsız koşar (plan: Başsız test).
    set_property(TEST perf APPEND PROPERTY ENVIRONMENT SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software)
endif()
