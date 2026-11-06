noteapp_add_test(cli cli_add_test.cpp cli_editor_test.cpp cli_manage_test.cpp cli_browse_test.cpp cli_search_test.cpp cli_config_test.cpp
    cli_sync_test.cpp cli_export_test.cpp cli_query_test.cpp cli_graph_test.cpp cli_tags_test.cpp cli_view_test.cpp cli_import_test.cpp
    cli_completion_test.cpp cli_term_test.cpp cli_suggest_test.cpp cli_help_test.cpp cli_flags_test.cpp cli_output_test.cpp
    cli_short_flags_test.cpp cli_relation_test.cpp)

# Testler gerçek `note` ikilisini alt süreçte çalıştırır; editör akışı için EDITOR = fake_editor.
add_dependencies(cli_tests note fake_editor)
target_compile_definitions(cli_tests PRIVATE NOTEAPP_NOTE_EXE="$<TARGET_FILE:note>"
                                             NOTEAPP_FAKE_EDITOR="$<TARGET_FILE:fake_editor>")
# Senkron testleri bare uzak depoyu libgit2 ile kurar ve kasayı sorgular.
target_link_libraries(cli_tests PRIVATE noteapp_core noteapp_sync noteapp_graph)
# confirm.hpp gibi yalnız-başlık cli yardımcıları ve saf ayrıştırıcı (args.cpp, G/Ç yok) doğrudan birim test edilir.
target_include_directories(cli_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_sources(cli_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/cli/args.cpp)

# Üretilen tamamlama betikleri kabukların ayrıştırıcısından geçer, bash (ve kuruluysa zsh) gerçekten tamamlar.
if(UNIX)
    add_test(NAME cli_completion_shell
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/completion_shell_test.sh $<TARGET_FILE:note>)
endif()

# scripts/install-local.sh geçici HOME'da sınanır.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME install_local COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/install_local_test.sh ${PROJECT_SOURCE_DIR})
endif()
