noteapp_add_test(cli cli_add_test.cpp cli_editor_test.cpp)

# Testler gerçek `note` ikilisini alt süreçte çalıştırır; editör akışı için EDITOR = fake_editor.
add_dependencies(cli_tests note fake_editor)
target_compile_definitions(cli_tests PRIVATE NOTEAPP_NOTE_EXE="$<TARGET_FILE:note>"
                                             NOTEAPP_FAKE_EDITOR="$<TARGET_FILE:fake_editor>")
target_link_libraries(cli_tests PRIVATE noteapp_core)
