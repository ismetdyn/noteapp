noteapp_add_test(core
    config_test.cpp
    editor_test.cpp
    file_icons_test.cpp
    i18n_test.cpp
    json_test.cpp
    log_test.cpp
    lsp_protocol_test.cpp
    note_path_test.cpp
    note_store_ops_test.cpp
    note_store_test.cpp
    process_test.cpp
    smoke_test.cpp
    text_test.cpp
    trash_test.cpp)

add_dependencies(core_tests fake_editor)
target_compile_definitions(core_tests PRIVATE NOTEAPP_FAKE_EDITOR="$<TARGET_FILE:fake_editor>")

# Katalog eşliği testi gömülü TOML'u kendisi ayrıştırır.
target_link_libraries(core_tests PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(core_tests PRIVATE TOML_EXCEPTIONS=0)

# Graph üzerindeki NoteStore: core_tests'ten ayrı, motora doğrudan bakar.
noteapp_add_test(note_graph note_graph_test.cpp note_limits_test.cpp note_links_test.cpp note_relations_test.cpp)
target_link_libraries(note_graph_tests PRIVATE noteapp_core)

# Etiketler (T80): iki cihazın birleşimi gerçek üç yönlü birleştirmeyle sınanır.
target_sources(note_graph_tests PRIVATE note_tags_test.cpp note_views_test.cpp note_budget_test.cpp)
target_link_libraries(note_graph_tests PRIVATE noteapp_sync)
