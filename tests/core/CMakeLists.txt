noteapp_add_test(core
    config_test.cpp
    editor_test.cpp
    i18n_test.cpp
    log_test.cpp
    note_path_test.cpp
    note_store_ops_test.cpp
    note_store_test.cpp
    process_test.cpp
    smoke_test.cpp
    text_test.cpp)

add_dependencies(core_tests fake_editor)
target_compile_definitions(core_tests PRIVATE NOTEAPP_FAKE_EDITOR="$<TARGET_FILE:fake_editor>")

# Katalog eşliği testi gömülü TOML'u kendisi ayrıştırır.
target_link_libraries(core_tests PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(core_tests PRIVATE TOML_EXCEPTIONS=0)
