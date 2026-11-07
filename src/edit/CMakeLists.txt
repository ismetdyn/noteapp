# Editör yığını (tampon, sözdizimi, yazım, tamamlama, bul, giriş satırı, pano, çocuk süreç, LSP): TUI ve GUI aynı
# modeli kullanır, yalnız çizim arayüzdedir. FTXUI, ImGui ve SDL bağımlılığı yoktur; arayüz başlığı da eklenmez
# (aşağıdaki denetim).
# Ad alanı tarihsel olarak noteapp::tui kaldı (A8: yalnız taşıma).
add_library(noteapp_edit
    child_process.cpp
    clipboard.cpp
    completion.cpp
    editor_buffer.cpp
    editor_modes.cpp
    editor_session.cpp
    input_line.cpp
    language_servers.cpp
    lsp_client.cpp
    lsp_workspace.cpp
    note_find.cpp
    server_settings.cpp
    spellcheck.cpp
    syntax.cpp)
target_include_directories(noteapp_edit PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_edit PUBLIC noteapp_app noteapp_core noteapp_search)
noteapp_target_defaults(noteapp_edit)

# Sınır: GUI bu kütüphaneyi bağlar; arayüz başlığı sızarsa GUI'ye FTXUI ya da TUI durumu taşınır. Denetim
# yapılandırmada koşar: dosya eklenince yeniden koşar, var olan dosyaya eklenen #include bir sonraki yapılandırmada
# (her temiz derleme, konteyner betikleri) düşer.
file(GLOB edit_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
foreach(file IN LISTS edit_files)
    file(STRINGS ${file} ui_includes REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](ftxui/|imgui|SDL3?/|tui/|gui/)")
    if(ui_includes)
        message(FATAL_ERROR "noteapp_edit must stay UI-free: ${file} has ${ui_includes}")
    endif()
endforeach()
