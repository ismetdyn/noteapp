noteapp_add_test(core
    config_test.cpp
    editor_test.cpp
    note_path_test.cpp
    note_store_test.cpp
    process_test.cpp
    smoke_test.cpp
    text_test.cpp)

add_dependencies(core_tests fake_editor)
target_compile_definitions(core_tests PRIVATE NOTEAPP_FAKE_EDITOR="$<TARGET_FILE:fake_editor>")
