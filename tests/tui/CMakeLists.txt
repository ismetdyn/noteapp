noteapp_add_test(tui
    tui_state_test.cpp
    editor_buffer_test.cpp
    editor_session_test.cpp
    editor_view_test.cpp
    frame_diff_test.cpp
    syntax_test.cpp
    layout_mouse_test.cpp
    render_test.cpp
    clipboard_test.cpp
    completion_test.cpp
    input_line_test.cpp
    list_ui_test.cpp
    list_nav_test.cpp
    list_batch_test.cpp
    list_tree_test.cpp
    folder_nav_test.cpp
    markdown_view_test.cpp
    save_dialog_test.cpp
    modals_test.cpp
    settings_test.cpp
    settings_sync_test.cpp
    sync_indicator_test.cpp)

# Gerçek terminal olmadan gerçek `notes`: forkpty yalnız Linux'ta sınanır (bkz. e2e_editor.sh).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(tui_pty_tests pty_bytes_test.cpp)
    target_link_libraries(tui_pty_tests PRIVATE doctest::doctest_with_main noteapp_core)
    target_include_directories(tui_pty_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
    target_compile_definitions(tui_pty_tests PRIVATE NOTEAPP_NOTES_EXE="$<TARGET_FILE:notes>")
    noteapp_target_defaults(tui_pty_tests)
    add_dependencies(tui_pty_tests notes)
    add_test(NAME tui_pty COMMAND tui_pty_tests)
endif()

# e2e_editor.sh notlara ayrı süreçten dokunur: depo tek dosyadır, dosya sisteminden yazılamaz.
add_executable(e2e_note_tool e2e_note_tool.cpp)
target_link_libraries(e2e_note_tool PRIVATE noteapp_core)
target_include_directories(e2e_note_tool PRIVATE ${PROJECT_SOURCE_DIR}/src)
noteapp_target_defaults(e2e_note_tool)
