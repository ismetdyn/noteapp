noteapp_add_test(cli cli_add_test.cpp)

# Testler gerçek `note` ikilisini alt süreçte çalıştırır.
add_dependencies(cli_tests note)
target_compile_definitions(cli_tests PRIVATE NOTEAPP_NOTE_EXE="$<TARGET_FILE:note>")
target_link_libraries(cli_tests PRIVATE noteapp_core)
