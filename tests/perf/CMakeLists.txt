# T35: SPEC.md §10/2–3 ve modül perf kabulleri. Yalnız Release'te eklenir (tests/CMakeLists.txt).
noteapp_add_test(perf perf_test.cpp)
target_link_libraries(perf_tests PRIVATE noteapp_tui)
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
endif()
