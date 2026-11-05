noteapp_add_test(gui
    browser_test.cpp
    command_parity_test.cpp
    content_search_test.cpp
    dialogs_test.cpp
    editor_features_test.cpp
    editor_lsp_test.cpp
    editor_widget_test.cpp
    external_copy_test.cpp
    folders_test.cpp
    fonts_test.cpp
    global_search_test.cpp
    gui_args_test.cpp
    icon_font_test.cpp
    import_test.cpp
    info_test.cpp
    layout_test.cpp
    links_test.cpp
    list_commands_test.cpp
    list_prefs_test.cpp
    main_window_test.cpp
    map_test.cpp
    marks_test.cpp
    note_editor_test.cpp
    note_list_model_test.cpp
    palette_test.cpp
    pdf_test.cpp
    preview_layout_test.cpp
    preview_test.cpp
    row_numbers_test.cpp
    save_test.cpp
    search_args_test.cpp
    search_line_test.cpp
    settings_test.cpp
    stray_notice_test.cpp
    tabs_test.cpp
    theme_test.cpp
    toolbar_test.cpp
    trash_test.cpp)
target_link_libraries(gui_tests PRIVATE noteapp_detect)
# Ayarlar testleri gerçek `note` ikilisiyle arka plan senkronunu uçtan uca koşar; dış editör testleri
# editör yerine fake_editor, dil sunucusu testleri gerçek sunucu yerine fake_lsp_server (tests/tui) çalıştırır;
# komut satırı testi gerçek notes-gui ikilisini çalıştırır.
add_dependencies(gui_tests note notes-gui fake_editor fake_lsp_server)
target_compile_definitions(gui_tests PRIVATE NOTEAPP_NOTE_EXE="$<TARGET_FILE:note>"
                                            NOTEAPP_FAKE_EDITOR="$<TARGET_FILE:fake_editor>"
                                            NOTEAPP_FAKE_LSP_SERVER="$<TARGET_FILE:fake_lsp_server>"
                                            NOTEAPP_GUI_EXE="$<TARGET_FILE:notes-gui>")
# Başsız çizim: pencere dummy sürücüde, çizim yazılımsal (plan: Başsız test). Xvfb gerekmez.
set_property(TEST gui APPEND PROPERTY ENVIRONMENT SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software)

# Yerleşim sahnelerini PNG'ye döker; gözle inceleme aracıdır, ctest'e kayıtlı değildir (kullanım: layout_shots.cpp).
add_executable(gui_layout_shots layout_shots.cpp)
target_link_libraries(gui_layout_shots PRIVATE doctest::doctest_with_main noteapp_gui)
target_include_directories(gui_layout_shots PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests)
noteapp_target_defaults(gui_layout_shots)
# G5c LSP sahnesi gerçek sunucu yerine tests/tui/fake_lsp_server kullanır.
add_dependencies(gui_layout_shots fake_lsp_server)
target_compile_definitions(gui_layout_shots PRIVATE NOTEAPP_FAKE_LSP_SERVER="$<TARGET_FILE:fake_lsp_server>")
