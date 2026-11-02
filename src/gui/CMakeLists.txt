add_library(noteapp_gui
    gui_state.cpp
    icon_font.cpp
    note_list_model.cpp
    save_dialog.cpp
    settings_page.cpp
    view.cpp)
# noteapp_embed_fonts pdf::embedded_font'u üretir ve noteapp_pdf'te zaten tanımlıdır; ikon fontu kendi işlevini alır ki
# `note` (noteapp_pdf'e bağlı) bu veriyi taşımasın.
noteapp_embed_files(noteapp_gui gui/icon_font.hpp noteapp::gui embedded_icon_font fonts/NoteappIcons.ttf)
target_include_directories(noteapp_gui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_gui
    PUBLIC noteapp_core noteapp_search noteapp_sync imgui
    PRIVATE noteapp_detect noteapp_pdf tomlplusplus::tomlplusplus)
# gui-state.toml'un ayrıştırma hatası parse_result ile döner (config.cpp ile aynı istisnasız mod).
target_compile_definitions(noteapp_gui PRIVATE TOML_EXCEPTIONS=0)
noteapp_target_defaults(noteapp_gui)

add_executable(notes-gui WIN32 main.cpp)
target_link_libraries(notes-gui PRIVATE noteapp_gui)
noteapp_target_defaults(notes-gui)

install(TARGETS notes-gui RUNTIME DESTINATION bin)
