noteapp_add_test(tui
    tui_state_test.cpp
    editor_buffer_test.cpp
    editor_session_test.cpp
    editor_view_test.cpp
    syntax_test.cpp
    layout_mouse_test.cpp
    render_test.cpp
    clipboard_test.cpp
    completion_test.cpp
    list_ui_test.cpp
    list_nav_test.cpp
    save_dialog_test.cpp
    modals_test.cpp
    settings_test.cpp
    settings_sync_test.cpp
    sync_indicator_test.cpp)

# e2e_editor.sh notlara ayrı süreçten dokunur: depo tek dosyadır, dosya sisteminden yazılamaz.
add_executable(e2e_note_tool e2e_note_tool.cpp)
target_link_libraries(e2e_note_tool PRIVATE noteapp_core)
target_include_directories(e2e_note_tool PRIVATE ${PROJECT_SOURCE_DIR}/src)
noteapp_target_defaults(e2e_note_tool)
