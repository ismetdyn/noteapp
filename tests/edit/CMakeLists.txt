noteapp_add_test(edit
    editor_buffer_test.cpp
    editor_modes_test.cpp
    editor_session_test.cpp
    syntax_test.cpp
    child_process_test.cpp)

# ChildProcess testleri borulu çocuk olarak fake_editor'ı (yankı/taşkın/uyuyan/çıkan kipler) başlatır.
add_dependencies(edit_tests fake_editor)
target_compile_definitions(edit_tests PRIVATE NOTEAPP_FAKE_EDITOR="$<TARGET_FILE:fake_editor>")
