noteapp_add_test(cli cli_add_test.cpp cli_editor_test.cpp cli_manage_test.cpp cli_browse_test.cpp cli_search_test.cpp cli_config_test.cpp
    cli_sync_test.cpp cli_export_test.cpp)

# Testler gerçek `note` ikilisini alt süreçte çalıştırır; editör akışı için EDITOR = fake_editor.
add_dependencies(cli_tests note fake_editor)
# export testleri gerçek notes-gui'yi başsız çağırır; GUI derlenmiyorsa yalnız GUI'siz vakalar koşar.
if(NOTEAPP_BUILD_GUI)
    add_dependencies(cli_tests notes-gui)
    # Derleme ağacında note ve notes-gui ayrı dizinlerde; kurulumda yan yanadır (NOTEAPP_GUI_EXE dikişiyle gösterilir).
    target_compile_definitions(cli_tests PRIVATE NOTEAPP_GUI_EXE="$<TARGET_FILE:notes-gui>")
endif()
target_compile_definitions(cli_tests PRIVATE NOTEAPP_NOTE_EXE="$<TARGET_FILE:note>"
                                             NOTEAPP_FAKE_EDITOR="$<TARGET_FILE:fake_editor>")
# Senkron testleri bare uzak depoyu libgit2 ile kurar ve kasayı sorgular.
target_link_libraries(cli_tests PRIVATE noteapp_core noteapp_sync)
# confirm.hpp gibi yalnız-başlık cli yardımcıları doğrudan birim test edilir.
target_include_directories(cli_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
