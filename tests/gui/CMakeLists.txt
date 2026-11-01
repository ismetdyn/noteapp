noteapp_add_test(gui
    content_search_test.cpp
    folders_test.cpp
    layout_test.cpp
    main_window_test.cpp
    note_list_model_test.cpp
    pdf_test.cpp
    save_test.cpp
    settings_test.cpp
    toolbar_test.cpp
    trash_test.cpp)
target_link_libraries(gui_tests PRIVATE noteapp_detect)
# Ayarlar testleri gerçek `note` ikilisiyle arka plan senkronunu uçtan uca koşar.
add_dependencies(gui_tests note)
target_compile_definitions(gui_tests PRIVATE NOTEAPP_NOTE_EXE="$<TARGET_FILE:note>")
# Başsız çizim: pencere dummy sürücüde, çizim yazılımsal (plan: Başsız test). Xvfb gerekmez.
set_property(TEST gui APPEND PROPERTY ENVIRONMENT SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software)

# Yerleşim sahnelerini PNG'ye döker; gözle inceleme aracıdır, ctest'e kayıtlı değildir (kullanım: layout_shots.cpp).
add_executable(gui_layout_shots layout_shots.cpp)
target_link_libraries(gui_layout_shots PRIVATE doctest::doctest_with_main noteapp_gui)
target_include_directories(gui_layout_shots PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests)
noteapp_target_defaults(gui_layout_shots)
